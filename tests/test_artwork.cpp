#include "services/artwork_cache.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t kPixelBytes = 256U * 256U * 4U;

std::string bmp_source()
{
    // Two opaque pixels provide a real image and distinguish source from decoded bytes.
    const unsigned char bmp[] = {
        'B', 'M', 62, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0,
        2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0, 0, 0, 0, 0, 8, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0, 255, 0, 0, 0
    };
    return {reinterpret_cast<const char *>(bmp), sizeof(bmp)};
}

std::string hash_key(const std::string &bytes)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : bytes) { hash ^= byte; hash *= 1099511628211ULL; }
    std::ostringstream result;
    result << std::hex << std::setfill('0') << std::setw(16) << hash;
    return result.str();
}

std::string metadata(const std::string &url, const std::string &extension, const std::string &source)
{
    return "RADIO-ART-ORIGINAL-V2\n" + url + '\n' + extension + '\n' +
           std::to_string(source.size()) + '\n' + hash_key(source) + '\n';
}

std::string find_ffmpeg()
{
    const char *environment = std::getenv("PATH");
    std::string paths = environment ? environment : "/usr/bin:/bin";
    std::size_t begin = 0;
    do {
        const auto end = paths.find(':', begin);
        const auto path = paths.substr(begin, end == std::string::npos ? end : end - begin) + "/ffmpeg";
        if (access(path.c_str(), X_OK) == 0) return path;
        if (end == std::string::npos) break;
        begin = end + 1;
    } while (begin <= paths.size());
    return {};
}

class Fixture {
public:
    Fixture() {
        char pattern[] = "/tmp/lilygo-radio-artwork-test-XXXXXX";
        const char *created = mkdtemp(pattern);
        assert(created);
        directory = created;
        if (const char *path = std::getenv("PATH")) old_path = path;
        write("curl", "#!/bin/sh\nprintf '%s\\n' \"$@\" >> \"$ARTWORK_TEST_DIRECTORY/curl-args\"\n"
              "if [ \"$ARTWORK_TEST_FAIL\" = 1 ]; then exit 22; fi\n"
              "if [ \"$ARTWORK_TEST_SLOW\" = 1 ]; then printf '%s' \"$$\" > \"$ARTWORK_TEST_DIRECTORY/pid\"; exec /bin/sleep 30; fi\n"
              "exec /bin/cat \"$ARTWORK_TEST_DIRECTORY/source\"\n");
        write("ffmpeg", "#!/bin/sh\nprintf '%s\\n' \"$@\" >> \"$ARTWORK_TEST_DIRECTORY/ffmpeg-args\"\n"
              "if [ -n \"$ARTWORK_TEST_REAL_FFMPEG\" ]; then exec \"$ARTWORK_TEST_REAL_FFMPEG\" \"$@\"; fi\n"
              "if [ \"$ARTWORK_TEST_DECODE_FAIL\" = 1 ]; then exit 1; fi\n"
              "if [ -e \"$ARTWORK_TEST_DIRECTORY/fail-next-decode\" ]; then /bin/rm \"$ARTWORK_TEST_DIRECTORY/fail-next-decode\"; exit 1; fi\n"
              "exec /bin/cat \"$ARTWORK_TEST_DIRECTORY/pixels\"\n");
        assert(chmod((directory + "/curl").c_str(), 0700) == 0);
        assert(chmod((directory + "/ffmpeg").c_str(), 0700) == 0);
        assert(setenv("PATH", directory.c_str(), 1) == 0);
        assert(setenv("ARTWORK_TEST_DIRECTORY", directory.c_str(), 1) == 0);
        write("source", bmp_source());
        write("pixels", std::string(kPixelBytes, '\x7f'));
    }
    ~Fixture() {
        (void)setenv("PATH", old_path.c_str(), 1);
        for (const char *name : {"ARTWORK_TEST_DIRECTORY", "ARTWORK_TEST_FAIL", "ARTWORK_TEST_SLOW",
                                "ARTWORK_TEST_REAL_FFMPEG", "ARTWORK_TEST_DECODE_FAIL"}) (void)unsetenv(name);
        std::filesystem::remove_all(directory);
    }
    void write(const std::string &name, const std::string &bytes) const {
        std::ofstream stream(directory + '/' + name, std::ios::binary);
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        assert(stream.good());
    }
    std::string read(const std::string &name) const {
        std::ifstream stream(directory + '/' + name, std::ios::binary);
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
    }
    std::size_t calls(const std::string &program) const {
        const auto arguments = read(program + "-args");
        const std::string marker = program == "curl" ? "--disable\n" : "-nostdin\n";
        std::size_t count = 0;
        std::size_t position = 0;
        while ((position = arguments.find(marker, position)) != std::string::npos) {
            ++count;
            position += marker.size();
        }
        return count;
    }
    std::string directory;
    std::string old_path;
};

