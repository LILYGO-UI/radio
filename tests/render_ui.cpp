#include "pages/radio/radio_view.hpp"
#include "services/artwork_cache.hpp"

#include <cm0/status_bar.h>
#include <cm0/typography.h>
#include <src/widgets/buttonmatrix/lv_buttonmatrix_private.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

class RenderPlayer final : public StreamPlayerInterface {
public:
    bool start(std::string_view url) override { last_url = url; state_ = StreamPlaybackState::Playing; return true; }
    void stop() noexcept override { state_ = StreamPlaybackState::Stopped; }
    bool toggle_pause() noexcept override {
        state_ = state_ == StreamPlaybackState::Paused ? StreamPlaybackState::Playing : StreamPlaybackState::Paused;
        return true;
    }
    void poll() noexcept override {}
    StreamPlaybackState state() const noexcept override { return state_; }
    const std::string &error() const noexcept override { return error_; }
    std::string last_url;
private:
    StreamPlaybackState state_ = StreamPlaybackState::Idle;
    std::string error_;
};

class RenderDirectory final : public RadioBrowserClientInterface {
public:
    bool search(const RadioBrowserQuery &query) override { query_ = query; pending_ = true; return true; }
    bool poll(RadioBrowserResult &result) override {
        if (!pending_) return false;
        pending_ = false;
        result = next;
        return true;
    }
    bool loading() const noexcept override { return pending_; }
    RadioBrowserQuery query_;
    RadioBrowserResult next{{
        {"Groove Salad", "https://example.com/groove", "United States", "MP3", 128, "groove", "ambient, chillout", {}, {}, {}},
        {"France Info", "https://example.com/france", "France", "MP3", 128, "france", "news, talk", {}, {}, {}},
        {"BBC Radio 6 Music", "https://example.com/bbc", "United Kingdom", "AAC", 320, "bbc", "alternative, music", {}, {}, {}},
        {"日本のラジオ · Radio Tokyo", "https://example.com/tokyo", "Japan", "AAC", 128, "tokyo", "music", {}, {}, {}},
        {"Jazz Radio", "https://example.com/jazz", "France", "MP3", 192, "jazz", "jazz, soul", {}, {}, {}},
        {"Radio Paradise", "https://example.com/paradise", "United States", "AAC", 192, "paradise", "eclectic", {}, {}, {}},
    }, {}, true};
private:
    bool pending_ = false;
};

class ArtworkFixture {
public:
    static constexpr const char *url = "https://radio.example.test/icon.png";

