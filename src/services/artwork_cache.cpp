#include "services/artwork_cache.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {
using Clock = std::chrono::steady_clock;
constexpr unsigned kImageSize = 256;
constexpr std::size_t kPixelBytes = kImageSize * kImageSize * 4U;
constexpr std::size_t kMaximumDownload = 2U * 1024U * 1024U;
constexpr std::size_t kMemoryEntries = 128;
constexpr std::size_t kDiskEntries = 100;
constexpr auto kRetryDelay = std::chrono::minutes(5);
constexpr std::array<std::string_view, 6> kImageExtensions{{".png", ".jpg", ".ico", ".webp", ".bmp", ".gif"}};
constexpr std::string_view kCacheVersion = "RADIO-ART-ORIGINAL-V2";

bool valid_url(const std::string &url)
{
    if (url.empty() || url.size() > 4096U) return false;
    const std::size_t start = url.compare(0, 7, "http://") == 0 ? 7U :
                              url.compare(0, 8, "https://") == 0 ? 8U : 0U;
    if (start == 0U || start == url.size() || url[start] == '/' || url[start] == '?' || url[start] == '#') return false;
    return std::none_of(url.begin(), url.end(), [](unsigned char c) { return c <= 0x20U || c == 0x7fU; });
}

bool write_all(int fd, const void *data, std::size_t length)
{
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    while (length > 0U) {
        const ssize_t count = write(fd, bytes, length);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes += count;
        length -= static_cast<std::size_t>(count);
    }
    return true;
}

class TemporaryFile {
public:
    explicit TemporaryFile(std::string pattern) : path_(std::move(pattern)) {
        fd_ = mkstemp(path_.data());
        if (fd_ >= 0 && fcntl(fd_, F_SETFD, FD_CLOEXEC) < 0) { close(fd_); fd_ = -1; unlink(path_.c_str()); }
    }
    ~TemporaryFile() { if (fd_ >= 0) close(fd_); if (!path_.empty()) unlink(path_.c_str()); }
    int fd() const { return fd_; }
    const std::string &path() const { return path_; }
private:
    std::string path_;
    int fd_ = -1;
};

std::string checksum(std::string_view bytes)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char c : bytes) { hash ^= c; hash *= 1099511628211ULL; }
    std::ostringstream key;
    key << std::hex << std::setfill('0') << std::setw(16) << hash;
    return key.str();
}

std::string_view image_extension(const std::vector<std::uint8_t> &source)
{
    if (source.empty()) return {};
    const std::string_view bytes(reinterpret_cast<const char *>(source.data()), source.size());
    if (bytes.substr(0, 8) == "\x89PNG\r\n\x1a\n") return ".png";
    if (bytes.substr(0, 3) == "\xff\xd8\xff") return ".jpg";
    if (bytes.substr(0, 4) == std::string_view("\0\0\1\0", 4)) return ".ico";
    if (bytes.size() >= 12 && bytes.substr(0, 4) == "RIFF" && bytes.substr(8, 4) == "WEBP") return ".webp";
    if (bytes.substr(0, 2) == "BM") return ".bmp";
    if (bytes.substr(0, 6) == "GIF87a" || bytes.substr(0, 6) == "GIF89a") return ".gif";
    return {};
}

std::vector<std::uint8_t> read_cache(const std::string &directory, const std::string &url)
{
    if (directory.empty()) return {};
    const auto base = std::filesystem::path(directory) / checksum(url);
    const auto metadata_path = base.string() + ".meta";
    std::error_code error;
    if (std::filesystem::file_size(metadata_path, error) > 8192 || error) return {};
    const auto modified = std::filesystem::last_write_time(metadata_path, error);
    if (error || std::filesystem::file_time_type::clock::now() - modified > std::chrono::hours(24 * 7)) return {};
    std::ifstream metadata(metadata_path, std::ios::binary);
    std::string version, stored_url, extension, stored_checksum;
    std::size_t length = 0;
    if (!std::getline(metadata, version) || version != kCacheVersion ||
        !std::getline(metadata, stored_url) || stored_url != url || !std::getline(metadata, extension) ||
        std::find(kImageExtensions.begin(), kImageExtensions.end(), extension) == kImageExtensions.end() ||
        !(metadata >> length >> stored_checksum) || length == 0 || length > kMaximumDownload) return {};
    metadata >> std::ws;
    if (metadata.peek() != std::char_traits<char>::eof()) return {};

    const auto image_path = base.string() + extension;
    if (std::filesystem::file_size(image_path, error) != length || error) return {};
    std::ifstream stream(image_path, std::ios::binary);
    std::vector<std::uint8_t> source(length);
    stream.read(reinterpret_cast<char *>(source.data()), static_cast<std::streamsize>(source.size()));
    if (!stream || stream.peek() != std::char_traits<char>::eof()) return {};
    // Sidecar integrity also detects interrupted writes of the image/metadata pair.
    if (image_extension(source) != extension ||
        checksum({reinterpret_cast<const char *>(source.data()), source.size()}) != stored_checksum) return {};
    return source;
}

