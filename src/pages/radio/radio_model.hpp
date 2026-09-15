#ifndef LILYGO_UI_RADIO_PAGES_RADIO_RADIO_MODEL_HPP
#define LILYGO_UI_RADIO_PAGES_RADIO_RADIO_MODEL_HPP

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

struct RadioStation {
    std::string name;
    std::string url;
    std::string country;
    std::string codec;
    int bitrate = 0;
    std::string uuid;
    std::string tags;
    std::string country_code;
    std::string language;
    std::string favicon;
};

enum class StreamUrlStatus {
    Valid,
    Empty,
    TooLong,
    UnsupportedScheme,
    InvalidCharacter,
    MissingHost,
};

class RadioModel {
public:
    // Number of bundled stations, not a limit on the Radio Browser directory.
    static constexpr std::size_t station_count      = 4;
    static constexpr std::size_t maximum_url_length = 1023;
    static constexpr std::size_t maximum_favorites = 256;

    explicit RadioModel(std::string config_path = default_config_path());

    [[nodiscard]] const std::vector<RadioStation> &stations() const noexcept;
    [[nodiscard]] int selected_index() const noexcept;
    [[nodiscard]] const std::string &current_name() const noexcept;
    [[nodiscard]] const std::string &current_url() const noexcept;
    [[nodiscard]] const std::string &current_metadata() const noexcept;
    [[nodiscard]] const std::string &custom_url() const noexcept;
    [[nodiscard]] std::size_t available_station_count() const noexcept;
    [[nodiscard]] bool has_selection() const noexcept;
    [[nodiscard]] const RadioStation &current_station() const noexcept;
    [[nodiscard]] bool is_current_station(const RadioStation &station) const noexcept;
    [[nodiscard]] const std::vector<RadioStation> &favorites() const noexcept;
    [[nodiscard]] bool is_favorite(const RadioStation &station) const noexcept;
    [[nodiscard]] int favorites_revision() const noexcept;
    [[nodiscard]] const std::string &favorite_error() const noexcept;
    bool toggle_favorite(const RadioStation &station);
    bool select_favorite(std::size_t index);

    // Replacing search results must never change the currently playing stream.
    void set_stations(std::vector<RadioStation> stations, bool append = false);

    bool select_station(std::size_t index) noexcept;
    StreamUrlStatus select_custom(std::string_view url);

    [[nodiscard]] static StreamUrlStatus validate_stream_url(std::string_view url) noexcept;
    [[nodiscard]] static std::string default_config_path();

private:
    void load_custom_url();
    bool save_custom_url() const;
    void load_favorites();
    bool save_favorites(const std::vector<RadioStation> &favorites) const;
    void select_snapshot(const RadioStation &station);
    void update_selected_index() noexcept;

    std::vector<RadioStation> stations_{
        {"Groove Salad", "http://ice3.somafm.com/groovesalad-128-mp3", "United States", "MP3", 128, "", "ambient, chillout", "US", "english", ""},
        {"Drone Zone", "http://ice3.somafm.com/dronezone-128-mp3", "United States", "MP3", 128, "", "ambient", "US", "english", ""},
        {"Deep Space One", "http://ice3.somafm.com/deepspaceone-128-mp3", "United States", "MP3", 128, "", "ambient, space", "US", "english", ""},
        {"Secret Agent", "http://ice3.somafm.com/secretagent-128-mp3", "United States", "MP3", 128, "", "lounge, downtempo", "US", "english", ""},
    };
    std::string config_path_;
    std::vector<RadioStation> favorites_;
    RadioStation current_station_;
    std::string favorite_error_;
    int favorites_revision_ = 0;
    std::string custom_url_;
    std::string current_name_ = "Select a station";
    std::string current_url_  = "Play from the list or add a custom audio stream URL";
    std::string current_metadata_ = "Choose something to listen to";
    int selected_index_       = -2;
    bool has_selection_ = false;
};

#endif