    explicit ArtworkFixture(bool enabled) {
        const char *cache = std::getenv("LILYGO_UI_RADIO_ARTWORK_CACHE");
        had_cache_ = cache != nullptr;
        original_cache_ = cache ? cache : "";
        if (!enabled) {
            assert(setenv("LILYGO_UI_RADIO_ARTWORK_CACHE", "", 1) == 0);
            return;
        }
        std::string pattern = (std::filesystem::temp_directory_path() / "radio-render-art-XXXXXX").string();
        assert(mkdtemp(pattern.data()));
        directory_ = pattern;
        const auto cache_directory = directory_ / "cache";
        assert(setenv("LILYGO_UI_RADIO_ARTWORK_CACHE", cache_directory.c_str(), 1) == 0);

        // Only the transport is substituted: the real PNG is decoded and cached
        // by the production worker, then loaded by a separate cache in the view.
        const char *preview_source = std::getenv("RADIO_RENDER_ARTWORK_IMAGE");
        const auto source = preview_source && *preview_source ? std::filesystem::path(preview_source) :
            std::filesystem::path(__FILE__).parent_path().parent_path() / "assets/app-icon-radio.png";
        assert(std::filesystem::exists(source));
        std::filesystem::copy_file(source, directory_ / "icon.png");
        std::ofstream curl(directory_ / "curl");
        curl << "#!/bin/sh\nexec /bin/cat \"$(dirname \"$0\")/icon.png\"\n";
        curl.close();
        assert(chmod((directory_ / "curl").c_str(), 0700) == 0);
        const char *path = std::getenv("PATH");
        original_path_ = path ? path : "";
        assert(setenv("PATH", (directory_.string() + ':' + original_path_).c_str(), 1) == 0);
        ArtworkCache cache_worker(cache_directory.string());
        assert(!cache_worker.request(url));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
        while (cache_worker.loading() && std::chrono::steady_clock::now() < deadline) {
            cache_worker.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        cache_worker.poll();
        const auto *image = cache_worker.request(url);
        assert(image && image->width == 256 && image->height == 256);
        pixels = image->pixels;
        assert(setenv("PATH", original_path_.c_str(), 1) == 0);
    }

    ~ArtworkFixture() {
        if (had_cache_) (void)setenv("LILYGO_UI_RADIO_ARTWORK_CACHE", original_cache_.c_str(), 1);
        else (void)unsetenv("LILYGO_UI_RADIO_ARTWORK_CACHE");
        if (!directory_.empty()) std::filesystem::remove_all(directory_);
    }

    std::vector<std::uint8_t> pixels;
private:
    std::filesystem::path directory_;
    std::string original_cache_;
    std::string original_path_;
    bool had_cache_ = false;
};

constexpr int kPartialRows = 80;
std::vector<lv_color32_t> framebuffer;
int display_width = 0;
lv_indev_t *pointer = nullptr;
lv_point_t pointer_position{};
lv_indev_state_t pointer_state = LV_INDEV_STATE_RELEASED;

void read_pointer(lv_indev_t *, lv_indev_data_t *data)
{
    data->point = pointer_position;
    data->state = pointer_state;
}

void flush_display(lv_display_t *display, const lv_area_t *area, std::uint8_t *pixels)
{
    const int width = area->x2 - area->x1 + 1;
    const auto *source = reinterpret_cast<const lv_color32_t *>(pixels);
    for (int y = area->y1; y <= area->y2; ++y) {
        const auto source_offset = static_cast<std::size_t>(y - area->y1) * static_cast<std::size_t>(width);
        const auto destination_offset =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(display_width) + static_cast<std::size_t>(area->x1);
        std::copy_n(source + source_offset, width, framebuffer.begin() + destination_offset);
    }
    lv_display_flush_ready(display);
}

bool write_ppm(const char *path, int width, int height)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (const auto &pixel : framebuffer) {
        file.put(static_cast<char>(pixel.red));
        file.put(static_cast<char>(pixel.green));
        file.put(static_cast<char>(pixel.blue));
    }
    return file.good();
}

bool colors_differ(const lv_color32_t &left, const lv_color32_t &right)
{
    return left.red != right.red || left.green != right.green || left.blue != right.blue || left.alpha != right.alpha;
}

lv_obj_t *named(lv_obj_t *root, const char *name)
{
    auto *object = lv_obj_find_by_name(root, name);
    if (!object) std::fprintf(stderr, "Missing object: %s\n", name);
    assert(object);
    return object;
}

