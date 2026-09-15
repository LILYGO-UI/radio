#ifndef LILYGO_UI_RADIO_PAGES_RADIO_RADIO_VIEW_HPP
#define LILYGO_UI_RADIO_PAGES_RADIO_RADIO_VIEW_HPP

#include "pages/radio/radio_view_model.hpp"
#include "components/host_text_input.hpp"
#include "services/artwork_cache.hpp"

#include <array>
#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <lvgl.h>

class RadioView {
public:
    explicit RadioView(RadioViewModel &view_model) noexcept;
    ~RadioView();

    lv_obj_t *create(lv_obj_t *parent, const char *application_name);
    void destroy() noexcept;

private:
    struct InputDialog {
        lv_obj_t *overlay  = nullptr;
        lv_obj_t *panel    = nullptr;
        lv_obj_t *input    = nullptr;
        lv_obj_t *error    = nullptr;
        lv_obj_t *keyboard = nullptr;
    };
    struct ArtworkView {
        lv_obj_t *frame = nullptr;
        lv_obj_t *image = nullptr;
        lv_obj_t *fallback = nullptr;
        std::string url;
        const Artwork *loaded = nullptr;
    };

    static void size_changed_event(lv_event_t *event);
    static void station_event(lv_event_t *event);
    static void play_pause_event(lv_event_t *event);
    static void stop_event(lv_event_t *event);
    static void custom_event(lv_event_t *event);
    static void search_event(lv_event_t *event);
    static void refresh_event(lv_event_t *event);
    static void popular_event(lv_event_t *event);
    static void load_more_event(lv_event_t *event);
    static void cancel_url_event(lv_event_t *event);
    static void submit_url_event(lv_event_t *event);
    static void submit_search_event(lv_event_t *event);
    static void keyboard_event(lv_event_t *event);
    static void search_keyboard_event(lv_event_t *event);
    static void poll_timer(lv_timer_t *timer);
    static void tab_event(lv_event_t *event);
    static void favorite_event(lv_event_t *event);
    static void row_favorite_event(lv_event_t *event);
    static void sleep_event(lv_event_t *event);
    static void sleep_close_event(lv_event_t *event);
    static void sleep_start_event(lv_event_t *event);
    static void sleep_cancel_event(lv_event_t *event);
    static void mini_player_event(lv_event_t *event);
    static void tooltip_event(lv_event_t *event);

    lv_obj_t *create_player_panel(lv_obj_t *parent);
    lv_obj_t *create_station_panel(lv_obj_t *parent);
    lv_obj_t *create_settings_panel(lv_obj_t *parent);
    lv_obj_t *create_station_row(lv_obj_t *parent, const RadioStation &station, std::size_t index);
    void create_input_dialog(InputDialog &dialog);
    void create_navigation();
    void create_sleep_dialog();
    ArtworkView create_artwork(lv_obj_t *parent, int size, const char *name);
    void refresh_artwork(ArtworkView &artwork, const std::string &url, bool priority = false);
    void refresh_artworks();
    void select_tab(int tab);
    void add_tooltip(lv_obj_t *button, const char *text);
    [[nodiscard]] const std::vector<RadioStation> &visible_stations() const noexcept;
    void show_url_dialog();
    void show_search_keyboard();
    void hide_search_keyboard() noexcept;
    void hide_dialog(InputDialog &dialog) noexcept;
    void submit_url();
    void submit_search();
    void rebuild_stations();
    void refresh_directory();
    void refresh_controls() noexcept;
    void apply_layout() noexcept;

    RadioViewModel &view_model_;
    lv_obj_t *surface_          = nullptr;
    lv_obj_t *body_             = nullptr;
    lv_obj_t *player_panel_     = nullptr;
    lv_obj_t *station_panel_    = nullptr;
    lv_obj_t *settings_panel_   = nullptr;
    lv_obj_t *settings_sleep_status_ = nullptr;
    lv_obj_t *station_list_     = nullptr;
    lv_obj_t *station_count_    = nullptr;
    lv_obj_t *directory_status_ = nullptr;
    lv_obj_t *empty_label_      = nullptr;
    lv_obj_t *search_input_     = nullptr;
    lv_obj_t *search_keyboard_  = nullptr;
    lv_obj_t *refresh_button_   = nullptr;
    lv_obj_t *popular_button_   = nullptr;
    lv_obj_t *load_more_button_ = nullptr;
    lv_obj_t *load_more_label_  = nullptr;
    lv_obj_t *now_title_        = nullptr;
    lv_obj_t *now_url_          = nullptr;
    lv_obj_t *status_label_     = nullptr;
    lv_obj_t *status_dot_       = nullptr;
    lv_obj_t *play_button_      = nullptr;
    lv_obj_t *play_icon_        = nullptr;
    lv_obj_t *stop_button_      = nullptr;
    lv_obj_t *favorite_button_  = nullptr;
    lv_obj_t *favorite_icon_    = nullptr;
    lv_obj_t *favorite_error_   = nullptr;
    lv_obj_t *sleep_button_     = nullptr;
    lv_obj_t *sleep_label_      = nullptr;
    lv_obj_t *sleep_overlay_    = nullptr;
    lv_obj_t *sleep_panel_      = nullptr;
    lv_obj_t *sleep_minutes_    = nullptr;
    lv_obj_t *sleep_countdown_  = nullptr;
    lv_obj_t *sleep_cancel_     = nullptr;
    lv_obj_t *navigation_      = nullptr;
    lv_obj_t *mini_player_     = nullptr;
    lv_obj_t *mini_title_      = nullptr;
    lv_obj_t *mini_icon_       = nullptr;
    lv_obj_t *station_heading_ = nullptr;
    lv_obj_t *artwork_top_space_ = nullptr;
    lv_obj_t *artwork_bottom_space_ = nullptr;
    lv_obj_t *search_row_      = nullptr;
    lv_obj_t *directory_row_   = nullptr;
    lv_obj_t *tooltip_         = nullptr;
    std::map<lv_obj_t *, std::string> tooltips_;
    std::array<lv_obj_t *, 4> tabs_{};
    ArtworkCache artwork_cache_;
    std::map<std::string, lv_image_dsc_t> artwork_sources_;
    std::set<std::string> requested_artwork_;
    ArtworkView current_artwork_;
    std::vector<ArtworkView> station_artworks_;
    std::vector<lv_obj_t *> station_favorites_;
    int active_tab_ = 1;
    bool favorites_visible_ = false;
    int favorites_revision_ = -1;
    InputDialog url_dialog_;
    HostTextInput host_text_input_;
    lv_timer_t *poll_timer_ = nullptr;
    int stations_revision_  = -1;
    std::vector<lv_obj_t *> station_rows_;
    std::vector<lv_obj_t *> station_actions_;
};

#endif
