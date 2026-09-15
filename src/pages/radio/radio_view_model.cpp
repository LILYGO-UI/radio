#include "pages/radio/radio_view_model.hpp"

#include <cctype>
#include <limits>
#include <utility>

namespace {
constexpr std::size_t kPageSize = 50;
}

RadioViewModel::RadioViewModel(StreamPlayerInterface &player, std::string config_path, NowFunction now)
    : RadioViewModel(player, std::move(config_path), std::make_unique<RadioBrowserClient>(), nullptr, std::move(now))
{
}

RadioViewModel::RadioViewModel(StreamPlayerInterface &player, RadioBrowserClientInterface &directory,
                               std::string config_path, NowFunction now)
    : RadioViewModel(player, std::move(config_path), nullptr, &directory, std::move(now))
{
}

RadioViewModel::RadioViewModel(StreamPlayerInterface &player, std::string config_path,
                               std::unique_ptr<RadioBrowserClientInterface> owned_directory,
                               RadioBrowserClientInterface *directory, NowFunction now)
    : model_(std::move(config_path)),
      player_(player),
      owned_directory_(std::move(owned_directory)),
      directory_(owned_directory_ ? *owned_directory_ : *directory),
      now_(now ? std::move(now) : NowFunction(Clock::now)),
      title_(model_.current_name()),
      url_(model_.current_url()),
      status_(status_text_),
      state_(static_cast<int>(playback_state_), static_cast<int>(StreamPlaybackState::Idle),
             static_cast<int>(StreamPlaybackState::Error)),
      selected_(model_.selected_index(), -2, std::numeric_limits<int>::max()),
      directory_status_(directory_status_text_),
      directory_state_value_(static_cast<int>(directory_state_), static_cast<int>(DirectoryState::Idle),
                             static_cast<int>(DirectoryState::Error)),
      stations_revision_value_(0)
{
}

const std::vector<RadioStation> &RadioViewModel::stations() const noexcept
{
    return model_.stations();
}

std::size_t RadioViewModel::available_station_count() const noexcept
{
    return model_.available_station_count();
}

bool RadioViewModel::load_radio_browser_stations()
{
    return request_stations({}, false);
}

bool RadioViewModel::search_stations(std::string_view query)
{
    while (!query.empty() && std::isspace(static_cast<unsigned char>(query.front())) != 0) query.remove_prefix(1);
    while (!query.empty() && std::isspace(static_cast<unsigned char>(query.back())) != 0) query.remove_suffix(1);
    return request_stations(std::string(query), false);
}

bool RadioViewModel::refresh_stations()
{
    return request_stations(search_query_, false);
}

bool RadioViewModel::load_more_stations()
{
    if (!has_more_stations_ || directory_state_ == DirectoryState::Loading) return false;
    return request_stations(search_query_, true);
}

DirectoryState RadioViewModel::directory_state() const noexcept
{
    return directory_state_;
}

const std::string &RadioViewModel::directory_status_text() const noexcept
{
    return directory_status_text_;
}

const std::string &RadioViewModel::search_query() const noexcept
{
    return search_query_;
}

bool RadioViewModel::has_more_stations() const noexcept
{
    return has_more_stations_;
}

int RadioViewModel::stations_revision() const noexcept
{
    return stations_revision_;
}

bool RadioViewModel::request_stations(std::string query, bool append)
{
    search_query_ = std::move(query);
    append_request_ = append;
    if (!append) {
        next_offset_ = 0;
        has_more_stations_ = false;
    }
    directory_state_ = DirectoryState::Loading;
    directory_status_text_ = append ? "Loading more stations…" : "Loading Radio Browser…";
    const bool started = directory_.search({search_query_, next_offset_, kPageSize});
    if (!started && !poll_directory()) {
        directory_state_ = DirectoryState::Error;
        directory_status_text_ = "Could not start Radio Browser. Please try again.";
    }
    publish();
    return started;
}