void await_idle(ArtworkCache &cache)
{
    const auto deadline = Clock::now() + std::chrono::seconds(6);
    while (cache.loading()) {
        const auto before = Clock::now();
        cache.poll();
        assert(Clock::now() - before < std::chrono::milliseconds(100));
        assert(Clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(!cache.poll());
}

void expect_failure(ArtworkCache &cache, const Fixture &fixture, const std::string &url)
{
    assert(!cache.request(url));
    await_idle(cache);
    const auto arguments = fixture.read("curl-args");
    assert(!cache.request(url));
    assert(!cache.loading());
    assert(fixture.read("curl-args") == arguments);
}
}  // namespace

int main(int argc, char **argv)
{
    const auto real_ffmpeg = find_ffmpeg();
    Fixture fixture;
    ArtworkCache cache("");
    for (const auto &url : {"", "file:///etc/passwd", "ftp://radio.example/logo.png", "--config=/tmp/config",
                            "https://", "https:///logo.png", "https://radio.example/\nlogo.png"}) {
        assert(!cache.request(url));
        assert(!cache.loading());
    }
    assert(!std::filesystem::exists(fixture.directory + "/curl-args"));

    const std::string first_url = "https://radio.example/logo.png?name=$(touch%20bad)&x=1";
    assert(!cache.request(first_url, true));
    assert(!cache.release(first_url));
    await_idle(cache);
    const Artwork *first = cache.request(first_url);
    assert(first && first->width == 256 && first->height == 256 && first->pixels.size() == kPixelBytes);
    assert(first->pixels.front() == 0x7f);
    const auto curl_arguments = fixture.read("curl-args");
    const auto ffmpeg_arguments = fixture.read("ffmpeg-args");
    assert(curl_arguments.find("--disable\n") == 0);
    assert(curl_arguments.find("--proto\n=http,https\n--proto-redir\n=http,https\n") != std::string::npos);
    assert(curl_arguments.find("--max-filesize\n2097152\n") != std::string::npos);
    assert(curl_arguments.find("--\n" + first_url + '\n') != std::string::npos);
    assert(ffmpeg_arguments.find("-format_whitelist\npng_pipe,jpeg_pipe,bmp_pipe,ico,webp_pipe,gif\n") != std::string::npos);
    assert(ffmpeg_arguments.find("-frames:v\n1\n") != std::string::npos);
    assert(ffmpeg_arguments.find("-max_pixels\n4194304\n") != std::string::npos);
    assert(ffmpeg_arguments.find("-pix_fmt\nbgra\n") != std::string::npos);

    for (int i = 0; i < 9; ++i) cache.request("http://radio.example/" + std::to_string(i));
    await_idle(cache);
    assert(cache.request(first_url) == first && first->pixels.front() == 0x7f);
    assert(cache.release("https://radio.example/not-cached"));

    assert(setenv("ARTWORK_TEST_FAIL", "1", 1) == 0);
    expect_failure(cache, fixture, "https://radio.example/http-failure");
    (void)unsetenv("ARTWORK_TEST_FAIL");
    assert(setenv("ARTWORK_TEST_DECODE_FAIL", "1", 1) == 0);
    expect_failure(cache, fixture, "https://radio.example/decode-failure");
    (void)unsetenv("ARTWORK_TEST_DECODE_FAIL");
    fixture.write("source", "");
    expect_failure(cache, fixture, "https://radio.example/empty");
    fixture.write("source", std::string(2U * 1024U * 1024U + 1U, 'x'));
    const auto decodes = fixture.read("ffmpeg-args");
    expect_failure(cache, fixture, "https://radio.example/too-large");
    assert(fixture.read("ffmpeg-args") == decodes);
    fixture.write("source", "not an image");
    expect_failure(cache, fixture, "https://radio.example/unsupported.svg");
    assert(fixture.read("ffmpeg-args") == decodes);
    fixture.write("source", bmp_source());
    fixture.write("pixels", "short-output");
    expect_failure(cache, fixture, "https://radio.example/short-pixels");
    fixture.write("pixels", std::string(kPixelBytes + 1U, 'x'));
    expect_failure(cache, fixture, "https://radio.example/excess-pixels");
    fixture.write("pixels", std::string(kPixelBytes, '\x7f'));

    const auto cache_directory = fixture.directory + "/cache";
    const std::string persistent_url = "https://radio.example/persistent.png";
    const auto persistent_key = hash_key(persistent_url);
    const auto source = bmp_source();
    {
        ArtworkCache persistent(cache_directory);
        persistent.request(persistent_url);
        await_idle(persistent);
        assert(persistent.request(persistent_url));
        const auto downloads = fixture.calls("curl");
        const auto decodes = fixture.calls("ffmpeg");
        assert(persistent.request(persistent_url));
        assert(!persistent.loading());
        assert(fixture.calls("curl") == downloads && fixture.calls("ffmpeg") == decodes);
    }
    assert(fixture.read("cache/" + persistent_key + ".bmp") == source);
    assert(fixture.read("cache/" + persistent_key + ".meta") == metadata(persistent_url, ".bmp", source));
    assert(!std::filesystem::exists(cache_directory + '/' + persistent_key + ".png"));
    assert(!std::filesystem::exists(cache_directory + '/' + persistent_key + ".radio-art"));
    assert(std::filesystem::file_size(cache_directory + '/' + persistent_key + ".bmp") == 62U);
    const auto persisted_arguments = fixture.read("curl-args");
    const auto persisted_decodes = fixture.calls("ffmpeg");
    assert(setenv("ARTWORK_TEST_FAIL", "1", 1) == 0);
    {
        ArtworkCache persistent(cache_directory);
        persistent.request(persistent_url);
        await_idle(persistent);
        assert(persistent.request(persistent_url));
        assert(fixture.read("curl-args") == persisted_arguments);
        assert(fixture.calls("ffmpeg") == persisted_decodes + 1U);
        assert(persistent.release(persistent_url));
        assert(!persistent.request(persistent_url));
        assert(!persistent.release(persistent_url));
        await_idle(persistent);
        const Artwork *reloaded = persistent.request(persistent_url);
        assert(reloaded && reloaded->pixels.size() == kPixelBytes && reloaded->pixels.front() == 0x7f);
        assert(fixture.read("curl-args") == persisted_arguments);
        assert(fixture.calls("ffmpeg") == persisted_decodes + 2U);
    }
    (void)unsetenv("ARTWORK_TEST_FAIL");

    const auto expect_redownload = [&] {
        const auto downloads = fixture.calls("curl");
        ArtworkCache persistent(cache_directory);
        persistent.request(persistent_url);
        await_idle(persistent);
        assert(persistent.request(persistent_url));
        assert(fixture.calls("curl") == downloads + 1U);
        assert(fixture.read("cache/" + persistent_key + ".bmp") == source);
        assert(fixture.read("cache/" + persistent_key + ".meta") == metadata(persistent_url, ".bmp", source));
    };

    // URL collisions, altered payloads, incomplete records, and stale entries refetch.
    fixture.write("cache/" + persistent_key + ".meta", metadata("https://radio.example/other.png", ".bmp", source));
    expect_redownload();
    auto corrupted = source;
    corrupted.back() ^= 0x01;
    fixture.write("cache/" + persistent_key + ".bmp", corrupted);
    expect_redownload();
    fixture.write("cache/" + persistent_key + ".bmp", source.substr(0, source.size() - 1U));
    expect_redownload();
    std::filesystem::last_write_time(cache_directory + '/' + persistent_key + ".meta",
        std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 8));
    expect_redownload();

    // A checksum alone cannot make a non-image disk payload a valid cache entry.
    fixture.write("cache/" + persistent_key + ".bmp", "not an image");
    fixture.write("cache/" + persistent_key + ".meta", metadata(persistent_url, ".bmp", "not an image"));
    expect_redownload();

    // Valid cached bytes that cannot decode get one fresh network attempt.
    fixture.write("fail-next-decode", "");
    {
        const auto decodes = fixture.calls("ffmpeg");
        expect_redownload();
        assert(fixture.calls("ffmpeg") == decodes + 2U);
    }

    const auto invalid_directory = fixture.directory + "/invalid-cache";
    {
        ArtworkCache invalid(invalid_directory);
        assert(setenv("ARTWORK_TEST_DECODE_FAIL", "1", 1) == 0);
        expect_failure(invalid, fixture, "https://radio.example/invalid.bmp");
        (void)unsetenv("ARTWORK_TEST_DECODE_FAIL");
    }
    assert(!std::filesystem::exists(invalid_directory) || std::filesystem::is_empty(invalid_directory));

    // Source signatures determine file extensions even when URLs claim PNG.
    const std::vector<std::pair<std::string, std::string>> formats = {
        {".png", std::string("\x89PNG\r\n\x1a\n", 8) + "png-payload"},
        {".jpg", std::string("\xff\xd8\xff\xe0", 4) + "jpeg-payload"},
        {".ico", std::string("\0\0\x01\0\x01\0", 6) + std::string(32, '\0')},
        {".webp", std::string("RIFF\x10\0\0\0WEBPVP8 ", 16) + "webp-payload"},
        {".bmp", source},
        {".gif", "GIF89a\x01\x01gif-payload"}
    };
    const auto formats_directory = fixture.directory + "/formats";
    for (std::size_t i = 0; i < formats.size(); ++i) {
        const auto &format = formats[i];
        const auto url = "https://radio.example/format-" + std::to_string(i) + ".png";
        const auto key = hash_key(url);
        fixture.write("source", format.second);
        {
            ArtworkCache persistent(formats_directory);
            persistent.request(url);
            await_idle(persistent);
            assert(persistent.request(url));
        }
        assert(fixture.read("formats/" + key + format.first) == format.second);
        assert(fixture.read("formats/" + key + ".meta") == metadata(url, format.first, format.second));
        const auto downloads = fixture.calls("curl");
        {
            ArtworkCache persistent(formats_directory);
            persistent.request(url);
            await_idle(persistent);
            assert(persistent.request(url));
        }
        assert(fixture.calls("curl") == downloads);
    }
    fixture.write("source", source);

    // Only owned image records count toward disk limits; unrelated files stay.
    const auto old_time = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
    for (int i = 0; i < 104; ++i) {
        const auto url = "https://radio.example/old-" + std::to_string(i);
        const auto key = hash_key(url);
        fixture.write("cache/" + key + ".meta", metadata(url, ".bmp", source));
        fixture.write("cache/" + key + ".bmp", source);
        std::filesystem::last_write_time(cache_directory + '/' + key + ".meta", old_time);
    }
    const auto legacy_key = hash_key("https://radio.example/legacy.png");
    fixture.write("cache/" + legacy_key + ".radio-art", "RADIO-ART-BGRA-256-V1\nlegacy\n" + std::string(kPixelBytes, 'x'));
    fixture.write("cache/keep.txt", "unrelated");
    fixture.write("cache/keep.png", "unrelated-image");
    fixture.write("cache/keep.meta", "unrelated-metadata");
    fixture.write("cache/keep.radio-art", "unrelated-legacy");
    {
        ArtworkCache persistent(cache_directory);
        persistent.request("https://radio.example/prune.png");
        await_idle(persistent);
        assert(persistent.request("https://radio.example/prune.png"));
    }
    unsigned metadata_files = 0;
    unsigned source_files = 0;
    for (const auto &entry : std::filesystem::directory_iterator(cache_directory)) {
        if (entry.path().stem() == "keep") continue;
        if (entry.path().extension() == ".meta") {
            ++metadata_files;
            auto source_path = entry.path();
            source_path.replace_extension(".bmp");
            assert(std::filesystem::exists(source_path));
        } else if (entry.path().extension() == ".bmp") {
            ++source_files;
            auto metadata_path = entry.path();
            metadata_path.replace_extension(".meta");
            assert(std::filesystem::exists(metadata_path));
        } else assert(false);
    }
    assert(metadata_files == 100 && source_files == 100);
    assert(!std::filesystem::exists(cache_directory + '/' + legacy_key + ".radio-art"));
    assert(fixture.read("cache/keep.txt") == "unrelated");
    assert(fixture.read("cache/keep.png") == "unrelated-image");
    assert(fixture.read("cache/keep.meta") == "unrelated-metadata");
    assert(fixture.read("cache/keep.radio-art") == "unrelated-legacy");

    pid_t cancelled_pid = -1;
    {
        ArtworkCache cancellable("");
        assert(setenv("ARTWORK_TEST_SLOW", "1", 1) == 0);
        cancellable.request("https://radio.example/slow");
        const auto deadline = Clock::now() + std::chrono::seconds(3);
        while (fixture.read("pid").empty()) {
            assert(Clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        cancelled_pid = static_cast<pid_t>(std::stoi(fixture.read("pid")));
        const auto before = Clock::now();
        cancellable.clear();
        assert(Clock::now() - before < std::chrono::milliseconds(100));
        assert(!cancellable.loading());
        (void)unsetenv("ARTWORK_TEST_SLOW");
        cancellable.request("https://radio.example/after-cancel");
        await_idle(cancellable);
        assert(cancellable.request("https://radio.example/after-cancel"));
    }
    assert(waitpid(cancelled_pid, nullptr, WNOHANG) == -1 && errno == ECHILD);

    if (!real_ffmpeg.empty()) {
        assert(setenv("ARTWORK_TEST_REAL_FFMPEG", real_ffmpeg.c_str(), 1) == 0);
        fixture.write("source", source);
        const auto decoded_directory = fixture.directory + "/decoded";
        ArtworkCache decoded(decoded_directory);
        decoded.request("https://radio.example/real.bmp");
        await_idle(decoded);
        const auto *image = decoded.request("https://radio.example/real.bmp");
        assert(image && image->pixels.size() == kPixelBytes);
        assert(image->pixels[3] == 0);
        const auto red = (128U * 256U + 16U) * 4U;
        const auto green = (128U * 256U + 240U) * 4U;
        assert(image->pixels[red] == 0 && image->pixels[red + 1] == 0 && image->pixels[red + 2] == 255 && image->pixels[red + 3] == 255);
        assert(image->pixels[green] == 0 && image->pixels[green + 1] == 255 && image->pixels[green + 2] == 0 && image->pixels[green + 3] == 255);
        assert(fixture.read("decoded/" + hash_key("https://radio.example/real.bmp") + ".bmp") == source);
        const auto downloads = fixture.calls("curl");
        {
            ArtworkCache offline(decoded_directory);
            assert(setenv("ARTWORK_TEST_FAIL", "1", 1) == 0);
            offline.request("https://radio.example/real.bmp");
            await_idle(offline);
            const auto *reloaded = offline.request("https://radio.example/real.bmp");
            assert(reloaded && reloaded->pixels == image->pixels);
            assert(fixture.calls("curl") == downloads);
            (void)unsetenv("ARTWORK_TEST_FAIL");
        }
        for (int i = 1; i < argc; ++i) {
            std::ifstream stream(argv[i], std::ios::binary);
            assert(stream.good());
            fixture.write("source", {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()});
            const auto url = "https://radio.example/format-" + std::to_string(i);
            decoded.request(url);
            await_idle(decoded);
            assert(decoded.request(url));
            std::cout << "Decoded image: " << argv[i] << '\n';
        }
        fixture.write("source", "not an image");
        expect_failure(decoded, fixture, "https://radio.example/malformed.png");
        (void)unsetenv("ARTWORK_TEST_REAL_FFMPEG");
    } else std::cout << "Real ffmpeg decode skipped: executable unavailable\n";

    cache.clear();
    assert(!cache.loading());
    std::cout << "Artwork cache tests passed\n";
}
