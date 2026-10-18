#include "pages/radio/radio_view.hpp"

#include "components/components.hpp"

#include <cm0/input.h>
#include <cm0/typography.h>
#include <src/misc/cache/instance/lv_image_cache.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

constexpr const char *kSearchIcon = "\xef\x80\x82";
constexpr const char *kStarIcon = "\xef\x80\x85";
constexpr const char *kClockIcon = "\xef\x80\x97";
constexpr std::array<int, 5> kSleepMinutes{{15, 30, 45, 60, 90}};

std::string countdown(int seconds)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%02d:%02d", seconds / 60, seconds % 60);
    return text;
}

void make_plain(lv_obj_t *object)
{
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *create_label(lv_obj_t *parent, const char *text, int size, std::uint32_t color)
{
    auto *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    if (const lv_font_t *font = lilygo_ui_font_get(size)) lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

lv_obj_t *create_icon_button(lv_obj_t *parent, const char *name, int size, std::uint32_t background,
                             lv_event_cb_t callback, void *user_data)
{
    auto *button = lv_button_create(parent);
    lv_obj_set_name_static(button, name);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(background), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(components::color_selected_background), LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(button, lv_color_hex(components::color_selected_background), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    return button;
}

lv_obj_t *create_text_button(lv_obj_t *parent, const char *name, const char *text, std::uint32_t background,
                             std::uint32_t foreground, lv_event_cb_t callback, void *user_data)
{
    auto *button = lv_button_create(parent);
    lv_obj_set_name_static(button, name);
    lv_obj_set_height(button, 48);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(background), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(components::color_selected_background), LV_STATE_CHECKED);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_hor(button, 16, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    auto *label = create_label(button, text, 18, foreground);
    lv_obj_center(label);
    return button;
}

lv_obj_t *create_input_keyboard(lv_obj_t *parent, const char *name, lv_event_cb_t callback, void *user_data)
{
    auto *keyboard = lv_keyboard_create(parent);
    lv_obj_set_name_static(keyboard, name);
    lv_obj_set_size(keyboard, LV_PCT(100), 220);
    lv_obj_set_style_bg_color(keyboard, lv_color_hex(components::color_control_background), 0);
    lv_obj_set_style_border_width(keyboard, 0, 0);
    lv_obj_set_style_radius(keyboard, 0, 0);
    if (const auto *font = lilygo_ui_font_get(16)) lv_obj_set_style_text_font(keyboard, font, LV_PART_ITEMS);
    lv_obj_add_event_cb(keyboard, callback, LV_EVENT_READY, user_data);
    lv_obj_add_event_cb(keyboard, callback, LV_EVENT_CANCEL, user_data);
    return keyboard;
}

bool is_active(StreamPlaybackState state) noexcept
{
    return state == StreamPlaybackState::Starting || state == StreamPlaybackState::Playing ||
           state == StreamPlaybackState::Paused;
}

std::string station_metadata(const RadioStation &station)
{
    std::string text = station.country;
    if (!station.codec.empty()) {
        if (!text.empty()) text += " · ";
        text += station.codec;
    }
    if (station.bitrate > 0) {
        if (!text.empty()) text += " · ";
        text += std::to_string(station.bitrate) + " kbps";
    }
    return text.empty() ? "Live radio" : text;
}

void set_disabled(lv_obj_t *object, bool disabled)
{
    if (disabled)
        lv_obj_add_state(object, LV_STATE_DISABLED);
    else
        lv_obj_remove_state(object, LV_STATE_DISABLED);
}

}  // namespace

RadioView::RadioView(RadioViewModel &view_model) noexcept : view_model_(view_model)
{
}

RadioView::~RadioView() { destroy(); }

lv_obj_t *RadioView::create(lv_obj_t *parent, const char *)
{
    destroy();
    surface_ = components::create_page(parent);
    lv_obj_add_event_cb(surface_, size_changed_event, LV_EVENT_SIZE_CHANGED, this);

    body_ = lv_obj_create(surface_);
    make_plain(body_);
    lv_obj_set_width(body_, LV_PCT(100));
    lv_obj_set_height(body_, 0);
    lv_obj_set_flex_grow(body_, 1);
    lv_obj_set_style_pad_row(body_, 0, 0);
    lv_obj_set_style_pad_column(body_, 1, 0);
    player_panel_  = create_player_panel(body_);
    station_panel_ = create_station_panel(body_);
    settings_panel_ = create_settings_panel(body_);
    create_navigation();
    search_keyboard_ = create_input_keyboard(surface_, "radio_search_keyboard", search_keyboard_event, this);
    lv_obj_add_flag(search_keyboard_, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(search_keyboard_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(search_keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(search_keyboard_, LV_OPA_COVER, 0);
    create_input_dialog(url_dialog_);
    create_sleep_dialog();
    tooltip_ = create_label(surface_, "", 14, components::color_container_background);
    lv_obj_add_flag(tooltip_, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(tooltip_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(tooltip_, lv_color_hex(components::color_text), 0);
    lv_obj_set_style_bg_opa(tooltip_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(tooltip_, 8, 0);
    lv_obj_set_style_radius(tooltip_, 6, 0);

    poll_timer_ = lv_timer_create(poll_timer, 200, this);
    lv_obj_update_layout(surface_);
    apply_layout();
    refresh_directory();
    refresh_controls();
    select_tab(0);
    return surface_;
}

void RadioView::destroy() noexcept
{
    host_text_input_.focus(nullptr);
    if (poll_timer_) lv_timer_delete(poll_timer_);
    poll_timer_ = nullptr;
    if (surface_) lv_obj_delete(surface_);
    surface_ = body_ = player_panel_ = station_panel_ = station_list_ = station_count_ = directory_status_ =
        empty_label_ = search_input_ = search_keyboard_ = refresh_button_ = popular_button_ = load_more_button_ = load_more_label_ =
            now_title_ = now_url_ = status_label_ = status_dot_ = play_button_ = play_icon_ = stop_button_ = nullptr;
    url_dialog_                                                                                            = {};
    settings_panel_ = settings_sleep_status_ = nullptr;
    station_rows_.clear();
    station_actions_.clear();
    station_favorites_.clear();
    station_artworks_.clear();
    current_artwork_ = {};
    for (auto &entry : artwork_sources_) lv_image_cache_drop(&entry.second);
    artwork_sources_.clear();
    requested_artwork_.clear();
    artwork_cache_.clear();
    tooltips_.clear();
    favorite_button_ = favorite_icon_ = favorite_error_ = sleep_button_ = sleep_label_ = sleep_overlay_ =
        sleep_panel_ = sleep_minutes_ = sleep_countdown_ = sleep_cancel_ = navigation_ = mini_player_ =
        mini_title_ = mini_icon_ = station_heading_ = search_row_ = directory_row_ = tooltip_ = nullptr;
    artwork_top_space_ = artwork_bottom_space_ = nullptr;
    tabs_ = {};
    active_tab_ = 0;
    favorites_visible_ = false;
    favorites_revision_ = -1;
    stations_revision_ = -1;
}

void RadioView::size_changed_event(lv_event_t *event)
{
    static_cast<RadioView *>(lv_event_get_user_data(event))->apply_layout();
}

void RadioView::station_event(lv_event_t *event)
{
    auto *view   = static_cast<RadioView *>(lv_event_get_user_data(event));
    auto *target = lv_event_get_target_obj(event);
    for (std::size_t index = 0; index < view->station_rows_.size(); ++index) {
        if (target != view->station_rows_[index]) continue;
        if (index >= view->visible_stations().size()) return;
        if (view->view_model_.is_current_station(view->visible_stations()[index]) &&
            is_active(view->view_model_.playback_state()))
            view->view_model_.toggle_playback();
        else if (view->favorites_visible_)
            (void)view->view_model_.play_favorite(index);
        else
            (void)view->view_model_.play_station(index);
        view->select_tab(0);
        view->refresh_controls();
        return;
    }
}

void RadioView::play_pause_event(lv_event_t *event)
{
    auto *view = static_cast<RadioView *>(lv_event_get_user_data(event));
    view->view_model_.toggle_playback();
    view->refresh_controls();
}

void RadioView::stop_event(lv_event_t *event)
{
    auto *view = static_cast<RadioView *>(lv_event_get_user_data(event));
    view->view_model_.stop();
    view->refresh_controls();
}

void RadioView::custom_event(lv_event_t *event)
{
    static_cast<RadioView *>(lv_event_get_user_data(event))->show_url_dialog();
}

void RadioView::search_event(lv_event_t *event)
{
    static_cast<RadioView *>(lv_event_get_user_data(event))->show_search_keyboard();
}

void RadioView::refresh_event(lv_event_t *event)
{
    auto *view = static_cast<RadioView *>(lv_event_get_user_data(event));
    (void)view->view_model_.refresh_stations();
    view->refresh_directory();
}

void RadioView::popular_event(lv_event_t *event)
{
    auto *view = static_cast<RadioView *>(lv_event_get_user_data(event));
    view->hide_search_keyboard();
    (void)view->view_model_.load_radio_browser_stations();
    lv_textarea_set_text(view->search_input_, "");
    lv_obj_scroll_to_y(view->station_list_, 0, LV_ANIM_OFF);
    view->refresh_directory();
}

void RadioView::load_more_event(lv_event_t *event)
{
    auto *view = static_cast<RadioView *>(lv_event_get_user_data(event));
    (void)view->view_model_.load_more_stations();
    view->refresh_directory();
}

void RadioView::cancel_url_event(lv_event_t *event)
{
    auto *view = static_cast<RadioView *>(lv_event_get_user_data(event));
    view->hide_dialog(view->url_dialog_);
}

void RadioView::submit_url_event(lv_event_t *event)
{
    static_cast<RadioView *>(lv_event_get_user_data(event))->submit_url();
}

void RadioView::submit_search_event(lv_event_t *event)
{
    static_cast<RadioView *>(lv_event_get_user_data(event))->submit_search();
}

void RadioView::keyboard_event(lv_event_t *event)
{
    auto *view = static_cast<RadioView *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_READY)
        view->submit_url();
    else if (lv_event_get_code(event) == LV_EVENT_CANCEL)
        view->hide_dialog(view->url_dialog_);
}

void RadioView::search_keyboard_event(lv_event_t *event)
{
    auto *view = static_cast<RadioView *>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_READY)
        view->submit_search();
    else if (lv_event_get_code(event) == LV_EVENT_CANCEL)
        view->hide_search_keyboard();
}

void RadioView::poll_timer(lv_timer_t *timer)
{
    auto *view = static_cast<RadioView *>(lv_timer_get_user_data(timer));
    view->view_model_.poll();
    (void)view->artwork_cache_.poll();
    view->refresh_directory();
    view->refresh_controls();
    view->refresh_artworks();
}

RadioView::ArtworkView RadioView::create_artwork(lv_obj_t *parent, int size, const char *name)
{
    ArtworkView result;
    result.frame = lv_obj_create(parent);
    if (name) lv_obj_set_name_static(result.frame, name);
    make_plain(result.frame);
    lv_obj_set_size(result.frame, size, size);
    lv_obj_set_style_radius(result.frame, 8, 0);
    lv_obj_set_style_bg_color(result.frame, lv_color_hex(components::color_control_background), 0);
    lv_obj_set_style_bg_opa(result.frame, LV_OPA_COVER, 0);
    lv_obj_remove_flag(result.frame, LV_OBJ_FLAG_CLICKABLE);
    result.fallback = create_label(result.frame, LV_SYMBOL_AUDIO, size > 100 ? 48 : 22, components::color_action);
    lv_obj_center(result.fallback);
    result.image = lv_image_create(result.frame);
    lv_obj_remove_flag(result.image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(result.image, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(result.image, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_center(result.image);
    lv_obj_add_flag(result.image, LV_OBJ_FLAG_HIDDEN);
    return result;
}

void RadioView::refresh_artwork(ArtworkView &view, const std::string &url, bool priority)
{
    if (!view.frame) return;
    if (view.url != url) {
        view.url = url;
        view.loaded = nullptr;
        lv_image_set_src(view.image, nullptr);
        lv_obj_add_flag(view.image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(view.fallback, LV_OBJ_FLAG_HIDDEN);
    }
    if (url.empty() || view.loaded) return;
    requested_artwork_.insert(url);
    const Artwork *artwork = artwork_cache_.request(url, priority);
    if (!artwork) return;
    auto &source = artwork_sources_[url];
    source.header.magic = LV_IMAGE_HEADER_MAGIC;
    source.header.cf = LV_COLOR_FORMAT_ARGB8888;
    source.header.w = artwork->width;
    source.header.h = artwork->height;
    source.header.stride = artwork->width * 4;
    source.data_size = static_cast<uint32_t>(artwork->pixels.size());
    source.data = artwork->pixels.data();
    lv_image_set_src(view.image, &source);
    lv_obj_remove_flag(view.image, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(view.fallback, LV_OBJ_FLAG_HIDDEN);
    view.loaded = artwork;
}

void RadioView::refresh_artworks()
{
    const auto &current_url = view_model_.current_station().favicon;
    std::set<std::string> retained{current_url};
    const auto &stations = visible_stations();
    for (std::size_t i = 0; i < station_artworks_.size() && i < stations.size(); ++i) {
        if (lv_obj_is_visible(station_rows_[i])) retained.insert(stations[i].favicon);
        else refresh_artwork(station_artworks_[i], "");
    }
    // Unbind old descriptors before releasing their pixel buffers. Keep pending
    // URLs until a later poll can release their completed result as well.
    refresh_artwork(current_artwork_, current_url, true);
    for (auto it = requested_artwork_.begin(); it != requested_artwork_.end();) {
        if (retained.count(*it)) { ++it; continue; }
        auto source = artwork_sources_.find(*it);
        if (source != artwork_sources_.end()) {
            lv_image_cache_drop(&source->second);
            artwork_sources_.erase(source);
        }
        if (artwork_cache_.release(*it)) it = requested_artwork_.erase(it);
        else ++it;
    }
    for (std::size_t i = 0; i < station_artworks_.size() && i < stations.size(); ++i) {
        if (lv_obj_is_visible(station_rows_[i])) refresh_artwork(station_artworks_[i], stations[i].favicon);
    }
}

void RadioView::add_tooltip(lv_obj_t *button, const char *text)
{
    tooltips_[button] = text;
    lv_obj_add_event_cb(button, tooltip_event, LV_EVENT_HOVER_OVER, this);
    lv_obj_add_event_cb(button, tooltip_event, LV_EVENT_HOVER_LEAVE, this);
    lv_obj_add_event_cb(button, tooltip_event, LV_EVENT_PRESSED, this);
}

void RadioView::tooltip_event(lv_event_t *event)
{
    auto *view = static_cast<RadioView *>(lv_event_get_user_data(event));
    if (!view->tooltip_) return;
    if (lv_event_get_code(event) != LV_EVENT_HOVER_OVER) {
        lv_obj_add_flag(view->tooltip_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    auto *target = lv_event_get_target_obj(event);
    const auto found = view->tooltips_.find(target);
    if (found == view->tooltips_.end()) return;
    lv_label_set_text(view->tooltip_, found->second.c_str());
    lv_obj_remove_flag(view->tooltip_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(view->tooltip_);
    lv_area_t area{}, surface{};
    lv_obj_get_coords(target, &area);
    lv_obj_get_coords(view->surface_, &surface);
    const int max_x = std::max(8, lv_obj_get_width(view->surface_) - lv_obj_get_width(view->tooltip_) - 8);
    lv_obj_set_pos(view->tooltip_, std::clamp(area.x1 - surface.x1, 8, max_x),
                   std::max(8, area.y1 - surface.y1 - lv_obj_get_height(view->tooltip_) - 6));
    lv_obj_move_foreground(view->tooltip_);
}

const std::vector<RadioStation> &RadioView::visible_stations() const noexcept
{
    return favorites_visible_ ? view_model_.favorites() : view_model_.stations();
}

void RadioView::create_navigation()
{
    mini_player_ = lv_button_create(surface_);
    lv_obj_set_name_static(mini_player_, "radio_mini_player");
    make_plain(mini_player_);
    lv_obj_set_size(mini_player_, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(mini_player_, lv_color_hex(components::color_selected_background), 0);
    lv_obj_set_style_bg_opa(mini_player_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(mini_player_, 20, 0);
    lv_obj_set_style_pad_column(mini_player_, 16, 0);
    lv_obj_set_style_shadow_width(mini_player_, 0, 0);
    lv_obj_set_flex_flow(mini_player_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mini_player_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(mini_player_, mini_player_event, LV_EVENT_CLICKED, this);
    create_label(mini_player_, LV_SYMBOL_AUDIO, 22, components::color_action);
    mini_title_ = create_label(mini_player_, "", 18, components::color_container_title);
    lv_obj_set_name_static(mini_title_, "radio_mini_title");
    lv_obj_set_width(mini_title_, 0);
    lv_obj_set_flex_grow(mini_title_, 1);
    lv_label_set_long_mode(mini_title_, LV_LABEL_LONG_DOT);
    auto *play = create_icon_button(mini_player_, "radio_mini_play_pause", 44,
                                    components::color_selected_background, play_pause_event, this);
    lv_obj_remove_flag(play, LV_OBJ_FLAG_EVENT_BUBBLE);
    mini_icon_ = create_label(play, LV_SYMBOL_PLAY, 22, components::color_action);
    lv_obj_center(mini_icon_);
    add_tooltip(play, "Play / pause");

    navigation_ = lv_obj_create(surface_);
    lv_obj_set_name_static(navigation_, "radio_navigation");
    make_plain(navigation_);
    lv_obj_set_size(navigation_, LV_PCT(100), 72);
    lv_obj_set_style_bg_color(navigation_, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_bg_opa(navigation_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(navigation_, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(navigation_, 1, 0);
    lv_obj_set_style_border_color(navigation_, lv_color_hex(components::color_border), 0);
    lv_obj_set_style_pad_ver(navigation_, 6, 0);
    lv_obj_set_flex_flow(navigation_, LV_FLEX_FLOW_ROW);
    const std::array<const char *, 4> names{{"radio_tab_playing", "radio_tab_stations", "radio_tab_favorites", "radio_tab_settings"}};
    const std::array<const char *, 4> labels{{"Playing", "Stations", "Favorites", "Settings"}};
    const std::array<const char *, 4> icons{{LV_SYMBOL_AUDIO, LV_SYMBOL_LIST, kStarIcon, LV_SYMBOL_SETTINGS}};
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        auto *tab = lv_button_create(navigation_);
        tabs_[i] = tab;
        lv_obj_set_name_static(tab, names[i]);
        make_plain(tab);
        lv_obj_set_size(tab, 0, LV_PCT(100));
        lv_obj_set_flex_grow(tab, 1);
        lv_obj_set_style_shadow_width(tab, 0, 0);
        lv_obj_set_style_bg_opa(tab, LV_OPA_TRANSP, LV_STATE_CHECKED);
        lv_obj_set_style_bg_color(tab, lv_color_hex(components::color_selected_background), LV_STATE_PRESSED);
        lv_obj_set_style_pad_row(tab, 5, 0);
        lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        create_label(tab, icons[i], 22, components::color_muted_text);
        create_label(tab, labels[i], 14, components::color_muted_text);
        lv_obj_add_event_cb(tab, tab_event, LV_EVENT_CLICKED, this);
    }
}

void RadioView::tab_event(lv_event_t *event)
{
    auto *view = static_cast<RadioView *>(lv_event_get_user_data(event));
    for (std::size_t i = 0; i < view->tabs_.size(); ++i)
        if (view->tabs_[i] == lv_event_get_target_obj(event)) view->select_tab(static_cast<int>(i));
}

void RadioView::mini_player_event(lv_event_t *event)
{
    static_cast<RadioView *>(lv_event_get_user_data(event))->select_tab(0);
}

void RadioView::select_tab(int tab)
{
    hide_search_keyboard();
    const bool station_tab = tab == 1 || tab == 2;
    const bool entering_station_tab = station_tab && active_tab_ != tab;
    active_tab_ = tab;
    if (station_tab && favorites_visible_ != (tab == 2)) {
        favorites_visible_ = tab == 2;
        stations_revision_ = -1;
    }
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
        const auto color = lv_color_hex(static_cast<int>(i) == tab ? components::color_action : components::color_muted_text);
        for (int child = 0; child < 2; ++child) lv_obj_set_style_text_color(lv_obj_get_child(tabs_[i], child), color, 0);
        lv_obj_set_state(tabs_[i], LV_STATE_CHECKED, static_cast<int>(i) == tab);
    }
    apply_layout();
    refresh_directory();
    refresh_controls();
    refresh_artworks();
    // Each station page starts at its first row. Keep the list position while
    // refreshing in place, but reset it when returning from another page.
    if (entering_station_tab && station_list_) {
        lv_obj_update_layout(station_list_);
        lv_obj_scroll_to_y(station_list_, 0, LV_ANIM_OFF);
    }
}

void RadioView::favorite_event(lv_event_t *event)
{
    auto *view = static_cast<RadioView *>(lv_event_get_user_data(event));
    (void)view->view_model_.toggle_current_favorite();
    view->refresh_directory();
    view->refresh_controls();
}

void RadioView::row_favorite_event(lv_event_t *event)
{
    auto *view = static_cast<RadioView *>(lv_event_get_user_data(event));
    for (std::size_t i = 0; i < view->station_favorites_.size(); ++i) {
        if (view->station_favorites_[i] != lv_event_get_target_obj(event)) continue;
        const RadioStation station = view->visible_stations()[i];
        (void)view->view_model_.toggle_favorite(station);
        view->refresh_directory();
        view->refresh_controls();
        return;
    }
}

void RadioView::create_sleep_dialog()
{
    sleep_overlay_ = lv_obj_create(surface_);
    cm0_input_set_focus_scope(sleep_overlay_);
    lv_obj_set_name_static(sleep_overlay_, "radio_sleep_overlay");
    make_plain(sleep_overlay_);
    lv_obj_add_flag(sleep_overlay_, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(sleep_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(sleep_overlay_, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(sleep_overlay_, lv_color_hex(components::color_overlay), 0);
    lv_obj_set_style_bg_opa(sleep_overlay_, LV_OPA_40, 0);
    sleep_panel_ = components::create_card(sleep_overlay_);
    lv_obj_set_name_static(sleep_panel_, "radio_sleep_dialog");
    lv_obj_set_size(sleep_panel_, 360, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(sleep_panel_, 20, 0);
    lv_obj_set_style_pad_row(sleep_panel_, 12, 0);
    lv_obj_set_flex_flow(sleep_panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sleep_panel_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    auto *heading = lv_obj_create(sleep_panel_);
    make_plain(heading);
    lv_obj_set_size(heading, LV_PCT(100), 44);
    lv_obj_set_flex_flow(heading, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(heading, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    auto *title = create_label(heading, "Sleep Timer", 22, components::color_container_title);
    lv_obj_set_flex_grow(title, 1);
    auto *close = create_icon_button(heading, "radio_sleep_close", 44, components::color_control_background,
                                     sleep_close_event, this);
    lv_obj_center(create_label(close, LV_SYMBOL_CLOSE, 18, components::color_muted_text));
    add_tooltip(close, "Close");
    sleep_countdown_ = create_label(sleep_panel_, "Off", 36, components::color_action);
    lv_obj_set_name_static(sleep_countdown_, "radio_sleep_countdown");
    lv_obj_set_size(sleep_countdown_, LV_PCT(100), 46);
    lv_obj_set_style_text_align(sleep_countdown_, LV_TEXT_ALIGN_CENTER, 0);
    sleep_minutes_ = lv_roller_create(sleep_panel_);
    lv_obj_set_name_static(sleep_minutes_, "radio_sleep_minutes");
    lv_roller_set_options(sleep_minutes_, "15 minutes\n30 minutes\n45 minutes\n60 minutes\n90 minutes", LV_ROLLER_MODE_NORMAL);
    lv_roller_set_selected(sleep_minutes_, 1, LV_ANIM_OFF);
    lv_obj_set_width(sleep_minutes_, LV_PCT(100));
    if (const auto *font = lilygo_ui_font_get(18)) {
        lv_obj_set_style_text_font(sleep_minutes_, font, LV_PART_MAIN);
        lv_obj_set_style_text_font(sleep_minutes_, font, LV_PART_SELECTED);
    }
    lv_roller_set_visible_row_count(sleep_minutes_, 3);
    lv_obj_set_style_border_width(sleep_minutes_, 0, 0);
    lv_obj_set_style_bg_color(sleep_minutes_, lv_color_hex(components::color_control_background), 0);
    lv_obj_set_style_bg_color(sleep_minutes_, lv_color_hex(components::color_action), LV_PART_SELECTED);
    auto *start = create_text_button(sleep_panel_, "radio_sleep_start", "Start Timer", components::color_action,
                                     components::color_container_background, sleep_start_event, this);
    lv_obj_set_width(start, LV_PCT(100));
    sleep_cancel_ = create_text_button(sleep_panel_, "radio_sleep_cancel", "Cancel Timer", components::color_control_background,
                                        components::color_error, sleep_cancel_event, this);
    lv_obj_set_width(sleep_cancel_, LV_PCT(100));
}

void RadioView::sleep_event(lv_event_t *event)
{
    auto *view = static_cast<RadioView *>(lv_event_get_user_data(event));
    view->hide_search_keyboard();
    view->refresh_controls();
    lv_obj_remove_flag(view->sleep_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(view->sleep_overlay_);
}

void RadioView::sleep_close_event(lv_event_t *event)
{
    auto *view = static_cast<RadioView *>(lv_event_get_user_data(event));
    lv_obj_add_flag(view->sleep_overlay_, LV_OBJ_FLAG_HIDDEN);
}

void RadioView::sleep_start_event(lv_event_t *event)
{
    auto *view = static_cast<RadioView *>(lv_event_get_user_data(event));
    const auto selected = lv_roller_get_selected(view->sleep_minutes_);
    if (selected < kSleepMinutes.size()) (void)view->view_model_.set_sleep_timer(kSleepMinutes[selected]);
    lv_obj_add_flag(view->sleep_overlay_, LV_OBJ_FLAG_HIDDEN);
    view->refresh_controls();
}

void RadioView::sleep_cancel_event(lv_event_t *event)
{
    auto *view = static_cast<RadioView *>(lv_event_get_user_data(event));
    view->view_model_.cancel_sleep_timer();
    view->refresh_controls();
}

lv_obj_t *RadioView::create_player_panel(lv_obj_t *parent)
{
    auto *panel = components::create_card(parent);
    lv_obj_set_name_static(panel, "radio_player_panel");
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 20, 0);
    lv_obj_set_style_pad_row(panel, 10, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto *status_row = lv_obj_create(panel);
    make_plain(status_row);
    lv_obj_set_size(status_row, LV_PCT(100), 24);
    lv_obj_set_style_pad_column(status_row, 8, 0);
    lv_obj_set_flex_flow(status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    status_dot_ = lv_obj_create(status_row);
    lv_obj_set_name_static(status_dot_, "radio_status_dot");
    lv_obj_set_size(status_dot_, 7, 7);
    lv_obj_set_style_radius(status_dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(status_dot_, 0, 0);
    lv_obj_set_style_pad_all(status_dot_, 0, 0);
    lv_obj_clear_flag(status_dot_, LV_OBJ_FLAG_SCROLLABLE);
    status_label_ = create_label(status_row, "", 14, components::color_muted_text);
    lv_obj_set_name_static(status_label_, "radio_status");
    lv_obj_set_style_max_width(status_label_, LV_PCT(92), 0);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_DOT);
    lv_label_bind_text(status_label_, view_model_.status_subject(), nullptr);

    now_title_ = create_label(panel, "", 22, components::color_container_title);
    lv_obj_set_name_static(now_title_, "radio_now_title");
    lv_obj_set_width(now_title_, LV_PCT(100));
    lv_label_set_long_mode(now_title_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(now_title_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_bind_text(now_title_, view_model_.title_subject(), nullptr);
    now_url_ = create_label(panel, "", 14, components::color_muted_text);
    lv_obj_set_name_static(now_url_, "radio_now_url");
    lv_obj_set_size(now_url_, LV_PCT(100), 22);
    lv_label_set_long_mode(now_url_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(now_url_, LV_TEXT_ALIGN_CENTER, 0);

    artwork_top_space_ = lv_obj_create(panel);
    make_plain(artwork_top_space_);
    lv_obj_set_size(artwork_top_space_, 0, 0);
    lv_obj_set_flex_grow(artwork_top_space_, 1);
    current_artwork_ = create_artwork(panel, 256, "radio_artwork");
    lv_obj_set_name_static(current_artwork_.image, "radio_artwork_image");
    lv_obj_set_name_static(current_artwork_.fallback, "radio_artwork_fallback");
    artwork_bottom_space_ = lv_obj_create(panel);
    make_plain(artwork_bottom_space_);
    lv_obj_set_size(artwork_bottom_space_, 0, 0);
    lv_obj_set_flex_grow(artwork_bottom_space_, 1);

    auto *controls = lv_obj_create(panel);
    make_plain(controls);
    lv_obj_set_size(controls, LV_PCT(100), 64);
    lv_obj_set_style_pad_column(controls, 22, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    stop_button_ =
        create_icon_button(controls, "radio_stop", 48, components::color_control_background, stop_event, this);
    add_tooltip(stop_button_, "Stop playback");
    lv_obj_center(create_label(stop_button_, LV_SYMBOL_STOP, 18, components::color_text));
    play_button_ =
        create_icon_button(controls, "radio_play_pause", 64, components::color_action, play_pause_event, this);
    add_tooltip(play_button_, "Play / pause");
    play_icon_ = create_label(play_button_, LV_SYMBOL_PLAY, 22, components::color_container_background);
    lv_obj_set_name_static(play_icon_, "radio_play_icon");
    lv_obj_center(play_icon_);
    favorite_button_ = create_icon_button(controls, "radio_favorite", 48, components::color_control_background,
                                          favorite_event, this);
    favorite_icon_ = create_label(favorite_button_, kStarIcon, 22, components::color_muted_text);
    lv_obj_center(favorite_icon_);
    add_tooltip(favorite_button_, "Add to favorites");

    auto *tools = lv_obj_create(panel);
    make_plain(tools);
    lv_obj_set_size(tools, LV_PCT(100), 44);
    lv_obj_set_style_pad_column(tools, 12, 0);
    lv_obj_set_flex_flow(tools, LV_FLEX_FLOW_ROW);
    sleep_button_ = create_text_button(tools, "radio_sleep", "", components::color_control_background,
                                        components::color_action, sleep_event, this);
    lv_obj_set_height(sleep_button_, 44);
    lv_obj_set_width(sleep_button_, 0);
    lv_obj_set_flex_grow(sleep_button_, 1);
    sleep_label_ = lv_obj_get_child(sleep_button_, 0);
    lv_obj_set_name_static(sleep_label_, "radio_sleep_label");
    auto *custom = create_icon_button(tools, "radio_custom", 44, components::color_control_background,
                                      custom_event, this);
    lv_obj_center(create_label(custom, LV_SYMBOL_PLUS, 18, components::color_action));
    add_tooltip(custom, "Add a stream");
    favorite_error_ = create_label(panel, "", 14, components::color_error);
    lv_obj_set_name_static(favorite_error_, "radio_favorite_error");
    lv_obj_set_width(favorite_error_, LV_PCT(100));
    lv_label_set_long_mode(favorite_error_, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(favorite_error_, LV_OBJ_FLAG_HIDDEN);
    return panel;
}

lv_obj_t *RadioView::create_station_panel(lv_obj_t *parent)
{
    auto *panel = components::create_card(parent);
    lv_obj_set_name_static(panel, "radio_station_panel");
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 16, 0);
    lv_obj_set_style_pad_row(panel, 10, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    auto *heading = lv_obj_create(panel);
    make_plain(heading);
    lv_obj_set_size(heading, LV_PCT(100), 44);
    lv_obj_set_flex_flow(heading, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(heading, 10, 0);
    lv_obj_set_flex_align(heading, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    auto *title = create_label(heading, "Stations", 22, components::color_container_title);
    station_heading_ = title;
    lv_obj_set_flex_grow(title, 1);
    station_count_ = create_label(heading, "", 14, components::color_muted_text);
    lv_obj_set_name_static(station_count_, "radio_station_count");
    refresh_button_ =
        create_icon_button(heading, "radio_refresh", 44, components::color_control_background, refresh_event, this);
    add_tooltip(refresh_button_, "Refresh stations");
    lv_obj_center(create_label(refresh_button_, LV_SYMBOL_REFRESH, 18, components::color_action));

    auto *search = lv_obj_create(panel);
    search_row_ = search;
    lv_obj_set_name_static(search, "radio_search");
    make_plain(search);
    lv_obj_set_size(search, LV_PCT(100), 48);
    lv_obj_set_style_radius(search, 8, 0);
    lv_obj_set_style_bg_color(search, lv_color_hex(components::color_control_background), 0);
    lv_obj_set_style_bg_opa(search, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(search, 4, 0);
    lv_obj_set_flex_flow(search, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(search, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    search_input_ = lv_textarea_create(search);
    lv_obj_set_name_static(search_input_, "radio_search_input");
    lv_textarea_set_one_line(search_input_, true);
    lv_textarea_set_max_length(search_input_, 120);
    lv_textarea_set_placeholder_text(search_input_, "Search stations");
    lv_textarea_set_text(search_input_, view_model_.search_query().c_str());
    lv_obj_set_size(search_input_, 0, LV_PCT(100));
    lv_obj_set_flex_grow(search_input_, 1);
    lv_obj_set_style_bg_opa(search_input_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(search_input_, 0, 0);
    lv_obj_set_style_radius(search_input_, 0, 0);
    lv_obj_set_style_pad_all(search_input_, 10, 0);
    lv_obj_set_style_text_color(search_input_, lv_color_hex(components::color_text), 0);
    lv_obj_set_style_text_color(search_input_, lv_color_hex(components::color_muted_text), LV_PART_TEXTAREA_PLACEHOLDER);
    if (const auto *font = lilygo_ui_font_get(18)) lv_obj_set_style_text_font(search_input_, font, 0);
    lv_obj_add_event_cb(search_input_, search_event, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(search_input_, search_keyboard_event, LV_EVENT_READY, this);
    lv_obj_add_event_cb(search_input_, search_keyboard_event, LV_EVENT_CANCEL, this);
    auto *submit = create_icon_button(search, "radio_search_submit", 40, components::color_control_background,
                                      submit_search_event, this);
    lv_obj_center(create_label(submit, kSearchIcon, 18, components::color_action));
    add_tooltip(submit, "Search stations");

    auto *directory_row = lv_obj_create(panel);
    directory_row_ = directory_row;
    make_plain(directory_row);
    lv_obj_set_size(directory_row, LV_PCT(100), 44);
    lv_obj_set_style_pad_column(directory_row, 10, 0);
    lv_obj_set_flex_flow(directory_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(directory_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    popular_button_ =
        create_text_button(directory_row, "radio_popular", "Popular", components::color_selected_background,
                           components::color_action, popular_event, this);
    lv_obj_set_size(popular_button_, 104, 44);
    directory_status_ = create_label(directory_row, "", 14, components::color_muted_text);
    lv_obj_set_name_static(directory_status_, "radio_directory_status");
    lv_obj_set_width(directory_status_, 0);
    lv_obj_set_flex_grow(directory_status_, 1);
    lv_label_set_long_mode(directory_status_, LV_LABEL_LONG_DOT);

    station_list_ = lv_obj_create(panel);
    lv_obj_set_name_static(station_list_, "radio_station_list");
    make_plain(station_list_);
    lv_obj_add_flag(station_list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(station_list_, LV_PCT(100), 0);
    lv_obj_set_flex_grow(station_list_, 1);
    lv_obj_set_style_pad_row(station_list_, 4, 0);
    lv_obj_set_flex_flow(station_list_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(station_list_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(station_list_, LV_SCROLLBAR_MODE_AUTO);

    auto *credit = create_label(panel, "Stations by Radio Browser", 14, components::color_muted_text);
    lv_obj_set_name_static(credit, "radio_directory_credit");
    lv_obj_set_width(credit, LV_PCT(100));
    lv_obj_set_style_text_align(credit, LV_TEXT_ALIGN_CENTER, 0);
    return panel;
}

lv_obj_t *RadioView::create_settings_panel(lv_obj_t *parent)
{
    auto *panel = components::create_card(parent);
    lv_obj_set_name_static(panel, "radio_settings_panel");
    lv_obj_set_size(panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_radius(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 20, 0);
    lv_obj_set_style_pad_row(panel, 12, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    auto *title = create_label(panel, "Settings", 22, components::color_container_title);
    lv_obj_set_size(title, LV_PCT(100), 44);

    const std::array<const char *, 2> names{{"radio_settings_sleep", "radio_settings_custom"}};
    const std::array<const char *, 2> labels{{"Sleep Timer", "Add a stream"}};
    const std::array<const char *, 2> icons{{kClockIcon, LV_SYMBOL_PLUS}};
    const std::array<lv_event_cb_t, 2> callbacks{{sleep_event, custom_event}};
    for (std::size_t i = 0; i < names.size(); ++i) {
        auto *row = lv_button_create(panel);
        lv_obj_set_name_static(row, names[i]);
        make_plain(row);
        lv_obj_set_size(row, LV_PCT(100), 64);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(components::color_border), 0);
        lv_obj_set_style_pad_column(row, 12, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(row, callbacks[i], LV_EVENT_CLICKED, this);
        auto *icon = create_label(row, icons[i], 22, components::color_action);
        lv_obj_set_width(icon, 24);
        auto *label = create_label(row, labels[i], 18, components::color_text);
        lv_obj_set_width(label, 0);
        lv_obj_set_flex_grow(label, 1);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        if (i == 0) {
            settings_sleep_status_ = create_label(row, "Off", 14, components::color_muted_text);
            lv_obj_set_name_static(settings_sleep_status_, "radio_settings_sleep_status");
            lv_obj_set_width(settings_sleep_status_, 56);
            lv_obj_set_style_text_align(settings_sleep_status_, LV_TEXT_ALIGN_RIGHT, 0);
        }
        create_label(row, LV_SYMBOL_RIGHT, 14, components::color_muted_text);
    }
    return panel;
}

lv_obj_t *RadioView::create_station_row(lv_obj_t *parent, const RadioStation &station, std::size_t index)
{
    auto *row = lv_button_create(parent);
    lv_obj_set_name(row, ("radio_station_" + std::to_string(index)).c_str());
    lv_obj_set_size(row, LV_PCT(100), 82);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(components::color_container_background), 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(components::color_control_background), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 10, 0);
    lv_obj_set_style_pad_column(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(row, station_event, LV_EVENT_CLICKED, this);

    station_artworks_.push_back(create_artwork(row, 54, nullptr));

    auto *text = lv_obj_create(row);
    make_plain(text);
    lv_obj_remove_flag(text, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(text, 0, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_style_pad_row(text, 5, 0);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    auto *name = create_label(text, station.name.c_str(), 18, components::color_action);
    lv_obj_set_width(name, LV_PCT(100));
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    auto *metadata = create_label(text, station_metadata(station).c_str(), 14, components::color_muted_text);
    lv_obj_set_width(metadata, LV_PCT(100));
    lv_label_set_long_mode(metadata, LV_LABEL_LONG_DOT);
    auto *favorite = create_icon_button(row, "radio_row_favorite", 38, components::color_container_background,
                                       row_favorite_event, this);
    lv_obj_set_name(favorite, ("radio_favorite_" + std::to_string(index)).c_str());
    lv_obj_remove_flag(favorite, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_center(create_label(favorite, kStarIcon, 18, components::color_muted_text));
    station_favorites_.push_back(favorite);
    add_tooltip(favorite, "Add to favorites");
    auto *action = create_label(row, LV_SYMBOL_PLAY, 14, components::color_action);
    lv_obj_set_width(action, 16);
    station_actions_.push_back(action);
    return row;
}

void RadioView::create_input_dialog(InputDialog &dialog)
{
    dialog.overlay = lv_obj_create(surface_);
    cm0_input_set_focus_scope(dialog.overlay);
    lv_obj_set_name_static(dialog.overlay, "radio_url_overlay");
    lv_obj_add_flag(dialog.overlay, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(dialog.overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(dialog.overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_align(dialog.overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(dialog.overlay, 0, 0);
    lv_obj_set_style_border_width(dialog.overlay, 0, 0);
    lv_obj_set_style_bg_color(dialog.overlay, lv_color_hex(components::color_overlay), 0);
    lv_obj_set_style_bg_opa(dialog.overlay, LV_OPA_40, 0);
    lv_obj_set_style_pad_all(dialog.overlay, 0, 0);
    lv_obj_clear_flag(dialog.overlay, LV_OBJ_FLAG_SCROLLABLE);

    dialog.panel = components::create_card(dialog.overlay);
    lv_obj_set_name_static(dialog.panel, "radio_url_dialog");
    lv_obj_add_flag(dialog.panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(dialog.panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(dialog.panel, LV_DIR_VER);
    lv_obj_set_size(dialog.panel, 520, LV_SIZE_CONTENT);
    lv_obj_align(dialog.panel, LV_ALIGN_TOP_MID, 0, 16);
    lv_obj_set_style_pad_all(dialog.panel, 20, 0);
    lv_obj_set_style_pad_row(dialog.panel, 12, 0);
    lv_obj_set_flex_flow(dialog.panel, LV_FLEX_FLOW_COLUMN);
    create_label(dialog.panel, "Add a stream", 22,
                 components::color_container_title);

    dialog.input = lv_textarea_create(dialog.panel);
    lv_obj_set_name_static(dialog.input, "radio_url_input");
    lv_obj_set_size(dialog.input, LV_PCT(100), 50);
    lv_textarea_set_one_line(dialog.input, true);
    lv_textarea_set_max_length(dialog.input, RadioModel::maximum_url_length);
    lv_textarea_set_placeholder_text(dialog.input, "https://example.com/live.mp3");
    lv_obj_add_event_cb(dialog.input, keyboard_event, LV_EVENT_READY, this);
    lv_obj_add_event_cb(dialog.input, keyboard_event, LV_EVENT_CANCEL, this);
    if (const lv_font_t *font = lilygo_ui_font_get(18)) lv_obj_set_style_text_font(dialog.input, font, 0);
    lv_obj_set_style_text_color(dialog.input, lv_color_hex(components::color_text), 0);
    lv_obj_set_style_border_color(dialog.input, lv_color_hex(components::color_action), 0);
    lv_obj_set_style_border_width(dialog.input, 1, 0);
    lv_obj_set_style_radius(dialog.input, 12, 0);

    dialog.error = create_label(dialog.panel, "", 14, components::color_error);
    lv_obj_set_name_static(dialog.error, "radio_url_error");
    lv_obj_set_size(dialog.error, LV_PCT(100), 22);
    lv_label_set_long_mode(dialog.error, LV_LABEL_LONG_DOT);
    auto *actions = lv_obj_create(dialog.panel);
    make_plain(actions);
    lv_obj_set_size(actions, LV_PCT(100), 48);
    lv_obj_set_style_pad_column(actions, 12, 0);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    auto *cancel = create_text_button(actions, "radio_url_cancel", "Cancel",
                                      components::color_control_background, components::color_text,
                                      cancel_url_event, this);
    lv_obj_set_flex_grow(cancel, 1);
    auto *submit =
        create_text_button(actions, "radio_url_submit", "Play",
                           components::color_action, components::color_container_background,
                           submit_url_event, this);
    lv_obj_set_flex_grow(submit, 1);

    dialog.keyboard = create_input_keyboard(dialog.overlay, "radio_url_keyboard", keyboard_event, this);
    lv_obj_add_flag(dialog.keyboard, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(dialog.keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
}

void RadioView::show_url_dialog()
{
    hide_search_keyboard();
    lv_textarea_set_text(url_dialog_.input, view_model_.custom_url().c_str());
    lv_textarea_set_cursor_pos(url_dialog_.input, LV_TEXTAREA_CURSOR_LAST);
    lv_label_set_text(url_dialog_.error, "");
    lv_keyboard_set_textarea(url_dialog_.keyboard, url_dialog_.input);
    lv_obj_remove_flag(url_dialog_.overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(url_dialog_.overlay);
    lv_obj_add_state(url_dialog_.input, LV_STATE_FOCUSED);
    host_text_input_.focus(url_dialog_.input);
}

void RadioView::show_search_keyboard()
{
    if (!lv_obj_has_flag(search_keyboard_, LV_OBJ_FLAG_HIDDEN)) return;
    lv_keyboard_set_textarea(search_keyboard_, search_input_);
    lv_obj_remove_flag(search_keyboard_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(search_keyboard_);
    lv_obj_add_state(search_input_, LV_STATE_FOCUSED);
    host_text_input_.focus(search_input_);
    apply_layout();
}

void RadioView::hide_search_keyboard() noexcept
{
    if (!search_keyboard_ || lv_obj_has_flag(search_keyboard_, LV_OBJ_FLAG_HIDDEN)) return;
    host_text_input_.focus(nullptr);
    lv_keyboard_set_textarea(search_keyboard_, nullptr);
    lv_obj_remove_state(search_input_, LV_STATE_FOCUSED);
    lv_obj_add_flag(search_keyboard_, LV_OBJ_FLAG_HIDDEN);
    apply_layout();
}

void RadioView::hide_dialog(InputDialog &dialog) noexcept
{
    if (!dialog.overlay) return;
    host_text_input_.focus(nullptr);
    lv_keyboard_set_textarea(dialog.keyboard, nullptr);
    lv_obj_remove_state(dialog.input, LV_STATE_FOCUSED);
    lv_obj_add_flag(dialog.overlay, LV_OBJ_FLAG_HIDDEN);
}

void RadioView::submit_url()
{
    if (!view_model_.play_custom(lv_textarea_get_text(url_dialog_.input))) {
        const auto &error = view_model_.input_error();
        lv_label_set_text(url_dialog_.error, error.empty() ? view_model_.status_text().c_str() : error.c_str());
        return;
    }
    hide_dialog(url_dialog_);
    select_tab(0);
    refresh_controls();
}

void RadioView::submit_search()
{
    (void)view_model_.search_stations(lv_textarea_get_text(search_input_));
    lv_textarea_set_text(search_input_, view_model_.search_query().c_str());
    hide_search_keyboard();
    select_tab(1);
    lv_obj_scroll_to_y(station_list_, 0, LV_ANIM_OFF);
    refresh_directory();
}

void RadioView::rebuild_stations()
{
    const int scroll_y = lv_obj_get_scroll_y(station_list_);
    for (auto *button : station_favorites_) tooltips_.erase(button);
    lv_obj_clean(station_list_);
    station_rows_.clear();
    station_actions_.clear();
    station_favorites_.clear();
    station_artworks_.clear();
    const auto &stations = visible_stations();
    station_rows_.reserve(stations.size());
    station_actions_.reserve(stations.size());
    for (std::size_t index = 0; index < stations.size(); ++index)
        station_rows_.push_back(create_station_row(station_list_, stations[index], index));
    empty_label_ = create_label(station_list_, "", 18, components::color_muted_text);
    lv_obj_set_name_static(empty_label_, "radio_directory_empty");
    lv_obj_set_width(empty_label_, LV_PCT(100));
    lv_obj_set_style_pad_all(empty_label_, 24, 0);
    lv_obj_set_style_text_align(empty_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(empty_label_, LV_LABEL_LONG_WRAP);
    load_more_button_ =
        create_text_button(station_list_, "radio_load_more", "Load more stations", components::color_control_background,
                           components::color_action, load_more_event, this);
    lv_obj_set_width(load_more_button_, LV_PCT(100));
    load_more_label_ = lv_obj_get_child(load_more_button_, 0);
    lv_obj_update_layout(station_list_);
    lv_obj_scroll_to_y(station_list_, scroll_y, LV_ANIM_OFF);
}

void RadioView::refresh_directory()
{
    if (!station_list_) return;
    if (stations_revision_ != view_model_.stations_revision() ||
        favorites_revision_ != view_model_.favorites_revision()) {
        stations_revision_ = view_model_.stations_revision();
        favorites_revision_ = view_model_.favorites_revision();
        rebuild_stations();
    }
    const bool loading = !favorites_visible_ && view_model_.directory_state() == DirectoryState::Loading;
    const bool empty   = visible_stations().empty();
    const bool failed  = !favorites_visible_ && view_model_.directory_state() == DirectoryState::Error;
    lv_label_set_text(station_heading_, favorites_visible_ ? "Favorites" : "Stations");
    lv_obj_set_flag(search_row_, LV_OBJ_FLAG_HIDDEN, favorites_visible_);
    lv_obj_set_flag(popular_button_, LV_OBJ_FLAG_HIDDEN, favorites_visible_);
    lv_obj_set_flag(refresh_button_, LV_OBJ_FLAG_HIDDEN, favorites_visible_);
    const auto &favorite_error = view_model_.favorite_error();
    lv_label_set_text(directory_status_, !favorite_error.empty() ? favorite_error.c_str() :
                       favorites_visible_ ? "My stations" : view_model_.directory_status_text().c_str());
    set_disabled(refresh_button_, loading);
    set_disabled(popular_button_, loading);
    set_disabled(load_more_button_, loading);
    lv_label_set_text(station_count_, (std::to_string(visible_stations().size()) + " stations").c_str());
    lv_obj_set_style_text_color(directory_status_,
                                lv_color_hex(failed || !favorite_error.empty() ? components::color_error : components::color_muted_text), 0);
    if (empty) {
        lv_obj_remove_flag(empty_label_, LV_OBJ_FLAG_HIDDEN);
        const char *message = favorites_visible_ ? "No favorites yet"
                              : loading  ? "Finding stations…"
                              : failed ? "Unable to load stations.\nTap refresh to try again."
                                       : "No stations found.\nTry another station name.";
        lv_label_set_text(empty_label_, message);
    } else
        lv_obj_add_flag(empty_label_, LV_OBJ_FLAG_HIDDEN);
    if (!favorites_visible_ && view_model_.has_more_stations())
        lv_obj_remove_flag(load_more_button_, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(load_more_button_, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(load_more_label_, loading ? "Loading…" : "Load more stations");
}

void RadioView::refresh_controls() noexcept
{
    if (!play_icon_) return;
    const StreamPlaybackState state = view_model_.playback_state();
    const bool show_pause           = state == StreamPlaybackState::Starting || state == StreamPlaybackState::Playing;
    lv_label_set_text(play_icon_, show_pause ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_label_set_text(mini_icon_, show_pause ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_label_set_text(mini_title_, view_model_.current_name().c_str());
    lv_label_set_text(now_url_, view_model_.current_metadata().c_str());
    std::uint32_t status_color = components::color_muted_text;
    if (state == StreamPlaybackState::Playing)
        status_color = components::color_action;
    else if (state == StreamPlaybackState::Starting || state == StreamPlaybackState::Paused)
        status_color = components::color_accent;
    else if (state == StreamPlaybackState::Error)
        status_color = components::color_error;
    lv_obj_set_style_bg_color(status_dot_, lv_color_hex(status_color), 0);
    set_disabled(stop_button_, !is_active(state));
    set_disabled(play_button_, view_model_.stations().empty() && !view_model_.has_selection());
    set_disabled(favorite_button_, !view_model_.has_selection());
    lv_obj_set_style_text_color(favorite_icon_, lv_color_hex(view_model_.current_is_favorite() ? components::color_favorite : components::color_muted_text), 0);
    tooltips_[favorite_button_] = view_model_.current_is_favorite() ? "Remove from favorites" : "Add to favorites";
    lv_obj_set_state(favorite_button_, LV_STATE_CHECKED, view_model_.current_is_favorite());
    const auto &error = view_model_.favorite_error();
    lv_label_set_text(favorite_error_, error.c_str());
    lv_obj_set_flag(favorite_error_, LV_OBJ_FLAG_HIDDEN, error.empty());
    const bool sleep_active = view_model_.sleep_timer_active();
    const std::string remaining = sleep_active ? countdown(view_model_.sleep_remaining_seconds()) : "Off";
    lv_label_set_text(sleep_label_, (std::string(kClockIcon) + "  " + (sleep_active ? remaining : "Sleep Timer")).c_str());
    lv_label_set_text(sleep_countdown_, remaining.c_str());
    lv_label_set_text(settings_sleep_status_, remaining.c_str());
    lv_obj_set_state(sleep_button_, LV_STATE_CHECKED, sleep_active);
    set_disabled(sleep_cancel_, !sleep_active);
    lv_obj_set_flag(mini_player_, LV_OBJ_FLAG_HIDDEN, active_tab_ == 0 || !view_model_.has_selection());
    const auto &stations = visible_stations();
    for (std::size_t index = 0; index < station_rows_.size() && index < stations.size(); ++index) {
        const bool selected = view_model_.is_current_station(stations[index]);
        lv_obj_set_style_bg_color(
            station_rows_[index],
            lv_color_hex(selected ? components::color_selected_background : components::color_container_background), 0);
        lv_label_set_text(station_actions_[index], selected && show_pause ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        const bool favorite = view_model_.is_favorite(stations[index]);
        auto *button = station_favorites_[index];
        lv_obj_set_state(button, LV_STATE_CHECKED, favorite);
        lv_obj_set_style_text_color(lv_obj_get_child(button, 0), lv_color_hex(favorite ? components::color_favorite : components::color_muted_text), 0);
        tooltips_[button] = favorite ? "Remove from favorites" : "Add to favorites";
    }
    refresh_artwork(current_artwork_, view_model_.current_station().favicon, true);
}

void RadioView::apply_layout() noexcept
{
    if (!surface_ || !body_ || !player_panel_ || !station_panel_ || !settings_panel_ || !url_dialog_.panel || !search_keyboard_) return;
    const int width        = lv_obj_get_content_width(surface_);
    const int height       = lv_obj_get_content_height(surface_);
    const bool wide        = width >= 800;
    const bool settings    = active_tab_ == 3;
    lv_obj_set_style_pad_all(body_, 0, 0);
    lv_obj_set_flex_flow(body_, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(body_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_grow(player_panel_, 0);
    lv_obj_set_flag(player_panel_, LV_OBJ_FLAG_HIDDEN, active_tab_ != 0);
    lv_obj_set_flag(station_panel_, LV_OBJ_FLAG_HIDDEN, settings || active_tab_ == 0);
    lv_obj_set_flag(settings_panel_, LV_OBJ_FLAG_HIDDEN, !settings);
    lv_obj_set_flag(mini_player_, LV_OBJ_FLAG_HIDDEN, active_tab_ == 0 || !view_model_.has_selection());
    if (wide) {
        lv_obj_set_size(player_panel_, active_tab_ == 0 ? LV_PCT(100) : std::clamp(width / 3, 320, 440), LV_PCT(100));
        lv_obj_set_size(station_panel_, 0, LV_PCT(100));
        lv_obj_set_flex_grow(station_panel_, 1);
    } else {
        lv_obj_set_size(player_panel_, LV_PCT(100), LV_PCT(100));
        lv_obj_set_size(station_panel_, LV_PCT(100), LV_PCT(100));
        lv_obj_set_flex_grow(station_panel_, 0);
    }
    const int keyboard_height = wide ? 184 : std::min(260, height / 3);
    const int available = height - 72;
    lv_obj_set_flag(artwork_top_space_, LV_OBJ_FLAG_HIDDEN, available < 650);
    lv_obj_set_flag(artwork_bottom_space_, LV_OBJ_FLAG_HIDDEN, available < 650);
    const int art_size = std::clamp(available - 290, 96, std::max(96, std::min(320, wide ? std::clamp(width / 3, 320, 440) - 48 : width - 48)));
    if (current_artwork_.frame) lv_obj_set_size(current_artwork_.frame, art_size, art_size);
    if (available < 370) {
        lv_obj_add_flag(player_panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_align(player_panel_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    } else {
        lv_obj_remove_flag(player_panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_align(player_panel_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }
    lv_obj_set_height(search_keyboard_, keyboard_height);
    lv_obj_set_height(url_dialog_.keyboard, keyboard_height);
    lv_obj_set_width(url_dialog_.panel, std::min(620, width - 32));
    lv_obj_set_style_max_height(url_dialog_.panel, std::max(180, height - keyboard_height - 32), 0);
    if (sleep_panel_) {
        lv_obj_set_width(sleep_panel_, std::min(380, width - 24));
        lv_obj_set_style_max_height(sleep_panel_, height - 24, 0);
        lv_obj_add_flag(sleep_panel_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(sleep_panel_, wide ? LV_ALIGN_CENTER : LV_ALIGN_BOTTOM_MID, 0, wide ? 0 : -12);
    }
}
