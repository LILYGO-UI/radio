#include "pages/radio/radio_model.hpp"
#include "services/stream_player.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <unistd.h>

namespace {

void test_stations_and_selection()
{
    RadioModel model("");
    assert(model.stations().size() == RadioModel::station_count);
    assert(model.selected_index() == -2);
    assert(!model.select_station(RadioModel::station_count));
    assert(model.select_station(1));
    assert(model.selected_index() == 1);
    assert(model.current_name() == "Drone Zone");
    assert(model.current_url().find("http://") == 0);
}

void test_url_validation()
{
    assert(RadioModel::validate_stream_url("https://example.com/live.mp3") == StreamUrlStatus::Valid);
    assert(RadioModel::validate_stream_url("HTTP://radio.example.com:8000/live") == StreamUrlStatus::Valid);
    assert(RadioModel::validate_stream_url("") == StreamUrlStatus::Empty);
    assert(RadioModel::validate_stream_url("ftp://example.com/live") == StreamUrlStatus::UnsupportedScheme);
    assert(RadioModel::validate_stream_url("http:///live") == StreamUrlStatus::MissingHost);
    assert(RadioModel::validate_stream_url("https://example.com/live stream") == StreamUrlStatus::InvalidCharacter);
    assert(RadioModel::validate_stream_url("https://example.com/\nstream") == StreamUrlStatus::InvalidCharacter);

    std::string long_url = "https://example.com/";
    long_url.append(RadioModel::maximum_url_length, 'a');
    assert(RadioModel::validate_stream_url(long_url) == StreamUrlStatus::TooLong);
}

void test_directory_replacement_preserves_current_stream()
{
    RadioModel model("");
    assert(model.select_station(1));
    const std::string current_url = model.current_url();
    const std::string metadata = model.current_metadata();
    std::vector<RadioStation> stations;
    for (int index = 0; index < 25; ++index) {
        RadioStation station;
        station.name = "Station " + std::to_string(index);
        station.url = "https://radio.example.com/" + std::to_string(index);
        station.uuid = "uuid-" + std::to_string(index);
        stations.push_back(std::move(station));
    }
    model.set_stations(stations);
    assert(model.available_station_count() == 25);
    assert(model.has_selection());
    assert(model.selected_index() == -2);
    assert(model.current_name() == "Drone Zone");
    assert(model.current_url() == current_url);
    assert(model.current_metadata() == metadata);

    assert(model.select_station(24));
    RadioStation selected = model.stations()[24];
    model.set_stations({selected});
    assert(model.selected_index() == 0);
    model.set_stations({selected}, true);
    assert(model.available_station_count() == 1);
    model.set_stations({});
    assert(model.available_station_count() == 0);
    assert(model.current_name() == "Station 24");
    assert(!model.select_station(0));

    assert(model.select_custom("https://radio.example.com/custom") == StreamUrlStatus::Valid);
    model.set_stations(stations);
    assert(model.selected_index() == -1);
    assert(model.current_url() == "https://radio.example.com/custom");
}

void test_custom_url_persistence()
{
    const std::string path = "/tmp/lilygo-ui-radio-core-" + std::to_string(getpid()) + ".conf";
    (void)std::remove(path.c_str());
    (void)std::remove((path + ".tmp").c_str());

    {
        RadioModel model(path);
        assert(model.custom_url().empty());
        assert(model.select_custom("  https://radio.example.com/live.aac  ") == StreamUrlStatus::Valid);
        assert(model.selected_index() == -1);
        assert(model.current_name() == "Custom audio stream");
        assert(model.current_url() == "https://radio.example.com/live.aac");
    }
    {
        RadioModel model(path);
        assert(model.custom_url() == "https://radio.example.com/live.aac");
        assert(model.selected_index() == -2);
    }

    (void)std::remove(path.c_str());
}

void test_favorite_snapshots_and_identity()
{
    RadioModel model("");
    RadioStation station = model.stations()[0];
    station.uuid = "favorite-station";
    station.favicon = "https://radio.example.com/logo.png";
    model.set_stations({station});
    assert(model.select_station(0));
    assert(model.toggle_favorite(station));
    assert(model.is_favorite(station));
    assert(model.favorites_revision() == 1);
    model.set_stations({});
    assert(model.current_station().favicon == station.favicon);
    assert(model.is_current_station(station));
    assert(model.is_favorite(model.current_station()));
    assert(model.select_favorite(0));
    assert(model.selected_index() == -2);
    assert(model.current_station().uuid == station.uuid);
    assert(!model.select_favorite(1));
    RadioStation same_id = station;
    same_id.url = "https://radio.example.com/new-url";
    assert(model.is_favorite(same_id));
    assert(model.toggle_favorite(same_id));
    assert(model.favorites().empty());
    assert(model.toggle_favorite(station));
    RadioStation same_url = station;
    same_url.uuid = "different-id";
    assert(model.is_favorite(same_url));
    assert(model.toggle_favorite(same_url));
    assert(model.favorites().empty());
    assert(model.select_custom("https://radio.example.com/custom") == StreamUrlStatus::Valid);
    assert(model.current_station().favicon.empty());
    assert(model.current_station().uuid.empty());
    assert(model.toggle_favorite(model.current_station()));
    assert(model.select_favorite(0));
    assert(model.current_url() == "https://radio.example.com/custom");
    assert(model.favorite_error().empty());
}

void test_favorites_persistence_and_failed_write_rollback()
{
    const std::string directory = "/tmp/lilygo-radio-favorites-" + std::to_string(getpid());
    std::filesystem::create_directories(directory);
    const std::string config = directory + "/radio.conf";
    RadioStation station;
    station.name = "电台 \"Jazz\" \\ Live\nParis";
    station.url = "https://radio.example.com/live";
    station.uuid = "uuid-123";
    station.country = "France";
    station.codec = "AAC";
    station.bitrate = 192;
    station.tags = "jazz,\"chill\"";
    station.country_code = "FR";
    station.language = "français";
    station.favicon = "https://radio.example.com/logo.png?size=256&style=light";
    {
        RadioModel model(config);
        assert(model.select_custom("https://radio.example.com/custom") == StreamUrlStatus::Valid);
        assert(model.toggle_favorite(station));
    }
    {
        RadioModel model(config);
        assert(model.custom_url() == "https://radio.example.com/custom");
        assert(model.favorites().size() == 1);
        const auto &saved = model.favorites()[0];
        assert(saved.name == station.name);
        assert(saved.url == station.url);
        assert(saved.country == station.country);
        assert(saved.codec == station.codec);
        assert(saved.bitrate == station.bitrate);
        assert(saved.uuid == station.uuid);
        assert(saved.tags == station.tags);
        assert(saved.country_code == station.country_code);
        assert(saved.language == station.language);
        assert(saved.favicon == station.favicon);
        assert(model.select_favorite(0));
        assert(model.current_station().favicon == station.favicon);

        // Replacing the destination with a directory forces rename to fail even when tests run as root.
        std::filesystem::rename(config + ".favorites", config + ".favorites.saved");
        std::filesystem::create_directory(config + ".favorites");
        assert(!model.toggle_favorite(station));
        assert(model.favorites().size() == 1);
        assert(model.is_favorite(station));
        assert(model.favorites_revision() == 0);
        assert(!model.favorite_error().empty());
        std::filesystem::remove(config + ".favorites");
        std::filesystem::rename(config + ".favorites.saved", config + ".favorites");
        assert(model.toggle_favorite(station));
        assert(model.favorites_revision() == 1);
        assert(model.favorite_error().empty());
    }
    assert(RadioModel(config).favorites().empty());
    std::filesystem::remove_all(directory);
}

void test_invalid_favorites_are_bounded_and_not_partially_loaded()
{
    const std::string config = "/tmp/lilygo-radio-invalid-favorites-" + std::to_string(getpid());
    const std::string favorites = config + ".favorites";
    for (const std::string invalid : {"LILYGO_RADIO_FAVORITES 1 999999999\n", "LILYGO_RADIO_FAVORITES 2 0\n",
                                      "LILYGO_RADIO_FAVORITES 1 1\n\"truncated"}) {
        {
            std::ofstream file(favorites);
            file << invalid;
        }
        RadioModel model(config);
        assert(model.favorites().empty());
        assert(!model.favorite_error().empty());
    }
    std::filesystem::resize_file(favorites, 4U * 1024U * 1024U + 1U);
    assert(!RadioModel(config).favorite_error().empty());
    std::filesystem::remove(favorites);
    RadioModel model("");
    RadioStation invalid = model.stations()[0];
    invalid.name.assign(1025, 'x');
    assert(!model.toggle_favorite(invalid));
    assert(model.favorites().empty());
    for (std::size_t index = 0; index < RadioModel::maximum_favorites; ++index) {
        RadioStation entry = model.stations()[0];
        entry.url = "https://radio.example.com/" + std::to_string(index);
        assert(model.toggle_favorite(entry));
    }
    assert(!model.toggle_favorite(model.stations()[0]));
    assert(model.favorites().size() == RadioModel::maximum_favorites);
}

void test_missing_player_dependencies()
{
    const char *current_path       = std::getenv("PATH");
    const std::string saved_path   = current_path ? current_path : "";
    const bool path_was_configured = current_path != nullptr;
    assert(setenv("PATH", "/path/that/does/not/exist", 1) == 0);

    StreamPlayer player;
    assert(!player.start("https://radio.example.com/live.mp3"));
    assert(player.state() == StreamPlaybackState::Error);
    assert(player.error() == "ffmpeg not found");

    if (path_was_configured) {
        assert(setenv("PATH", saved_path.c_str(), 1) == 0);
    } else {
        assert(unsetenv("PATH") == 0);
    }
}

void test_player_process_lifecycle()
{
    const std::string directory = "/tmp/lilygo-ui-radio-player-" + std::to_string(getpid());
    const std::string ffmpeg    = directory + "/ffmpeg";
#if defined(__APPLE__)
    const std::string sink_name = "ffplay";
    const std::string expected_arguments =
        "-nodisp\n-autoexit\n-hide_banner\n-loglevel\nwarning\n-f\ns16le\n-sample_rate\n48000\n-ch_layout\nstereo\n-i\npipe:0\n";
#else
    const std::string sink_name = "aplay";
    const std::string expected_arguments =
        "-q\n-D\ndefault\n-t\nraw\n-f\nS16_LE\n-c\n2\n-r\n48000\n-B\n50000\n-F\n10000\n-\n";
#endif
    const std::string audio_sink = directory + "/" + sink_name;
    const std::string arguments_path = directory + "/sink-arguments";
    std::filesystem::create_directories(directory);
    {
        std::ofstream script(ffmpeg, std::ios::trunc);
        script << "#!/bin/sh\nprintf 'data'\n/bin/sleep 5\n";
    }
    {
        std::ofstream script(audio_sink, std::ios::trunc);
        script << "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"" << arguments_path << "\"\n/bin/cat >/dev/null\n";
    }
    assert(chmod(ffmpeg.c_str(), 0755) == 0);
    assert(chmod(audio_sink.c_str(), 0755) == 0);

    const char *current_path       = std::getenv("PATH");
    const std::string saved_path   = current_path ? current_path : "";
    const bool path_was_configured = current_path != nullptr;
    assert(setenv("PATH", directory.c_str(), 1) == 0);
    {
        StreamPlayer player;
        assert(player.start("https://radio.example.com/live.mp3"));
        assert(player.state() == StreamPlaybackState::Starting);
        std::this_thread::sleep_for(std::chrono::milliseconds(850));
        player.poll();
        assert(player.state() == StreamPlaybackState::Playing);
        std::string arguments;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            std::ifstream arguments_file(arguments_path);
            arguments.assign(std::istreambuf_iterator<char>(arguments_file), std::istreambuf_iterator<char>());
            if (arguments == expected_arguments) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (std::chrono::steady_clock::now() < deadline);
        assert(arguments == expected_arguments);
        assert(player.toggle_pause());
        assert(player.state() == StreamPlaybackState::Paused);
        assert(player.toggle_pause());
        assert(player.state() == StreamPlaybackState::Playing);
        player.stop();
        assert(player.state() == StreamPlaybackState::Stopped);

        assert(player.start("https://radio.example.com/live.mp3"));
        assert(player.toggle_pause());
        player.stop();
        assert(player.state() == StreamPlaybackState::Stopped);
    }
    std::filesystem::remove(audio_sink);
    {
        StreamPlayer player;
        assert(!player.start("https://radio.example.com/live.mp3"));
        assert(player.state() == StreamPlaybackState::Error);
        assert(player.error() == sink_name + " not found");
    }

    if (path_was_configured) {
        assert(setenv("PATH", saved_path.c_str(), 1) == 0);
    } else {
        assert(unsetenv("PATH") == 0);
    }
    std::filesystem::remove_all(directory);
}

}  // namespace

int main()
{
    test_stations_and_selection();
    test_url_validation();
    test_directory_replacement_preserves_current_stream();
    test_custom_url_persistence();
    test_favorite_snapshots_and_identity();
    test_favorites_persistence_and_failed_write_rollback();
    test_invalid_favorites_are_bounded_and_not_partially_loaded();
    test_missing_player_dependencies();
    test_player_process_lifecycle();
    return 0;
}
