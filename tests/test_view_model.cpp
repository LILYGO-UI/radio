#include "pages/radio/radio_view_model.hpp"

#include <cassert>
#include <string>
#include <utility>

namespace {

class FakeStreamPlayer final : public StreamPlayerInterface {
public:
    bool start(std::string_view url) override
    {
        started_url = url;
        if (!start_result) {
            playback_state = StreamPlaybackState::Error;
            error_text     = "Simulated playback failure";
            return false;
        }
        playback_state = StreamPlaybackState::Starting;
        return true;
    }

    void stop() noexcept override
    {
        ++stop_calls;
        playback_state = StreamPlaybackState::Stopped;
    }

    bool toggle_pause() noexcept override
    {
        if (playback_state == StreamPlaybackState::Paused) {
            playback_state = StreamPlaybackState::Playing;
            return true;
        }
        if (playback_state == StreamPlaybackState::Playing || playback_state == StreamPlaybackState::Starting) {
            playback_state = StreamPlaybackState::Paused;
            return true;
        }
        return false;
    }

    void poll() noexcept override
    {
    }

    [[nodiscard]] StreamPlaybackState state() const noexcept override
    {
        return playback_state;
    }

    [[nodiscard]] const std::string &error() const noexcept override
    {
        return error_text;
    }

    bool start_result                  = true;
    int stop_calls = 0;
    StreamPlaybackState playback_state = StreamPlaybackState::Idle;
    std::string started_url;
    std::string error_text;
};

class FakeRadioBrowser final : public RadioBrowserClientInterface {
public:
    bool search(const RadioBrowserQuery &query) override
    {
        last_query = query;
        ready = false;
        busy = true;
        if (!start_result) {
            finish({{}, "Unable to start request", false});
            return false;
        }
        return true;
    }

    bool poll(RadioBrowserResult &result) override
    {
        if (!ready) return false;
        result = std::move(completed);
        ready = false;
        return true;
    }

    bool loading() const noexcept override { return busy; }

    void finish(RadioBrowserResult result)
    {
        completed = std::move(result);
        ready = true;
        busy = false;
    }

