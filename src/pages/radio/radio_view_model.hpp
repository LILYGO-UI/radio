#ifndef LILYGO_UI_RADIO_PAGES_RADIO_RADIO_VIEW_MODEL_HPP
#define LILYGO_UI_RADIO_PAGES_RADIO_RADIO_VIEW_MODEL_HPP

#include "components/lv_subject.hpp"
#include "pages/radio/radio_model.hpp"
#include "services/radio_browser_client.hpp"
#include "services/stream_player.hpp"

#include <cstddef>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

enum class DirectoryState { Idle, Loading, Ready, Empty, Error };

class RadioViewModel {
public:
    using Clock = std::chrono::steady_clock;
    using NowFunction = std::function<Clock::time_point()>;
    static constexpr std::size_t title_capacity  = 96;
    static constexpr std::size_t url_capacity    = RadioModel::maximum_url_length + 1U;
    static constexpr std::size_t status_capacity = 128;

    explicit RadioViewModel(StreamPlayerInterface &player, std::string config_path = RadioModel::default_config_path(),
                            NowFunction now = Clock::now);
    RadioViewModel(StreamPlayerInterface &player, RadioBrowserClientInterface &directory,
                   std::string config_path = RadioModel::default_config_path(), NowFunction now = Clock::now);

    [[nodiscard]] const std::vector<RadioStation> &stations() const noexcept;
    [[nodiscard]] std::size_t available_station_count() const noexcept;
    // These commands start a request and return immediately. poll() applies the result.
    bool load_radio_browser_stations();
    bool search_stations(std::string_view query);
    bool refresh_stations();
    bool load_more_stations();
    [[nodiscard]] DirectoryState directory_state() const noexcept;
    [[nodiscard]] const std::string &directory_status_text() const noexcept;
    [[nodiscard]] const std::string &search_query() const noexcept;
    [[nodiscard]] bool has_more_stations() const noexcept;
    [[nodiscard]] int stations_revision() const noexcept;
    [[nodiscard]] int selected_index() const noexcept;
    [[nodiscard]] bool has_selection() const noexcept;
    [[nodiscard]] const std::string &current_name() const noexcept;
    [[nodiscard]] const std::string &current_url() const noexcept;
    [[nodiscard]] const std::string &current_metadata() const noexcept;
    [[nodiscard]] const std::string &custom_url() const noexcept;
    [[nodiscard]] const std::string &status_text() const noexcept;
    [[nodiscard]] const std::string &input_error() const noexcept;
    [[nodiscard]] StreamPlaybackState playback_state() const noexcept;
    [[nodiscard]] const RadioStation &current_station() const noexcept;
    [[nodiscard]] bool is_current_station(const RadioStation &station) const noexcept;
    [[nodiscard]] const std::vector<RadioStation> &favorites() const noexcept;
    [[nodiscard]] bool is_favorite(const RadioStation &station) const noexcept;
    [[nodiscard]] bool current_is_favorite() const noexcept;
    [[nodiscard]] int favorites_revision() const noexcept;
    [[nodiscard]] const std::string &favorite_error() const noexcept;
    bool toggle_favorite(const RadioStation &station);
    bool toggle_current_favorite();
    bool play_favorite(std::size_t index);
    bool set_sleep_timer(int minutes);
    void cancel_sleep_timer() noexcept;
    [[nodiscard]] int sleep_remaining_seconds() const noexcept;
    [[nodiscard]] bool sleep_timer_active() const noexcept;

    bool play_station(std::size_t index);
    bool play_custom(std::string_view url);
    void toggle_playback();
    void stop() noexcept;
    void poll() noexcept;

    [[nodiscard]] lv_subject_t *title_subject() noexcept;
    [[nodiscard]] lv_subject_t *url_subject() noexcept;
    [[nodiscard]] lv_subject_t *status_subject() noexcept;
    [[nodiscard]] lv_subject_t *state_subject() noexcept;
    [[nodiscard]] lv_subject_t *selected_subject() noexcept;
    [[nodiscard]] lv_subject_t *directory_subject() noexcept;
    [[nodiscard]] lv_subject_t *directory_state_subject() noexcept;
    [[nodiscard]] lv_subject_t *stations_revision_subject() noexcept;

private:
    RadioViewModel(StreamPlayerInterface &player, std::string config_path,
                   std::unique_ptr<RadioBrowserClientInterface> owned_directory,
                   RadioBrowserClientInterface *directory, NowFunction now);
    static const char *validation_message(StreamUrlStatus status) noexcept;
    bool request_stations(std::string query, bool append);
    bool poll_directory() noexcept;
    void update_from_player() noexcept;
    void publish() noexcept;

    RadioModel model_;
    StreamPlayerInterface &player_;
    std::unique_ptr<RadioBrowserClientInterface> owned_directory_;
    RadioBrowserClientInterface &directory_;
    std::string status_text_ = "Ready";
    std::string input_error_;
    StreamPlaybackState playback_state_ = StreamPlaybackState::Idle;
    DirectoryState directory_state_ = DirectoryState::Idle;
    std::string directory_status_text_ = "Saved stations · Connect to Radio Browser for more";
    std::string search_query_;
    bool append_request_ = false;
    bool has_more_stations_ = false;
    bool has_loaded_directory_ = false;
    std::size_t next_offset_ = 0;
    int stations_revision_ = 0;
    NowFunction now_;
    std::optional<Clock::time_point> sleep_deadline_;

    StringSubject<title_capacity> title_;
    StringSubject<url_capacity> url_;
    StringSubject<status_capacity> status_;
    IntSubject state_;
    IntSubject selected_;
    StringSubject<status_capacity> directory_status_;
    IntSubject directory_state_value_;
    IntSubject stations_revision_value_;
};

#endif