bool owned_key(const std::filesystem::path &path)
{
    const auto stem = path.stem().string();
    return stem.size() == 16 && std::all_of(stem.begin(), stem.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

void remove_cache_entry(const std::filesystem::path &metadata_path)
{
    std::error_code error;
    const auto base = metadata_path.parent_path() / metadata_path.stem();
    std::filesystem::remove(metadata_path, error);
    for (const auto extension : kImageExtensions)
        std::filesystem::remove(base.string() + std::string(extension), error);
}

void prune_cache(const std::string &directory)
{
    std::error_code error;
    using CachedFile = std::pair<std::filesystem::file_time_type, std::filesystem::path>;
    std::vector<CachedFile> files;
    for (std::filesystem::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) {
        if (!owned_key(it->path()) || !it->is_regular_file(error) || error) continue;
        if (it->path().extension() == ".radio-art") {
            std::filesystem::remove(it->path(), error);
            continue;
        }
        if (it->path().extension() != ".meta") continue;
        const auto time = it->last_write_time(error);
        if (!error) files.emplace_back(time, it->path());
    }
    if (files.size() <= kDiskEntries) return;
    std::sort(files.begin(), files.end());
    for (std::size_t i = 0; i < files.size() - kDiskEntries; ++i) remove_cache_entry(files[i].second);
}

void write_cache(const std::string &directory, const std::string &url, const std::vector<std::uint8_t> &source)
{
    const auto extension = image_extension(source);
    if (directory.empty() || extension.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return;
    const auto base = (std::filesystem::path(directory) / checksum(url)).string();
    TemporaryFile image_file((std::filesystem::path(directory) / ".radio-art-XXXXXX").string());
    TemporaryFile metadata_file((std::filesystem::path(directory) / ".radio-art-XXXXXX").string());
    const auto body_checksum = checksum({reinterpret_cast<const char *>(source.data()), source.size()});
    std::ostringstream metadata;
    metadata << kCacheVersion << '\n' << url << '\n' << extension << '\n' << source.size() << '\n' << body_checksum << '\n';
    const auto record = metadata.str();
    if (image_file.fd() < 0 || metadata_file.fd() < 0 ||
        !write_all(image_file.fd(), source.data(), source.size()) ||
        !write_all(metadata_file.fd(), record.data(), record.size())) return;
    std::filesystem::rename(image_file.path(), base + std::string(extension), error);
    if (error) return;
    std::filesystem::rename(metadata_file.path(), base + ".meta", error);
    if (error) return;
    for (const auto previous_extension : kImageExtensions) {
        if (previous_extension != extension)
            std::filesystem::remove(base + std::string(previous_extension), error);
    }
    prune_cache(directory);
}
}  // namespace

struct ArtworkCache::Impl {
    enum class State { Pending, Ready, Failed };
    struct Entry { State state = State::Pending; Artwork image; Clock::time_point retry_at{}; };
    struct Job { std::string url; std::uint64_t generation = 0; };
    struct Completion { Job job; Artwork image; };

    explicit Impl(std::string directory) : cache_directory(std::move(directory)), worker([this] { run(); }) {}
    ~Impl() {
        stopping.store(true);
        condition.notify_one();
        worker.join();
    }

    bool cancelled(const Job &job) const { return stopping.load() || generation.load() != job.generation; }

    bool capture(const std::vector<std::string> &arguments, std::size_t maximum,
                 std::chrono::seconds timeout, const Job &job, std::vector<std::uint8_t> &output)
    {
        if (cancelled(job)) return false;
        int pipe_fds[2]{};
        if (pipe(pipe_fds) != 0) return false;
        if (fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK) < 0 || fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC) < 0 ||
            fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC) < 0) {
            close(pipe_fds[0]); close(pipe_fds[1]); return false;
        }
        std::vector<char *> argv;
        for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawnattr_t attributes;
        if (posix_spawn_file_actions_init(&actions) != 0) { close(pipe_fds[0]); close(pipe_fds[1]); return false; }
        if (posix_spawnattr_init(&attributes) != 0) {
            posix_spawn_file_actions_destroy(&actions); close(pipe_fds[0]); close(pipe_fds[1]); return false;
        }
        bool configured = posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO) == 0 &&
            posix_spawn_file_actions_addclose(&actions, pipe_fds[0]) == 0 &&
            posix_spawn_file_actions_addclose(&actions, pipe_fds[1]) == 0 &&
            posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0 &&
            posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0 &&
            posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP) == 0 &&
            posix_spawnattr_setpgroup(&attributes, 0) == 0;
        pid_t child = -1;
        const int spawned = configured ? posix_spawnp(&child, argv[0], &actions, &attributes, argv.data(), environ) : EINVAL;
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        close(pipe_fds[1]);
        if (spawned != 0) { close(pipe_fds[0]); return false; }

        const auto deadline = Clock::now() + timeout;
        bool eof = false;
        bool exited = false;
        bool success = true;
        int status = 0;
        output.clear();
        std::array<std::uint8_t, 16384> bytes{};
        while (success && !(eof && exited)) {
            if (cancelled(job) || Clock::now() >= deadline) { success = false; break; }
            if (!eof) {
                const ssize_t count = read(pipe_fds[0], bytes.data(), bytes.size());
                if (count > 0) {
                    if (output.size() + static_cast<std::size_t>(count) > maximum) { success = false; break; }
                    output.insert(output.end(), bytes.begin(), bytes.begin() + count);
                    continue;
                }
                if (count == 0) eof = true;
                else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) { success = false; break; }
            }
            if (!exited) {
                const pid_t result = waitpid(child, &status, WNOHANG);
                if (result == child) exited = true;
                else if (result < 0 && errno != EINTR) { success = false; exited = true; break; }
            }
            if (!(eof && exited)) {
                pollfd descriptor{pipe_fds[0], POLLIN, 0};
                // EOF with a live child must also wait, otherwise POLLHUP would spin.
                (void)::poll(eof ? nullptr : &descriptor, eof ? 0U : 1U, 20);
            }
        }
        close(pipe_fds[0]);
        if (!success || !exited) {
            // Spawn each request in its own process group so cancellation also
            // terminates helpers and always reaps the direct child.
            (void)kill(-child, SIGKILL);
            if (!exited) while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
        }
        return success && WIFEXITED(status) && WEXITSTATUS(status) == 0 && !cancelled(job);
    }

    Artwork decode(const Job &job, const std::vector<std::uint8_t> &source)
    {
        if (image_extension(source).empty() || cancelled(job)) return {};
        std::error_code error;
        const auto temporary_directory = std::filesystem::temp_directory_path(error);
        if (error) return {};
        TemporaryFile input((temporary_directory / "lilygo-radio-art-XXXXXX").string());
        if (input.fd() < 0 || !write_all(input.fd(), source.data(), source.size())) return {};
        std::vector<std::uint8_t> pixels;
        // Restrict demuxers to raster images. Catalog logos cannot make ffmpeg
        // open a playlist, another network resource, or a vector document.
        if (!capture({"ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-max_alloc", "67108864",
                      "-threads", "1", "-max_pixels", "4194304", "-protocol_whitelist", "file,pipe",
                      "-format_whitelist", "png_pipe,jpeg_pipe,bmp_pipe,ico,webp_pipe,gif",
                      "-i", input.path(), "-map", "0:v:0", "-frames:v", "1", "-an", "-sn", "-dn",
                      "-vf", "scale=256:256:force_original_aspect_ratio=decrease,format=bgra,pad=256:256:(ow-iw)/2:(oh-ih)/2:color=0x00000000",
                      "-threads", "1", "-f", "rawvideo", "-pix_fmt", "bgra", "pipe:1"},
                     kPixelBytes, std::chrono::seconds(8), job, pixels) || pixels.size() != kPixelBytes) return {};
        return {kImageSize, kImageSize, std::move(pixels)};
    }

    Artwork fetch(const Job &job)
    {
        auto source = read_cache(cache_directory, job.url);
        if (!source.empty()) {
            auto image = decode(job, source);
            if (!image.pixels.empty()) return image;
        }
        if (cancelled(job)) return {};
        if (!capture({"curl", "--disable", "--location", "--silent", "--show-error", "--fail",
                      "--proto", "=http,https", "--proto-redir", "=http,https", "--max-redirs", "5",
                      "--max-time", "12", "--connect-timeout", "4", "--max-filesize", "2097152",
                      "--user-agent", "lilygo-ui-radio/1.0", "--", job.url},
                     kMaximumDownload, std::chrono::seconds(14), job, source)) return {};
        auto image = decode(job, source);
        if (!image.pixels.empty() && !cancelled(job)) write_cache(cache_directory, job.url, source);
        return image;
    }

    void run() {
        while (!stopping.load()) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] { return stopping.load() || !jobs.empty(); });
                if (stopping.load()) return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            Artwork image;
            try { image = fetch(job); } catch (const std::exception &) { image = {}; }
            std::lock_guard<std::mutex> lock(mutex);
            if (!cancelled(job)) completed.push_back({std::move(job), std::move(image)});
        }
    }

    const std::string cache_directory;
    std::unordered_map<std::string, Entry> entries;
    std::atomic<std::uint64_t> generation{0};
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<Job> jobs;
    std::deque<Completion> completed;
    std::thread worker;
};

