#ifndef LILYGO_UI_TEMPLATE_COMPONENTS_HPP
#define LILYGO_UI_TEMPLATE_COMPONENTS_HPP

#include <lvgl.h>

#include <cstdint>

namespace components {

inline constexpr std::uint32_t color_page_background      = 0xf2f2f7;
inline constexpr std::uint32_t color_container_background = 0xffffff;
inline constexpr std::uint32_t color_container_title      = 0x1a1a1a;
inline constexpr std::uint32_t color_text                 = 0x262c33;
inline constexpr std::uint32_t color_border               = 0xd8dde3;
inline constexpr std::uint32_t color_action               = 0x15735d;
inline constexpr std::uint32_t color_accent               = 0xe4573d;
inline constexpr std::uint32_t color_muted_text           = 0x68737f;
inline constexpr std::uint32_t color_control_background   = 0xedf1f4;
inline constexpr std::uint32_t color_selected_background  = 0xe7f4ef;
inline constexpr std::uint32_t color_error                = 0xc83e4d;
// Filled favorite stars, opaque gold on white or selected backgrounds.
inline constexpr std::uint32_t color_favorite             = 0xc88a16;
// Modal backdrop, rendered at 40% opacity over the application.
inline constexpr std::uint32_t color_overlay = 0x18221e;

lv_obj_t *create_page(lv_obj_t *parent);
lv_obj_t *create_header(lv_obj_t *parent, const char *title);
lv_obj_t *create_card(lv_obj_t *parent);
lv_obj_t *create_button(lv_obj_t *parent, const char *text, lv_event_cb_t callback, void *user_data);

}  // namespace components

#endif