bool RadioViewModel::poll_directory() noexcept
{
    RadioBrowserResult result;
    if (!directory_.poll(result)) return false;
    if (!result.error.empty()) {
        directory_state_ = DirectoryState::Error;
        directory_status_text_ = result.error;
        if (!has_loaded_directory_) directory_status_text_ += " · Saved stations are available";
        return true;
    }
    model_.set_stations(std::move(result.stations), append_request_);
    next_offset_ += kPageSize;
    has_more_stations_ = result.has_more;
    has_loaded_directory_ = true;
    stations_revision_ = stations_revision_ == std::numeric_limits<int>::max() ? 0 : stations_revision_ + 1;
    if (model_.stations().empty()) {
        directory_state_ = DirectoryState::Empty;
        directory_status_text_ = "No stations found. Try another name.";
    } else {
        directory_state_ = DirectoryState::Ready;
        directory_status_text_ = std::to_string(model_.available_station_count()) + " stations · Radio Browser";
    }
    return true;
}

int RadioViewModel::selected_index() const noexcept
{
    return model_.selected_index();
}

bool RadioViewModel::has_selection() const noexcept
{
    return model_.has_selection();
}

const std::string &RadioViewModel::current_name() const noexcept
{
    return model_.current_name();
}

const std::string &RadioViewModel::current_url() const noexcept
{
    return model_.current_url();
}

const std::string &RadioViewModel::current_metadata() const noexcept
{
    return model_.current_metadata();
}

const std::string &RadioViewModel::custom_url() const noexcept
{
    return model_.custom_url();
}

const std::string &RadioViewModel::status_text() const noexcept
{
    return status_text_;
}

const std::string &RadioViewModel::input_error() const noexcept
{
    return input_error_;
}

StreamPlaybackState RadioViewModel::playback_state() const noexcept
{
    return playback_state_;
}

const RadioStation &RadioViewModel::current_station() const noexcept
{
    return model_.current_station();
}

bool RadioViewModel::is_current_station(const RadioStation &station) const noexcept
{
    return model_.is_current_station(station);
}

const std::vector<RadioStation> &RadioViewModel::favorites() const noexcept
{
    return model_.favorites();
}

bool RadioViewModel::is_favorite(const RadioStation &station) const noexcept
{
    return model_.is_favorite(station);
}

bool RadioViewModel::current_is_favorite() const noexcept
{
    return model_.has_selection() && model_.is_favorite(model_.current_station());
}

int RadioViewModel::favorites_revision() const noexcept
{
    return model_.favorites_revision();
}

const std::string &RadioViewModel::favorite_error() const noexcept
{
    return model_.favorite_error();
}

bool RadioViewModel::toggle_favorite(const RadioStation &station)
{
    return model_.toggle_favorite(station);
}

bool RadioViewModel::toggle_current_favorite()
{
    if (!model_.has_selection()) return false;
    return model_.toggle_favorite(model_.current_station());
}

bool RadioViewModel::play_favorite(std::size_t index)
{
    if (!model_.select_favorite(index)) return false;
    const bool started = player_.start(model_.current_url());
    update_from_player();
    publish();
    return started;
}

bool RadioViewModel::set_sleep_timer(int minutes)
{
    switch (minutes) {
        case 0:
            cancel_sleep_timer();
            return true;
        case 15:
        case 30:
        case 45:
        case 60:
        case 90:
            sleep_deadline_ = now_() + std::chrono::minutes(minutes);
            return true;
        default:
            return false;
    }
}

void RadioViewModel::cancel_sleep_timer() noexcept
{
    sleep_deadline_.reset();
}

int RadioViewModel::sleep_remaining_seconds() const noexcept
{
    if (!sleep_deadline_) return 0;
    const auto remaining = std::chrono::ceil<std::chrono::seconds>(*sleep_deadline_ - now_()).count();
    return remaining > 0 ? static_cast<int>(remaining) : 0;
}

bool RadioViewModel::sleep_timer_active() const noexcept
{
    return sleep_deadline_.has_value();
}

bool RadioViewModel::play_station(std::size_t index)
{
    if (!model_.select_station(index)) return false;
    if (!player_.start(model_.current_url())) {
        update_from_player();
        publish();
        return false;
    }
    update_from_player();
    publish();
    return true;
}