ArtworkCache::ArtworkCache() : ArtworkCache(default_cache_directory()) {}
ArtworkCache::ArtworkCache(std::string directory) : impl_(std::make_unique<Impl>(std::move(directory))) {}
ArtworkCache::~ArtworkCache() = default;

const Artwork *ArtworkCache::request(const std::string &url, bool priority)
{
    if (!valid_url(url)) return nullptr;
    auto found = impl_->entries.find(url);
    if (found != impl_->entries.end()) {
        if (found->second.state == Impl::State::Ready) return &found->second.image;
        if (found->second.state == Impl::State::Failed && Clock::now() < found->second.retry_at) return nullptr;
        if (found->second.state == Impl::State::Pending) {
            if (priority) {
                std::lock_guard<std::mutex> lock(impl_->mutex);
                auto queued = std::find_if(impl_->jobs.begin(), impl_->jobs.end(), [&url](const Impl::Job &job) { return job.url == url; });
                if (queued != impl_->jobs.end()) { auto job = std::move(*queued); impl_->jobs.erase(queued); impl_->jobs.push_front(std::move(job)); }
            }
            return nullptr;
        }
    } else {
        if (impl_->entries.size() >= kMemoryEntries) {
            auto failed = std::find_if(impl_->entries.begin(), impl_->entries.end(), [](const auto &entry) { return entry.second.state == Impl::State::Failed; });
            if (failed == impl_->entries.end()) return nullptr;
            impl_->entries.erase(failed);
        }
        found = impl_->entries.emplace(url, Impl::Entry{}).first;
    }
    found->second.state = Impl::State::Pending;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Impl::Job job{url, impl_->generation.load()};
        if (priority) impl_->jobs.push_front(std::move(job));
        else impl_->jobs.push_back(std::move(job));
    }
    impl_->condition.notify_one();
    return nullptr;
}