lv_obj_t *create_preview_root(lv_obj_t *screen)
{
    // Match the AppKit runtime shell so the page gets the same usable area.
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xF2F2F7), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(screen, 36, 0);
    lv_obj_set_style_clip_corner(screen, true, 0);
    auto *status = cm0_status_bar_create(lv_layer_top());
    assert(status);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 0);

    auto *content = lv_obj_create(screen);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
    lv_obj_align(content, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_top(content, CM0_STATUS_BAR_HEIGHT, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_style_bg_color(content, lv_color_hex(0xF2F2F7), 0);
    lv_obj_set_style_bg_grad_dir(content, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    auto *indicator = cm0_home_indicator_create(lv_layer_top());
    assert(indicator);
    lv_obj_set_size(indicator, LV_PCT(100), 38);
    lv_obj_align(indicator, LV_ALIGN_BOTTOM_MID, 0, -3);
    lv_obj_remove_flag(indicator, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    return content;
}

void set_preview_status()
{
    auto *layer = lv_layer_top();
    lv_label_set_text(named(layer, "cm0_status_bar_clock"), "9:41");
    lv_label_set_text(named(layer, "cm0_status_bar_battery_text"), "82");
    lv_bar_set_value(named(layer, "cm0_status_bar_battery_level"), 82, LV_ANIM_OFF);
    for (const auto *name : {"cm0_status_bar_keyboard", "cm0_status_bar_ethernet", "cm0_status_bar_wifi_no",
                             "cm0_status_bar_wifi_zero", "cm0_status_bar_wifi_low", "cm0_status_bar_wifi_mid"}) {
        lv_obj_add_flag(named(layer, name), LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(named(layer, "cm0_status_bar_wifi_high"), LV_OBJ_FLAG_HIDDEN);
}

void settle(lv_obj_t *root, RadioViewModel &model)
{
    model.poll();
    lv_tick_inc(250);
    lv_timer_handler();
    lv_obj_update_layout(root);
}

void press_pointer(lv_obj_t *root, RadioViewModel &model)
{
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer);
    lv_tick_inc(30);
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    settle(root, model);
}

void click(lv_obj_t *root, RadioViewModel &model, const char *name)
{
    auto *object = named(root, name);
    lv_obj_scroll_to_view_recursive(object, LV_ANIM_OFF);
    lv_obj_update_layout(root);
    if (!lv_obj_is_visible(object)) std::fprintf(stderr, "Invisible click target: %s\n", name);
    assert(lv_obj_is_visible(object));
    assert(!lv_obj_has_state(object, LV_STATE_DISABLED));
    lv_area_t area{};
    lv_obj_get_coords(object, &area);
    pointer_position = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    auto *hit = lv_indev_search_obj(root, &pointer_position);
    if (hit != object) {
        std::fprintf(stderr, "Click target %s is blocked by %s at %d,%d\n", name,
                     hit && lv_obj_get_name(hit) ? lv_obj_get_name(hit) : "unnamed object",
                     static_cast<int>(pointer_position.x), static_cast<int>(pointer_position.y));
    }
    assert(hit == object);
    press_pointer(root, model);
}

lv_area_t bounds(lv_obj_t *object)
{
    lv_area_t area{};
    lv_obj_get_coords(object, &area);
    return area;
}

void click_search_key(lv_obj_t *root, RadioViewModel &model, const char *text)
{
    auto *keyboard = named(root, "radio_search_keyboard");
    const auto *matrix = reinterpret_cast<const lv_buttonmatrix_t *>(keyboard);
    const auto area = bounds(keyboard);
    for (std::uint32_t index = 0; index < matrix->btn_cnt; ++index) {
        if (std::string(lv_buttonmatrix_get_button_text(keyboard, index)) != text) continue;
        const auto button = matrix->button_areas[index];
        pointer_position = {area.x1 + (button.x1 + button.x2) / 2,
                            area.y1 + (button.y1 + button.y2) / 2};
        assert(lv_indev_search_obj(root, &pointer_position) == keyboard);
        press_pointer(root, model);
        return;
    }
    std::fprintf(stderr, "Missing keyboard key: %s\n", text);
    assert(false);
}

void assert_bounds(lv_obj_t *object, const lv_area_t &expected)
{
    const auto actual = bounds(object);
    assert(actual.x1 == expected.x1 && actual.x2 == expected.x2);
    assert(actual.y1 == expected.y1 && actual.y2 == expected.y2);
}

void assert_inside(lv_obj_t *object, int width, int height)
{
    if (!lv_obj_is_visible(object)) return;
    const auto area = bounds(object);
    if (area.x1 < 0 || area.x2 >= width || area.y1 < 0 || area.y2 >= height) {
        std::fprintf(stderr, "%s out of %dx%d bounds: (%d,%d)-(%d,%d)\n", lv_obj_get_name(object), width, height,
                     static_cast<int>(area.x1), static_cast<int>(area.y1), static_cast<int>(area.x2), static_cast<int>(area.y2));
    }
    assert(area.x1 >= 0 && area.x2 < width);
    assert(area.y1 >= 0 && area.y2 < height);
    assert(area.x2 >= area.x1 && area.y2 >= area.y1);
}

void assert_disjoint(lv_obj_t *left, lv_obj_t *right)
{
    if (!lv_obj_is_visible(left) || !lv_obj_is_visible(right)) return;
    const auto a = bounds(left);
    const auto b = bounds(right);
    if (!(a.x2 < b.x1 || b.x2 < a.x1 || a.y2 < b.y1 || b.y2 < a.y1)) {
        std::fprintf(stderr, "Overlapping objects: %s and %s\n", lv_obj_get_name(left), lv_obj_get_name(right));
    }
    assert(a.x2 < b.x1 || b.x2 < a.x1 || a.y2 < b.y1 || b.y2 < a.y1);
}

void check_layout(lv_obj_t *root, int width, int height)
{
    for (const auto *name : {"radio_player_panel", "radio_station_panel", "radio_settings_panel", "radio_navigation",
                             "radio_mini_player", "radio_artwork", "radio_now_title", "radio_play_pause",
                             "radio_stop", "radio_favorite", "radio_sleep", "radio_sleep_label", "radio_custom",
                             "radio_search", "radio_search_input", "radio_search_submit", "radio_settings_sleep",
                             "radio_settings_sleep_status", "radio_settings_custom"}) {
        assert_inside(named(root, name), width, height);
    }
    const char *tabs[] = {"radio_tab_playing", "radio_tab_stations", "radio_tab_favorites", "radio_tab_settings"};
    lv_obj_t *previous = nullptr;
    for (const auto *name : tabs) {
        auto *tab = named(root, name);
        assert_inside(tab, width, height);
        if (previous) assert_disjoint(previous, tab);
        previous = tab;
    }
    assert_disjoint(named(root, "radio_player_panel"), named(root, "radio_station_panel"));
    assert_disjoint(named(root, "radio_player_panel"), named(root, "radio_navigation"));
    assert_disjoint(named(root, "radio_settings_panel"), named(root, "radio_player_panel"));
    assert_disjoint(named(root, "radio_settings_panel"), named(root, "radio_station_panel"));
    assert_disjoint(named(root, "radio_settings_panel"), named(root, "radio_navigation"));
    assert_disjoint(named(root, "radio_settings_panel"), named(root, "radio_mini_player"));
    assert_disjoint(named(root, "radio_settings_sleep"), named(root, "radio_settings_custom"));
    assert_disjoint(named(root, "radio_station_panel"), named(root, "radio_mini_player"));
    assert_disjoint(named(root, "radio_mini_player"), named(root, "radio_navigation"));
    assert_disjoint(named(root, "radio_artwork"), named(root, "radio_now_title"));
    assert_disjoint(named(root, "radio_now_title"), named(root, "radio_play_pause"));
    assert_disjoint(named(root, "radio_play_pause"), named(root, "radio_stop"));
    assert_disjoint(named(root, "radio_play_pause"), named(root, "radio_favorite"));
    assert_disjoint(named(root, "radio_sleep"), named(root, "radio_custom"));
}

void exercise_favorites(lv_obj_t *root, RadioViewModel &model, RenderPlayer &player)
{
    click(root, model, "radio_favorite_0");
    assert(model.favorites().size() == 1);
    assert(model.playback_state() == StreamPlaybackState::Idle);
    click(root, model, "radio_favorite_0");
    assert(model.favorites().empty());
    click(root, model, "radio_station_0");
    assert(player.last_url == "https://example.com/groove");
    assert(model.playback_state() == StreamPlaybackState::Playing);
    assert(lv_obj_is_visible(named(root, "radio_player_panel")));
    click(root, model, "radio_favorite");
    assert(model.current_is_favorite());
    click(root, model, "radio_stop");
    assert(model.playback_state() == StreamPlaybackState::Stopped);
    click(root, model, "radio_tab_favorites");
    assert(lv_obj_is_visible(named(root, "radio_station_panel")));
    assert(model.favorites().size() == 1);
    click(root, model, "radio_station_0");
    assert(model.current_name() == "Groove Salad");
    click(root, model, "radio_tab_favorites");
    click(root, model, "radio_favorite_0");
    assert(model.favorites().empty());
    assert(model.playback_state() == StreamPlaybackState::Playing);
    assert(model.current_url() == "https://example.com/groove");
    assert(!model.current_is_favorite());
    assert(lv_obj_is_visible(named(root, "radio_directory_empty")));
    click(root, model, "radio_tab_playing");
    click(root, model, "radio_favorite");
    assert(model.current_is_favorite());
}

void exercise_sleep(lv_obj_t *root, RadioViewModel &model, RadioViewModel::Clock::time_point &now)
{
    click(root, model, "radio_sleep");
    auto *roller = named(root, "radio_sleep_minutes");
    assert(lv_roller_get_option_count(roller) == 5);
    assert(lv_roller_get_selected(roller) == 1);
    lv_roller_set_selected(roller, 4, LV_ANIM_OFF);
    click(root, model, "radio_sleep_start");
    assert(model.sleep_remaining_seconds() == 90 * 60);
    assert(!lv_obj_is_visible(named(root, "radio_sleep_overlay")));
    click(root, model, "radio_sleep");
    click(root, model, "radio_sleep_cancel");
    assert(!model.sleep_timer_active());
    if (lv_obj_is_visible(named(root, "radio_sleep_overlay"))) click(root, model, "radio_sleep_close");

    click(root, model, "radio_sleep");
    lv_roller_set_selected(roller, 0, LV_ANIM_OFF);
    click(root, model, "radio_sleep_start");
    now += std::chrono::minutes(15);
    settle(root, model);
    assert(!model.sleep_timer_active());
    assert(model.playback_state() == StreamPlaybackState::Stopped);
    click(root, model, "radio_play_pause");
    assert(model.playback_state() == StreamPlaybackState::Playing);

    click(root, model, "radio_sleep");
    lv_roller_set_selected(roller, 1, LV_ANIM_OFF);
    click(root, model, "radio_sleep_start");
    now += std::chrono::seconds(61);
    settle(root, model);
    assert(model.sleep_remaining_seconds() == 28 * 60 + 59);
    assert(std::string(lv_label_get_text(named(root, "radio_sleep_label"))).find("28:59") != std::string::npos);
    click(root, model, "radio_sleep");
    assert(std::string(lv_label_get_text(named(root, "radio_sleep_countdown"))).find("28:59") != std::string::npos);
    click(root, model, "radio_sleep_close");
    assert(model.sleep_timer_active());
}

void exercise_settings(lv_obj_t *root, RadioViewModel &model, RadioViewModel::Clock::time_point &now,
                       int width, int height)
{
    click(root, model, "radio_tab_settings");
    assert(lv_obj_is_visible(named(root, "radio_settings_panel")));
    assert(lv_obj_has_state(named(root, "radio_tab_settings"), LV_STATE_CHECKED));
    assert(!lv_obj_is_visible(named(root, "radio_player_panel")));
    assert(!lv_obj_is_visible(named(root, "radio_station_panel")));
    assert(!lv_obj_is_visible(named(root, "radio_search_keyboard")));
    assert(std::string(lv_label_get_text(named(root, "radio_settings_sleep_status"))) == "28:59");
    assert(model.playback_state() == StreamPlaybackState::Playing);
    check_layout(root, width, height);

    click(root, model, "radio_settings_sleep");
    assert(lv_obj_is_visible(named(root, "radio_sleep_overlay")));
    click(root, model, "radio_sleep_cancel");
    assert(!model.sleep_timer_active());
    assert(std::string(lv_label_get_text(named(root, "radio_settings_sleep_status"))) == "Off");
    if (lv_obj_is_visible(named(root, "radio_sleep_overlay"))) click(root, model, "radio_sleep_close");
    click(root, model, "radio_settings_sleep");
    lv_roller_set_selected(named(root, "radio_sleep_minutes"), 1, LV_ANIM_OFF);
    click(root, model, "radio_sleep_start");
    now += std::chrono::seconds(61);
    settle(root, model);
    assert(std::string(lv_label_get_text(named(root, "radio_settings_sleep_status"))) == "28:59");

    click(root, model, "radio_settings_custom");
    assert(lv_obj_is_visible(named(root, "radio_url_overlay")));
    click(root, model, "radio_url_cancel");
    assert(lv_obj_is_visible(named(root, "radio_settings_panel")));
    click(root, model, "radio_tab_stations");
    assert(!lv_obj_is_visible(named(root, "radio_settings_panel")));
    assert(lv_obj_is_visible(named(root, "radio_station_panel")));
}

void check_input_dialog(lv_obj_t *root, int width, int height)
{
    auto *dialog = named(root, "radio_url_dialog");
    auto *keyboard = named(root, "radio_url_keyboard");
    assert_inside(dialog, width, height);
    assert_inside(keyboard, width, height);
    assert_disjoint(dialog, keyboard);
    assert_inside(named(root, "radio_url_input"), width, height);
    assert_inside(named(root, "radio_url_submit"), width, height);
}

void check_keyboard_alignment(lv_obj_t *root)
{
    const auto surface = bounds(named(root, "app_surface"));
    const auto keyboard = bounds(named(root, "radio_search_keyboard"));
    const auto navigation = bounds(named(root, "radio_navigation"));
    assert(keyboard.x1 == surface.x1 && keyboard.x2 == surface.x2);
    assert(keyboard.y2 == surface.y2);
    assert(keyboard.x1 <= navigation.x1 && keyboard.x2 >= navigation.x2);
    assert(keyboard.y1 <= navigation.y1 && keyboard.y2 >= navigation.y2);
}

void check_inline_search(lv_obj_t *root, int width, int height)
{
    auto *search = named(root, "radio_search");
    auto *input = named(root, "radio_search_input");
    auto *keyboard = named(root, "radio_search_keyboard");
    assert(lv_obj_get_parent(input) == search);
    assert(lv_obj_is_visible(input));
    assert(lv_obj_is_visible(keyboard));
    assert_inside(search, width, height);
    assert_inside(input, width, height);
    assert_inside(named(root, "radio_search_submit"), width, height);
    assert_inside(keyboard, width, height);
    assert_disjoint(search, keyboard);
    check_keyboard_alignment(root);
    for (const auto *name : {"radio_tab_playing", "radio_tab_stations", "radio_tab_favorites", "radio_tab_settings"}) {
        const auto tab = bounds(named(root, name));
        lv_point_t position{(tab.x1 + tab.x2) / 2, (tab.y1 + tab.y2) / 2};
        assert(lv_indev_search_obj(root, &position) == keyboard);
    }
    const auto input_area = bounds(input);
    lv_point_t input_position{(input_area.x1 + input_area.x2) / 2, (input_area.y1 + input_area.y2) / 2};
    assert(lv_indev_search_obj(root, &input_position) == input);
}

}  // namespace

int main(int argc, char **argv)
{
    assert(argc >= 4 && argc <= 6);
    const int width = std::atoi(argv[1]);
    const int height = std::atoi(argv[2]);
    const std::string scenario = argc >= 5 ? argv[4] : "main";
    const bool status_bar = argc == 6 && std::string(argv[5]) == "--status-bar";
    assert(argc != 6 || status_bar);
    const bool artwork = scenario == "artwork";
    assert(width >= 320 && height >= 240);
    ArtworkFixture artwork_fixture(artwork);

    display_width = width;
    framebuffer.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    std::vector<lv_color32_t> draw_buffer(static_cast<std::size_t>(width) * kPartialRows);
    lv_init();
    auto *display = lv_display_create(width, height);
    assert(display);
    lv_display_set_buffers(display, draw_buffer.data(), nullptr,
                           static_cast<std::uint32_t>(draw_buffer.size() * sizeof(lv_color32_t)),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_display);
    lv_display_set_default(display);
    pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer, display);
    lv_indev_set_read_cb(pointer, read_pointer);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);

    auto *root = lv_screen_active();
    auto *content = status_bar ? create_preview_root(root) : root;
    RenderPlayer player;
    RenderDirectory directory;
    if (artwork) directory.next.stations[0].favicon = ArtworkFixture::url;
    auto now = RadioViewModel::Clock::time_point{};
    // Keep subjects alive until their bound objects have been destroyed.
    {
        RadioViewModel model(player, directory, "", [&now] { return now; });
        RadioView view(model);
        assert(model.load_radio_browser_stations());
        view.create(content, "Internet Radio");
        settle(root, model);

        assert(!lv_obj_find_by_name(root, "app_header"));
        assert(!lv_obj_find_by_name(root, "app_title"));
        assert(!lv_obj_find_by_name(root, "radio_tab_search"));
        assert(!lv_obj_find_by_name(root, "radio_search_overlay"));
        assert(!lv_obj_find_by_name(root, "radio_search_dialog"));
        assert(std::string(lv_label_get_text(named(root, "radio_now_title"))) == "Select a station");
        assert(std::string(lv_label_get_text(named(root, "radio_status"))) == "Ready");
        assert(lv_obj_is_visible(named(root, "radio_station_panel")));
        if (width < 800) {
            assert(!lv_obj_is_visible(named(root, "radio_player_panel")));
            assert(!lv_obj_is_visible(named(root, "radio_mini_player")));
        }
        check_layout(root, width, height);
        exercise_favorites(root, model, player);
        check_layout(root, width, height);
        exercise_sleep(root, model, now);
        exercise_settings(root, model, now, width, height);

        click(root, model, "radio_tab_stations");
        if (width < 800) {
            assert(!lv_obj_is_visible(named(root, "radio_player_panel")));
            assert(lv_obj_is_visible(named(root, "radio_mini_player")));
            assert(std::string(lv_label_get_text(named(root, "radio_mini_title"))) == "Groove Salad");
            click(root, model, "radio_mini_play_pause");
            assert(model.playback_state() == StreamPlaybackState::Paused);
            click(root, model, "radio_mini_play_pause");
            assert(model.playback_state() == StreamPlaybackState::Playing);
            assert(lv_obj_is_visible(named(root, "radio_station_panel")));
        }
        check_layout(root, width, height);

        auto *body = lv_obj_get_parent(named(root, "radio_station_panel"));
        auto *navigation = named(root, "radio_navigation");
        auto *mini_player = named(root, "radio_mini_player");
        const auto body_before_search = bounds(body);
        const auto navigation_before_search = bounds(navigation);
        const auto mini_before_search = bounds(mini_player);
        const bool mini_visible_before_search = lv_obj_is_visible(mini_player);
        const auto check_search_layout_unchanged = [&] {
            assert_bounds(body, body_before_search);
            assert_bounds(navigation, navigation_before_search);
            if (mini_visible_before_search) assert_bounds(mini_player, mini_before_search);
            assert(lv_obj_is_visible(mini_player) == mini_visible_before_search);
        };
        click(root, model, "radio_search_input");
        check_search_layout_unchanged();
        check_inline_search(root, width, height);
        auto *search_input = named(root, "radio_search_input");
        click_search_key(root, model, "j");
        assert(std::string(lv_textarea_get_text(search_input)) == "j");
        lv_textarea_set_text(search_input, "Jazz");
        click_search_key(root, model, LV_SYMBOL_KEYBOARD);
        assert(!lv_obj_is_visible(named(root, "radio_search_keyboard")));
        assert(!lv_obj_has_state(search_input, LV_STATE_FOCUSED));
        assert(std::string(lv_textarea_get_text(search_input)) == "Jazz");
        check_search_layout_unchanged();
        if (width >= 800) {
            click(root, model, "radio_sleep");
            assert(!lv_obj_is_visible(named(root, "radio_search_keyboard")));
            assert(!lv_obj_has_state(search_input, LV_STATE_FOCUSED));
            assert(lv_obj_is_visible(named(root, "radio_sleep_overlay")));
            click(root, model, "radio_sleep_close");
            assert(std::string(lv_textarea_get_text(search_input)) == "Jazz");
        }
        click(root, model, "radio_tab_settings");
        assert(!lv_obj_is_visible(named(root, "radio_search_keyboard")));
        click(root, model, "radio_tab_stations");
        assert(std::string(lv_textarea_get_text(search_input)) == "Jazz");
        click(root, model, "radio_search_input");
        if (scenario == "search") {
            auto *surface = named(root, "app_surface");
            const auto surface_width = lv_obj_get_width(surface);
            const auto surface_height = lv_obj_get_height(surface);
            lv_obj_set_size(surface, width >= 800 ? 700 : 820, surface_height);
            lv_obj_update_layout(root);
            assert(std::string(lv_textarea_get_text(search_input)) == "Jazz");
            assert(lv_obj_is_visible(named(root, "radio_search_keyboard")));
            assert(lv_obj_has_state(search_input, LV_STATE_FOCUSED));
            check_keyboard_alignment(root);
            lv_obj_set_size(surface, surface_width, surface_height);
            lv_obj_update_layout(root);
            check_keyboard_alignment(root);
            check_search_layout_unchanged();
        } else {
            if (scenario == "empty") directory.next = {};
            else if (scenario == "error") directory.next = {{}, "Unable to reach Radio Browser. Check your network and try again.", false};
            click(root, model, "radio_search_submit");
            assert(directory.query_.name == "Jazz");
            assert(!lv_obj_is_visible(named(root, "radio_search_keyboard")));
            assert(lv_obj_is_visible(search_input));
            assert(player.last_url == "https://example.com/groove");
            assert(model.playback_state() == StreamPlaybackState::Playing);
            click(root, model, "radio_tab_playing");
            click(root, model, "radio_play_pause");
            assert(model.playback_state() == StreamPlaybackState::Paused);
            click(root, model, "radio_play_pause");
            assert(model.playback_state() == StreamPlaybackState::Playing);

            if (scenario == "empty" || scenario == "error") {
                click(root, model, "radio_tab_stations");
                if (scenario == "empty") {
                    assert(model.stations().empty());
                    assert(lv_obj_is_visible(named(root, "radio_directory_empty")));
                } else assert(model.directory_state() == DirectoryState::Error);
            } else {
                click(root, model, "radio_tab_stations");
                click(root, model, "radio_load_more");
                assert(directory.query_.offset == 50);
                assert(model.stations().size() == 6);
                click(root, model, "radio_popular");
                assert(directory.query_.name.empty());
                if (scenario == "favorites") {
                    click(root, model, "radio_favorite_2");
                    click(root, model, "radio_favorite_4");
                    click(root, model, "radio_tab_favorites");
                    assert(model.favorites().size() == 3);
                } else if (scenario == "settings") {
                    click(root, model, "radio_tab_settings");
                } else if (scenario != "stations") {
                    click(root, model, "radio_tab_playing");
                    if (scenario == "dialog") click(root, model, "radio_custom");
                    if (scenario == "sleep") click(root, model, "radio_sleep");
                }
            }
        }

        settle(root, model);
        check_layout(root, width, height);
        if (scenario == "dialog") check_input_dialog(root, width, height);
        if (scenario == "search") check_inline_search(root, width, height);
        if (scenario == "sleep") {
            for (const auto *name : {"radio_sleep_minutes", "radio_sleep_countdown", "radio_sleep_start", "radio_sleep_cancel", "radio_sleep_close"}) {
                assert_inside(named(root, name), width, height);
            }
            assert_disjoint(named(root, "radio_sleep_minutes"), named(root, "radio_sleep_start"));
            assert_disjoint(named(root, "radio_sleep_start"), named(root, "radio_sleep_cancel"));
        }
        if (artwork) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!lv_obj_is_visible(named(root, "radio_artwork_image")) && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                settle(root, model);
            }
            auto *image = named(root, "radio_artwork_image");
            assert(lv_obj_is_visible(image));
            assert(!lv_obj_is_visible(named(root, "radio_artwork_fallback")));
            const auto *descriptor = static_cast<const lv_image_dsc_t *>(lv_image_get_src(image));
            assert(descriptor && descriptor->header.w == 256 && descriptor->header.h == 256);
            assert(descriptor->data_size == artwork_fixture.pixels.size());
            assert(std::equal(artwork_fixture.pixels.begin(), artwork_fixture.pixels.end(), descriptor->data));
        } else {
            assert(lv_obj_has_flag(named(root, "radio_artwork_image"), LV_OBJ_FLAG_HIDDEN));
            assert(!lv_obj_has_flag(named(root, "radio_artwork_fallback"), LV_OBJ_FLAG_HIDDEN));
        }

        if (status_bar) {
            set_preview_status();
            lv_obj_update_layout(root);
            auto *status = named(lv_layer_top(), "status_bar");
            assert_inside(status, width, height);
            assert_disjoint(status, named(root, "app_surface"));
        }
        lv_tick_inc(20);
        lv_refr_now(display);
        const bool has_contrast = std::any_of(framebuffer.cbegin() + 1, framebuffer.cend(),
                                              [](const auto &pixel) { return colors_differ(pixel, framebuffer[0]); });
        assert(has_contrast);
        if (artwork) {
            const auto area = bounds(named(root, "radio_artwork"));
            const auto first = framebuffer[static_cast<std::size_t>((area.y1 + area.y2) / 2 * width + area.x1 + 8)];
            unsigned different_pixels = 0;
            for (int y = area.y1 + 8; y <= area.y2 - 8; ++y) {
                for (int x = area.x1 + 8; x <= area.x2 - 8; ++x) {
                    if (colors_differ(first, framebuffer[static_cast<std::size_t>(y * width + x)])) ++different_pixels;
                }
            }
            assert(different_pixels > 1000);
        }
        assert(write_ppm(argv[3], width, height));
        view.destroy();
    }
    lv_indev_delete(pointer);
    pointer = nullptr;
    lv_display_delete(display);
    lilygo_ui_fonts_deinit();
    lv_deinit();
    return 0;
}