bool RadioViewModel::play_custom(std::string_view url)
{
    const StreamUrlStatus result = model_.select_custom(url);
    if (result != StreamUrlStatus::Valid) {
        input_error_ = validation_message(result);
        return false;
    }
    input_error_.clear();
    if (!player_.start(model_.current_url())) {
        update_from_player();
        publish();
        return false;
    }
    update_from_player();
    publish();
    return true;
}

void RadioViewModel::toggle_playback()
{
    if (playback_state_ == StreamPlaybackState::Playing || playback_state_ == StreamPlaybackState::Starting ||
        playback_state_ == StreamPlaybackState::Paused) {
        (void)player_.toggle_pause();
        update_from_player();
        publish();
        return;
    }

    if (model_.has_selection()) {
        (void)player_.start(model_.current_url());
        update_from_player();
        publish();
    } else {
        (void)play_station(0);
    }
}

void RadioViewModel::stop() noexcept
{
    cancel_sleep_timer();
    player_.stop();
    update_from_player();
    publish();
}

void RadioViewModel::poll() noexcept
{
    player_.poll();
    const bool directory_changed = poll_directory();
    if (sleep_deadline_ && now_() >= *sleep_deadline_) {
        stop();
        return;
    }
    const bool player_changed = player_.state() != playback_state_;
    if (player_changed) update_from_player();
    if (directory_changed || player_changed) publish();
}

lv_subject_t *RadioViewModel::title_subject() noexcept
{
    return title_.get();
}

lv_subject_t *RadioViewModel::url_subject() noexcept
{
    return url_.get();
}

lv_subject_t *RadioViewModel::status_subject() noexcept
{
    return status_.get();
}

lv_subject_t *RadioViewModel::state_subject() noexcept
{
    return state_.get();
}

lv_subject_t *RadioViewModel::selected_subject() noexcept
{
    return selected_.get();
}

lv_subject_t *RadioViewModel::directory_subject() noexcept
{
    return directory_status_.get();
}

lv_subject_t *RadioViewModel::directory_state_subject() noexcept
{
    return directory_state_value_.get();
}

lv_subject_t *RadioViewModel::stations_revision_subject() noexcept
{
    return stations_revision_value_.get();
}

const char *RadioViewModel::validation_message(StreamUrlStatus status) noexcept
{
    switch (status) {
        case StreamUrlStatus::Empty:
            return "Enter an audio stream URL";
        case StreamUrlStatus::TooLong:
            return "URL is too long";
        case StreamUrlStatus::UnsupportedScheme:
            return "URL must start with http:// or https://";
        case StreamUrlStatus::InvalidCharacter:
            return "URL cannot contain spaces or control characters";
        case StreamUrlStatus::MissingHost:
            return "URL is missing a hostname";
        case StreamUrlStatus::Valid:
            break;
    }
    return "Invalid URL";
}

void RadioViewModel::update_from_player() noexcept
{
    playback_state_ = player_.state();
    switch (playback_state_) {
        case StreamPlaybackState::Idle:
            status_text_ = "Ready";
            break;
        case StreamPlaybackState::Starting:
            status_text_ = "Connecting...";
            break;
        case StreamPlaybackState::Playing:
            status_text_ = "Playing";
            break;
        case StreamPlaybackState::Paused:
            status_text_ = "Paused";
            break;
        case StreamPlaybackState::Stopped:
            status_text_ = "Stopped";
            break;
        case StreamPlaybackState::Error:
            status_text_ = player_.error().empty() ? "Playback failed" : player_.error();
            break;
    }
}

void RadioViewModel::publish() noexcept
{
    title_.set(model_.current_name());
    url_.set(model_.current_url());
    status_.set(status_text_);
    state_.set(static_cast<int>(playback_state_));
    selected_.set(model_.selected_index());
    directory_status_.set(directory_status_text_);
    directory_state_value_.set(static_cast<int>(directory_state_));
    stations_revision_value_.set(stations_revision_);
}