bool ArtworkCache::poll()
{
    std::deque<Impl::Completion> completed;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        // Publishing at most four images bounds one LVGL timer invocation.
        for (unsigned i = 0; i < 4 && !impl_->completed.empty(); ++i) {
            completed.push_back(std::move(impl_->completed.front()));
            impl_->completed.pop_front();
        }
    }
    bool changed = false;
    for (auto &completion : completed) {
        const auto found = impl_->entries.find(completion.job.url);
        if (completion.job.generation != impl_->generation.load() || found == impl_->entries.end()) continue;
        auto &entry = found->second;
        if (completion.image.pixels.empty()) { entry.state = Impl::State::Failed; entry.retry_at = Clock::now() + kRetryDelay; }
        else { entry.image = std::move(completion.image); entry.state = Impl::State::Ready; }
        changed = true;
    }
    return changed;
}

bool ArtworkCache::loading() const
{
    return std::any_of(impl_->entries.begin(), impl_->entries.end(), [](const auto &entry) { return entry.second.state == Impl::State::Pending; });
}

bool ArtworkCache::release(const std::string &url)
{
    const auto found = impl_->entries.find(url);
    if (found == impl_->entries.end()) return true;
    if (found->second.state == Impl::State::Pending) return false;
    impl_->entries.erase(found);
    return true;
}

void ArtworkCache::clear()
{
    impl_->generation.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->jobs.clear();
        impl_->completed.clear();
    }
    impl_->entries.clear();
}

std::string ArtworkCache::default_cache_directory()
{
    if (const char *directory = std::getenv("LILYGO_UI_RADIO_ARTWORK_CACHE")) return directory;
    if (const char *directory = std::getenv("XDG_CACHE_HOME")) {
        if (*directory != '\0') return std::string(directory) + "/lilygo/ui/radio-artwork";
    }
    if (const char *directory = std::getenv("HOME")) {
        if (*directory != '\0') return std::string(directory) + "/.cache/lilygo/ui/radio-artwork";
    }
    return {};
}
