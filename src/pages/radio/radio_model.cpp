#include "pages/radio/radio_model.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

#include <unistd.h>

namespace {

constexpr std::string_view kSettingPrefix = "custom_url=";
constexpr std::size_t kMaximumFavoritesFileSize = 4U * 1024U * 1024U;
constexpr std::string_view kFavoritesFormat = "LILYGO_RADIO_FAVORITES";

bool same_station(const RadioStation &left, const RadioStation &right) noexcept
{
    return (!left.uuid.empty() && left.uuid == right.uuid) || (!left.url.empty() && left.url == right.url);
}

bool valid_favorite(const RadioStation &station) noexcept
{
    return !station.name.empty() && station.name.size() <= 1024 && station.country.size() <= 256 &&
           station.codec.size() <= 64 && station.bitrate >= 0 && station.bitrate <= 100000 &&
           station.uuid.size() <= 128 && station.tags.size() <= 2048 && station.country_code.size() <= 8 &&
           station.language.size() <= 512 && station.favicon.size() <= 4096 &&
           RadioModel::validate_stream_url(station.url) == StreamUrlStatus::Valid;
}

std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.remove_suffix(1);
    return text;
}

bool starts_with_ignore_case(std::string_view text, std::string_view prefix) noexcept
{
    if (text.size() < prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(text[index])) !=
            std::tolower(static_cast<unsigned char>(prefix[index]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

RadioModel::RadioModel(std::string config_path) : config_path_(std::move(config_path))
{
    load_custom_url();
    load_favorites();
}

const std::vector<RadioStation> &RadioModel::stations() const noexcept
{
    return stations_;
}

int RadioModel::selected_index() const noexcept
{
    return selected_index_;
}

const std::string &RadioModel::current_name() const noexcept
{
    return current_name_;
}

const std::string &RadioModel::current_url() const noexcept
{
    return current_url_;
}

const std::string &RadioModel::current_metadata() const noexcept
{
    return current_metadata_;
}

const std::string &RadioModel::custom_url() const noexcept
{
    return custom_url_;
}

std::size_t RadioModel::available_station_count() const noexcept
{
    return stations_.size();
}

bool RadioModel::has_selection() const noexcept
{
    return has_selection_;
}

const RadioStation &RadioModel::current_station() const noexcept
{
    return current_station_;
}

bool RadioModel::is_current_station(const RadioStation &station) const noexcept
{
    return has_selection_ && same_station(current_station_, station);
}

const std::vector<RadioStation> &RadioModel::favorites() const noexcept
{
    return favorites_;
}

bool RadioModel::is_favorite(const RadioStation &station) const noexcept
{
    return std::any_of(favorites_.begin(), favorites_.end(), [&](const RadioStation &favorite) {
        return same_station(favorite, station);
    });
}

int RadioModel::favorites_revision() const noexcept
{
    return favorites_revision_;
}

const std::string &RadioModel::favorite_error() const noexcept
{
    return favorite_error_;
}

bool RadioModel::toggle_favorite(const RadioStation &station)
{
    favorite_error_.clear();
    std::vector<RadioStation> updated = favorites_;
    const auto found = std::find_if(updated.begin(), updated.end(), [&](const RadioStation &favorite) {
        return same_station(favorite, station);
    });
    if (found != updated.end()) {
        updated.erase(found);
    } else {
        if (!valid_favorite(station)) {
            favorite_error_ = "This station cannot be saved";
            return false;
        }
        if (updated.size() >= maximum_favorites) {
            favorite_error_ = "Favorites are full";
            return false;
        }
        updated.push_back(station);
    }
    if (!save_favorites(updated)) {
        favorite_error_ = "Could not save favorites. Check storage permissions and space.";
        return false;
    }
    favorites_.swap(updated);
    favorites_revision_ = favorites_revision_ == std::numeric_limits<int>::max() ? 0 : favorites_revision_ + 1;
    return true;
}

bool RadioModel::select_favorite(std::size_t index)
{
    if (index >= favorites_.size()) return false;
    select_snapshot(favorites_[index]);
    selected_index_ = -2;
    update_selected_index();
    return true;
}

void RadioModel::set_stations(std::vector<RadioStation> stations, bool append)
{
    if (!append) stations_.clear();
    for (auto &station : stations) {
        if (station.name.empty() || validate_stream_url(station.url) != StreamUrlStatus::Valid) continue;
        const auto duplicate = std::find_if(stations_.begin(), stations_.end(), [&](const RadioStation &existing) {
            return (!station.uuid.empty() && station.uuid == existing.uuid) || station.url == existing.url;
        });
        if (duplicate == stations_.end()) stations_.push_back(std::move(station));
    }
    update_selected_index();
}

void RadioModel::update_selected_index() noexcept
{
    if (selected_index_ == -1) return;
    selected_index_ = -2;
    if (!has_selection_) return;
    for (std::size_t index = 0; index < stations_.size(); ++index) {
        const auto &station = stations_[index];
        if (is_current_station(station)) {
            selected_index_ = static_cast<int>(index);
            break;
        }
    }
}

bool RadioModel::select_station(std::size_t index) noexcept
{
    if (index >= stations_.size()) return false;
    selected_index_ = static_cast<int>(index);
    select_snapshot(stations_[index]);
    return true;
}

void RadioModel::select_snapshot(const RadioStation &station)
{
    current_station_ = station;
    current_name_   = station.name;
    current_url_    = station.url;
    has_selection_  = true;
    current_metadata_ = station.country;
    if (!station.codec.empty()) {
        if (!current_metadata_.empty()) current_metadata_ += " · ";
        current_metadata_ += station.codec;
    }
    if (station.bitrate > 0) {
        if (!current_metadata_.empty()) current_metadata_ += " · ";
        current_metadata_ += std::to_string(station.bitrate) + " kbps";
    }
    if (current_metadata_.empty()) current_metadata_ = "Live radio";
}

StreamUrlStatus RadioModel::select_custom(std::string_view url)
{
    url                          = trim(url);
    const StreamUrlStatus status = validate_stream_url(url);
    if (status != StreamUrlStatus::Valid) return status;

    custom_url_.assign(url);
    current_name_   = "Custom audio stream";
    current_url_    = custom_url_;
    selected_index_ = -1;
    current_station_ = {};
    current_station_.name = current_name_;
    current_station_.url = custom_url_;
    has_selection_ = true;
    current_metadata_ = "Custom audio stream";
    (void)save_custom_url();
    return StreamUrlStatus::Valid;
}

StreamUrlStatus RadioModel::validate_stream_url(std::string_view url) noexcept
{
    url = trim(url);
    if (url.empty()) return StreamUrlStatus::Empty;
    if (url.size() > maximum_url_length) return StreamUrlStatus::TooLong;

    std::size_t authority_start = 0;
    if (starts_with_ignore_case(url, "http://")) {
        authority_start = 7;
    } else if (starts_with_ignore_case(url, "https://")) {
        authority_start = 8;
    } else {
        return StreamUrlStatus::UnsupportedScheme;
    }

    for (const unsigned char character : url) {
        if (character <= 0x20U || character == 0x7fU) return StreamUrlStatus::InvalidCharacter;
    }

    const std::size_t authority_end  = url.find_first_of("/?#", authority_start);
    const std::string_view authority = url.substr(authority_start, authority_end - authority_start);
    if (authority.empty()) return StreamUrlStatus::MissingHost;
    return StreamUrlStatus::Valid;
}

std::string RadioModel::default_config_path()
{
    if (const char *override_path = std::getenv("LILYGO_UI_RADIO_CONFIG")) {
        if (override_path[0] != '\0') return override_path;
    }

    if (const char *config_home = std::getenv("XDG_CONFIG_HOME")) {
        if (config_home[0] != '\0') return std::string(config_home) + "/lilygo/ui/radio.conf";
    }
    if (const char *home = std::getenv("HOME")) {
        if (home[0] != '\0') return std::string(home) + "/.config/lilygo/ui/radio.conf";
    }
    return {};
}

void RadioModel::load_custom_url()
{
    if (config_path_.empty()) return;

    std::ifstream file(config_path_);
    std::string line;
    while (std::getline(file, line)) {
        if (line.compare(0, kSettingPrefix.size(), kSettingPrefix) != 0) continue;
        const std::string_view value = trim(std::string_view(line).substr(kSettingPrefix.size()));
        if (validate_stream_url(value) == StreamUrlStatus::Valid) custom_url_.assign(value);
        return;
    }
}

bool RadioModel::save_custom_url() const
{
    if (config_path_.empty()) return false;

    const std::filesystem::path path(config_path_);
    std::error_code error;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;

    const std::string temporary = config_path_ + ".tmp";
    {
        std::ofstream file(temporary, std::ios::trunc);
        if (!file) return false;
        file << kSettingPrefix << custom_url_ << '\n';
        if (!file) return false;
    }
    std::filesystem::rename(temporary, config_path_, error);
    if (error) {
        std::filesystem::remove(temporary);
        return false;
    }
    return true;
}

void RadioModel::load_favorites()
{
    if (config_path_.empty()) return;
    const std::string path = config_path_ + ".favorites";
    std::error_code error;
    if (!std::filesystem::exists(path, error) && !error) return;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > kMaximumFavoritesFileSize) {
        favorite_error_ = "Could not read saved favorites";
        return;
    }
    std::ifstream file(path, std::ios::binary);
    std::string format;
    unsigned int version = 0;
    std::size_t count = 0;
    if (!(file >> format >> version >> count) || format != kFavoritesFormat || version != 1 ||
        count > maximum_favorites) {
        favorite_error_ = "Saved favorites have an unsupported or damaged format";
        return;
    }
    std::vector<RadioStation> loaded;
    for (std::size_t index = 0; index < count; ++index) {
        RadioStation station;
        if (!(file >> std::quoted(station.name) >> std::quoted(station.url) >> std::quoted(station.country) >>
              std::quoted(station.codec) >> station.bitrate >> std::quoted(station.uuid) >> std::quoted(station.tags) >>
              std::quoted(station.country_code) >> std::quoted(station.language) >> std::quoted(station.favicon)) ||
            !valid_favorite(station)) {
            favorite_error_ = "Saved favorites have an unsupported or damaged format";
            return;
        }
        const auto duplicate = std::find_if(loaded.begin(), loaded.end(), [&](const RadioStation &existing) {
            return same_station(existing, station);
        });
        if (duplicate == loaded.end()) loaded.push_back(std::move(station));
    }
    file >> std::ws;
    if (!file.eof()) {
        favorite_error_ = "Saved favorites have an unsupported or damaged format";
        return;
    }
    favorites_ = std::move(loaded);
}

bool RadioModel::save_favorites(const std::vector<RadioStation> &favorites) const
{
    if (config_path_.empty()) return true;
    std::ostringstream contents;
    contents << kFavoritesFormat << " 1 " << favorites.size() << '\n';
    for (const auto &station : favorites) {
        contents << std::quoted(station.name) << ' ' << std::quoted(station.url) << ' ' << std::quoted(station.country)
                 << ' ' << std::quoted(station.codec) << ' ' << station.bitrate << ' ' << std::quoted(station.uuid)
                 << ' ' << std::quoted(station.tags) << ' ' << std::quoted(station.country_code) << ' '
                 << std::quoted(station.language) << ' ' << std::quoted(station.favicon) << '\n';
    }
    const std::string serialized = contents.str();
    if (serialized.size() > kMaximumFavoritesFileSize) return false;
    const std::filesystem::path destination(config_path_ + ".favorites");
    std::error_code error;
    if (destination.has_parent_path()) std::filesystem::create_directories(destination.parent_path(), error);
    if (error) return false;

    // A private sibling file keeps failed or interrupted writes from damaging the saved collection.
    std::string temporary = destination.string() + ".tmp.XXXXXX";
    const int descriptor = mkstemp(temporary.data());
    if (descriptor < 0) return false;
    std::size_t written = 0;
    while (written < serialized.size()) {
        const ssize_t result = ::write(descriptor, serialized.data() + written, serialized.size() - written);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) break;
        written += static_cast<std::size_t>(result);
    }
    bool success = written == serialized.size();
    if (success && fsync(descriptor) != 0) success = false;
    if (::close(descriptor) != 0) success = false;
    if (success) {
        std::filesystem::rename(temporary, destination, error);
        success = !error;
    }
    if (!success) std::filesystem::remove(temporary, error);
    return success;
}
