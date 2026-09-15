#ifndef LILYGO_UI_RADIO_SERVICES_STREAM_PLAYER_HPP
#define LILYGO_UI_RADIO_SERVICES_STREAM_PLAYER_HPP

#include <chrono>
#include <string>
#include <string_view>

#include <sys/types.h>

enum class StreamPlaybackState {
    Idle,
    Starting,
    Playing,
    Paused,
    Stopped,
    Error,
};

class StreamPlayerInterface {
public:
    virtual ~StreamPlayerInterface() = default;

    virtual bool start(std::string_view url) = 0;
    virtual void stop() noexcept             = 0;
    virtual bool toggle_pause() noexcept     = 0;
    virtual void poll() noexcept             = 0;

    [[nodiscard]] virtual StreamPlaybackState state() const noexcept = 0;
    [[nodiscard]] virtual const std::string &error() const noexcept  = 0;
};

class StreamPlayer final : public StreamPlayerInterface {
public:
    StreamPlayer() = default;
    ~StreamPlayer() override;

    StreamPlayer(const StreamPlayer &)            = delete;
    StreamPlayer &operator=(const StreamPlayer &) = delete;
    StreamPlayer(StreamPlayer &&)                 = delete;
    StreamPlayer &operator=(StreamPlayer &&)      = delete;

    bool start(std::string_view url) override;
    void stop() noexcept override;
    bool toggle_pause() noexcept override;
    void poll() noexcept override;

    [[nodiscard]] StreamPlaybackState state() const noexcept override;
    [[nodiscard]] const std::string &error() const noexcept override;

private:
    static std::string find_executable(std::string_view name);
    static void run_pipeline(const std::string &ffmpeg, const std::string &audio_sink, const std::string &url) noexcept;
    void terminate_process() noexcept;

    pid_t player_pid_          = -1;
    StreamPlaybackState state_ = StreamPlaybackState::Idle;
    std::string error_;
    std::chrono::steady_clock::time_point started_at_{};
};

#endif
