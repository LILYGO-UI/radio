#include "services/stream_player.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr const char *kLogPath = "/tmp/lilygo-ui-radio.log";
#if defined(__APPLE__)
constexpr const char *kAudioSink = "ffplay";
#else
constexpr const char *kAudioSink = "aplay";
#endif

void attach_log_to_stderr() noexcept
{
    const int descriptor = open(kLogPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (descriptor < 0) return;
    (void)dup2(descriptor, STDERR_FILENO);
    close(descriptor);
}

int child_exit_code(int status) noexcept
{
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

}  // namespace

StreamPlayer::~StreamPlayer()
{
    terminate_process();
}

bool StreamPlayer::start(std::string_view url)
{
    const std::string ffmpeg = find_executable("ffmpeg");
    const std::string audio_sink = find_executable(kAudioSink);
    if (ffmpeg.empty()) {
        error_ = "ffmpeg not found";
        state_ = StreamPlaybackState::Error;
        return false;
    }
    if (audio_sink.empty()) {
        error_ = std::string(kAudioSink) + " not found";
        state_ = StreamPlaybackState::Error;
        return false;
    }

    terminate_process();
    const std::string owned_url(url);
    const pid_t pid = fork();
    if (pid < 0) {
        error_ = std::string("Unable to start player: ") + std::strerror(errno);
        state_ = StreamPlaybackState::Error;
        return false;
    }
    if (pid == 0) {
        (void)setpgid(0, 0);
        run_pipeline(ffmpeg, audio_sink, owned_url);
    }

    (void)setpgid(pid, pid);
    player_pid_ = pid;
    error_.clear();
    state_      = StreamPlaybackState::Starting;
    started_at_ = std::chrono::steady_clock::now();
    return true;
}

void StreamPlayer::stop() noexcept
{
    terminate_process();
    error_.clear();
    state_ = StreamPlaybackState::Stopped;
}

bool StreamPlayer::toggle_pause() noexcept
{
    if (player_pid_ <= 0) return false;

    if (state_ == StreamPlaybackState::Paused) {
        if (kill(-player_pid_, SIGCONT) != 0) return false;
        state_ = StreamPlaybackState::Playing;
        return true;
    }
    if (state_ == StreamPlaybackState::Starting || state_ == StreamPlaybackState::Playing) {
        if (kill(-player_pid_, SIGSTOP) != 0) return false;
        state_ = StreamPlaybackState::Paused;
        return true;
    }
    return false;
}

void StreamPlayer::poll() noexcept
{
    if (player_pid_ <= 0) return;

    int status      = 0;
    const pid_t pid = waitpid(player_pid_, &status, WNOHANG);
    if (pid == 0) {
        if (state_ == StreamPlaybackState::Starting &&
            std::chrono::steady_clock::now() - started_at_ >= std::chrono::milliseconds(800)) {
            state_ = StreamPlaybackState::Playing;
        }
        return;
    }
    if (pid < 0) {
        if (errno == EINTR) return;
        error_      = std::string("Player status error: ") + std::strerror(errno);
        player_pid_ = -1;
        state_      = StreamPlaybackState::Error;
        return;
    }

    const int exit_code = child_exit_code(status);
    player_pid_         = -1;
    if (exit_code == 0) {
        error_.clear();
        state_ = StreamPlaybackState::Stopped;
    } else {
        error_ = "Playback failed. Check the network, URL, and audio device.";
        state_ = StreamPlaybackState::Error;
    }
}

StreamPlaybackState StreamPlayer::state() const noexcept
{
    return state_;
}

const std::string &StreamPlayer::error() const noexcept
{
    return error_;
}

std::string StreamPlayer::find_executable(std::string_view name)
{
    if (name.empty() || name.find('/') != std::string_view::npos) return {};

    const char *path_value = std::getenv("PATH");
    if (!path_value || path_value[0] == '\0') path_value = "/usr/local/bin:/usr/bin:/bin";

    std::string_view paths(path_value);
    while (true) {
        const std::size_t separator = paths.find(':');
        std::string directory(paths.substr(0, separator));
        if (directory.empty()) directory = ".";
        const std::string candidate = directory + "/" + std::string(name);
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
        if (separator == std::string_view::npos) break;
        paths.remove_prefix(separator + 1U);
    }
    return {};
}

void StreamPlayer::run_pipeline(const std::string &ffmpeg, const std::string &audio_sink, const std::string &url) noexcept
{
    int pipe_descriptors[2]{};
    if (pipe(pipe_descriptors) != 0) _exit(126);

    const pid_t decoder = fork();
    if (decoder < 0) _exit(126);
    if (decoder == 0) {
        (void)dup2(pipe_descriptors[1], STDOUT_FILENO);
        close(pipe_descriptors[0]);
        close(pipe_descriptors[1]);
        attach_log_to_stderr();
        execl(ffmpeg.c_str(), ffmpeg.c_str(), "-nostdin", "-hide_banner", "-loglevel", "warning", "-reconnect", "1",
              "-reconnect_streamed", "1", "-reconnect_delay_max", "5", "-rw_timeout", "15000000", "-i", url.c_str(),
              "-vn", "-f", "s16le", "-acodec", "pcm_s16le", "-ac", "2", "-ar", "48000", "-",
              static_cast<char *>(nullptr));
        _exit(127);
    }

    const pid_t sink = fork();
    if (sink < 0) {
        (void)kill(decoder, SIGTERM);
        (void)waitpid(decoder, nullptr, 0);
        _exit(126);
    }
    if (sink == 0) {
        (void)dup2(pipe_descriptors[0], STDIN_FILENO);
        close(pipe_descriptors[0]);
        close(pipe_descriptors[1]);
        attach_log_to_stderr();
#if defined(__APPLE__)
        execl(audio_sink.c_str(), audio_sink.c_str(), "-nodisp", "-autoexit", "-hide_banner", "-loglevel", "warning",
              "-f", "s16le", "-sample_rate", "48000", "-ch_layout", "stereo", "-i", "pipe:0",
              static_cast<char *>(nullptr));
#else
        execl(audio_sink.c_str(), audio_sink.c_str(), "-q", "-D", "default", "-t", "raw", "-f", "S16_LE", "-c", "2", "-r",
              "48000", "-B", "50000", "-F", "10000", "-", static_cast<char *>(nullptr));
#endif
        _exit(127);
    }

    close(pipe_descriptors[0]);
    close(pipe_descriptors[1]);
    int decoder_status = 0;
    int sink_status    = 0;
    (void)waitpid(decoder, &decoder_status, 0);
    (void)waitpid(sink, &sink_status, 0);
    _exit(child_exit_code(decoder_status) == 0 && child_exit_code(sink_status) == 0 ? 0 : 1);
}

void StreamPlayer::terminate_process() noexcept
{
    if (player_pid_ <= 0) return;

    (void)kill(-player_pid_, SIGTERM);
    int status = 0;
    for (int attempt = 0; attempt < 10; ++attempt) {
        const pid_t result = waitpid(player_pid_, &status, WNOHANG);
        if (result == player_pid_ || (result < 0 && errno == ECHILD)) {
            player_pid_ = -1;
            return;
        }
        usleep(20000);
    }
    (void)kill(-player_pid_, SIGKILL);
    while (waitpid(player_pid_, &status, 0) < 0 && errno == EINTR) {
    }
    player_pid_ = -1;
}