    RadioBrowserQuery last_query;
    RadioBrowserResult completed;
    bool ready = false;
    bool busy = false;
    bool start_result = true;
};

RadioStation station(std::string name, std::string id)
{
    RadioStation result;
    result.name = std::move(name);
    result.url = "https://radio.example.com/" + id;
    result.uuid = std::move(id);
    result.country = "France";
    result.codec = "MP3";
    result.bitrate = 128;
    return result;
}

void test_playback_commands_and_subjects()
{
    FakeStreamPlayer player;
    RadioViewModel view_model(player, "");
    assert(view_model.selected_index() == -2);
    assert(!view_model.has_selection());
    assert(std::string(lv_subject_get_string(view_model.title_subject())) == "Select a station");
    assert(std::string(lv_subject_get_string(view_model.status_subject())) == "Ready");

    assert(view_model.play_station(2));
    assert(view_model.selected_index() == 2);
    assert(player.started_url == view_model.stations()[2].url);
    assert(view_model.playback_state() == StreamPlaybackState::Starting);
    assert(std::string(lv_subject_get_string(view_model.title_subject())) == "Deep Space One");
    assert(std::string(lv_subject_get_string(view_model.status_subject())) == "Connecting...");
    assert(lv_subject_get_int(view_model.selected_subject()) == 2);

    player.playback_state = StreamPlaybackState::Playing;
    view_model.poll();
    assert(view_model.status_text() == "Playing");
    view_model.toggle_playback();
    assert(view_model.playback_state() == StreamPlaybackState::Paused);
    assert(view_model.status_text() == "Paused");

    assert(view_model.play_custom("https://radio.example.com/live.ogg"));
    assert(view_model.selected_index() == -1);
    assert(player.started_url == "https://radio.example.com/live.ogg");
    assert(!view_model.play_custom("file:///tmp/test.mp3"));
    assert(view_model.selected_index() == -1);
    assert(view_model.input_error() == "URL must start with http:// or https://");
    assert(view_model.status_text() == "Connecting...");

    view_model.stop();
    assert(view_model.playback_state() == StreamPlaybackState::Stopped);
    assert(view_model.status_text() == "Stopped");
}

void test_backend_error_is_published()
{
    FakeStreamPlayer player;
    player.start_result = false;
    RadioViewModel view_model(player, "");
    assert(!view_model.play_station(0));
    assert(view_model.playback_state() == StreamPlaybackState::Error);
    assert(view_model.status_text() == "Simulated playback failure");
    assert(std::string(lv_subject_get_string(view_model.status_subject())) == "Simulated playback failure");
}

void test_async_directory_search_pagination_and_playback()
{
    FakeStreamPlayer player;
    FakeRadioBrowser directory;
    RadioViewModel view_model(player, directory, "");
    assert(view_model.load_radio_browser_stations());
    assert(view_model.directory_state() == DirectoryState::Loading);
    assert(view_model.stations_revision() == 0);
    assert(view_model.available_station_count() == RadioModel::station_count);
    assert(directory.last_query.name.empty());
    assert(directory.last_query.offset == 0);
    view_model.poll();
    assert(view_model.directory_state() == DirectoryState::Loading);

    directory.finish({{station("Jazz Paris", "jazz"), station("Ambient", "ambient")}, {}, true});
    view_model.poll();
    assert(view_model.directory_state() == DirectoryState::Ready);
    assert(view_model.available_station_count() == 2);
    assert(view_model.has_more_stations());
    assert(view_model.stations_revision() == 1);
    assert(lv_subject_get_int(view_model.stations_revision_subject()) == 1);
    assert(view_model.play_station(0));
    const std::string stream_url = player.started_url;
    assert(view_model.current_metadata() == "France · MP3 · 128 kbps");

    assert(view_model.load_more_stations());
    assert(directory.last_query.offset == 50);
    assert(!view_model.load_more_stations());
    directory.finish({{}, "Temporary network error", false});
    view_model.poll();
    assert(view_model.directory_state() == DirectoryState::Error);
    assert(view_model.available_station_count() == 2);
    assert(view_model.load_more_stations());
    assert(directory.last_query.offset == 50);
    directory.finish({{station("Ambient", "ambient"), station("News", "news")}, {}, false});
    view_model.poll();
    assert(view_model.available_station_count() == 3);
    assert(view_model.selected_index() == 0);
    assert(!view_model.has_more_stations());

    assert(view_model.search_stations("  news  "));
    assert(view_model.search_query() == "news");
    assert(directory.last_query.name == "news");
    assert(directory.last_query.offset == 0);
    directory.finish({{station("News", "news")}, {}, false});
    view_model.poll();
    assert(view_model.current_name() == "Jazz Paris");
    assert(view_model.current_url() == stream_url);
    assert(view_model.selected_index() == -2);
    assert(view_model.status_text() == "Connecting...");
    view_model.stop();
    view_model.toggle_playback();
    assert(player.started_url == stream_url);

    assert(view_model.refresh_stations());
    assert(directory.last_query.name == "news");
    directory.finish({{}, "Radio Browser is unavailable", false});
    view_model.poll();
    assert(view_model.directory_state() == DirectoryState::Error);
    assert(view_model.available_station_count() == 1);
    assert(view_model.stations_revision() == 3);
    assert(std::string(lv_subject_get_string(view_model.directory_subject())) == "Radio Browser is unavailable");

    assert(view_model.search_stations("nonexistent"));
    directory.finish({});
    view_model.poll();
    assert(view_model.directory_state() == DirectoryState::Empty);
    assert(view_model.stations().empty());
    assert(view_model.has_selection());
    assert(view_model.current_name() == "Jazz Paris");
    assert(!view_model.play_station(0));
}

void test_directory_failure_retains_saved_stations()
{
    FakeStreamPlayer player;
    FakeRadioBrowser directory;
    RadioViewModel view_model(player, directory, "");
    assert(view_model.load_radio_browser_stations());
    directory.finish({{}, "Unable to connect", false});
    view_model.poll();
    assert(view_model.directory_state() == DirectoryState::Error);
    assert(view_model.available_station_count() == RadioModel::station_count);
    assert(view_model.directory_status_text().find("Saved stations") != std::string::npos);
    assert(view_model.play_station(0));
    directory.start_result = false;
    assert(!view_model.refresh_stations());
    assert(view_model.directory_state() == DirectoryState::Error);
    assert(view_model.directory_status_text().find("Unable to start request") != std::string::npos);
    assert(view_model.available_station_count() == RadioModel::station_count);
}

void test_long_station_title_preserves_utf8_characters()
{
    FakeStreamPlayer player;
    FakeRadioBrowser directory;
    RadioViewModel view_model(player, directory, "");
    std::string name;
    for (int index = 0; index < 80; ++index) name += "电";
    assert(view_model.load_radio_browser_stations());
    directory.finish({{station(name, "chinese")}, {}, false});
    view_model.poll();
    assert(view_model.play_station(0));
    assert(view_model.current_name() == name);
    // The 96-byte title buffer holds 31 complete three-byte Chinese characters.
    std::string expected;
    for (int index = 0; index < 31; ++index) expected += "电";
    assert(std::string(lv_subject_get_string(view_model.title_subject())) == expected);
}

void test_favorites_survive_search_and_play_without_directory()
{
    FakeStreamPlayer player;
    FakeRadioBrowser directory;
    RadioViewModel view_model(player, directory, "");
    assert(!view_model.toggle_current_favorite());
    auto favorite = station("Current station", "favorite");
    favorite.favicon = "https://radio.example.com/logo.png";
    assert(view_model.search_stations("Current"));
    directory.finish({{favorite}, {}, false});
    view_model.poll();
    assert(view_model.play_station(0));
    assert(view_model.toggle_current_favorite());
    assert(view_model.current_is_favorite());
    assert(view_model.favorites_revision() == 1);
    assert(view_model.search_stations("Another"));
    directory.finish({{station("Another station", "another")}, {}, false});
    view_model.poll();
    assert(view_model.current_station().favicon == favorite.favicon);
    assert(view_model.current_is_favorite());
    assert(view_model.is_current_station(view_model.favorites()[0]));
    assert(view_model.play_station(0));
    assert(!view_model.current_is_favorite());
    assert(view_model.play_favorite(0));
    assert(player.started_url == favorite.url);
    assert(view_model.current_station().favicon == favorite.favicon);
    assert(view_model.selected_index() == -2);
    assert(!view_model.play_favorite(1));
    assert(view_model.toggle_favorite(favorite));
    assert(!view_model.current_is_favorite());
    assert(view_model.favorites().empty());
    assert(view_model.favorites_revision() == 2);
    assert(view_model.favorite_error().empty());
    assert(view_model.play_custom("https://radio.example.com/custom"));
    assert(view_model.current_station().favicon.empty());
    assert(!view_model.current_is_favorite());
}

void test_sleep_timer_expiry_cancellation_and_reset()
{
    FakeStreamPlayer player;
    FakeRadioBrowser directory;
    RadioViewModel::Clock::time_point now{};
    RadioViewModel view_model(player, directory, "", [&] { return now; });
    assert(!view_model.sleep_timer_active());
    assert(view_model.sleep_remaining_seconds() == 0);
    for (const int minutes : {15, 30, 45, 60, 90}) {
        assert(view_model.set_sleep_timer(minutes));
        assert(view_model.sleep_remaining_seconds() == minutes * 60);
    }
    assert(!view_model.set_sleep_timer(-1));
    assert(!view_model.set_sleep_timer(1));
    assert(view_model.sleep_remaining_seconds() == 90 * 60);
    assert(view_model.set_sleep_timer(15));
    now += std::chrono::milliseconds(1);
    assert(view_model.sleep_remaining_seconds() == 900);
    now += std::chrono::seconds(899);
    assert(view_model.sleep_remaining_seconds() == 1);
    assert(view_model.play_station(0));
    view_model.poll();
    assert(player.stop_calls == 0);
    now += std::chrono::milliseconds(999);
    view_model.poll();
    assert(player.stop_calls == 1);
    assert(view_model.playback_state() == StreamPlaybackState::Stopped);
    assert(!view_model.sleep_timer_active());
    assert(view_model.sleep_remaining_seconds() == 0);
    view_model.poll();
    assert(player.stop_calls == 1);

    assert(view_model.play_station(1));
    view_model.toggle_playback();
    assert(view_model.playback_state() == StreamPlaybackState::Paused);
    assert(view_model.set_sleep_timer(30));
    now += std::chrono::minutes(29);
    assert(view_model.set_sleep_timer(15));
    assert(view_model.sleep_remaining_seconds() == 900);
    now += std::chrono::minutes(15);
    view_model.poll();
    assert(player.stop_calls == 2);
    assert(view_model.playback_state() == StreamPlaybackState::Stopped);

    assert(view_model.play_station(0));
    assert(view_model.set_sleep_timer(15));
    view_model.cancel_sleep_timer();
    now += std::chrono::hours(1);
    view_model.poll();
    assert(player.stop_calls == 2);
    assert(view_model.set_sleep_timer(30));
    assert(view_model.set_sleep_timer(0));
    assert(!view_model.sleep_timer_active());
    assert(view_model.set_sleep_timer(45));
    view_model.stop();
    assert(player.stop_calls == 3);
    assert(!view_model.sleep_timer_active());
    now += std::chrono::hours(2);
    view_model.poll();
    assert(player.stop_calls == 3);
}

}  // namespace

int main()
{
    lv_init();
    {
        test_playback_commands_and_subjects();
        test_backend_error_is_published();
        test_async_directory_search_pagination_and_playback();
        test_directory_failure_retains_saved_stations();
        test_long_station_title_preserves_utf8_characters();
        test_favorites_survive_search_and_play_without_directory();
        test_sleep_timer_expiry_cancellation_and_reset();
    }
    lv_deinit();
    return 0;
}
