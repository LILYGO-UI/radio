#include "pages/radio/radio_view.hpp"

#include <SDL.h>
#include <cm0/typography.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(RADIO_TEST_SDL3_LIBRARY)
void push_compat_text_event(unsigned int window_id, const char *text);
#endif

namespace {

class TestPlayer final : public StreamPlayerInterface {
public:
    bool start(std::string_view url) override
    {
        last_url = url;
        return true;
    }
    void stop() noexcept override
    {
    }
    bool toggle_pause() noexcept override
    {
        return false;
    }
    void poll() noexcept override
    {
    }
    StreamPlaybackState state() const noexcept override
    {
        return StreamPlaybackState::Idle;
    }
    const std::string &error() const noexcept override
    {
        return error_;
    }
    std::string last_url;
    std::string error_;
};

class TestDirectory final : public RadioBrowserClientInterface {
public:
    bool search(const RadioBrowserQuery &query) override
    {
        last_query = query.name;
        return true;
    }
    bool poll(RadioBrowserResult &) override
    {
        return false;
    }
    bool loading() const noexcept override
    {
        return false;
    }
    std::string last_query;
};

#if defined(__APPLE__)
constexpr SDL_Keymod kShortcut = KMOD_GUI;
#else
constexpr SDL_Keymod kShortcut = KMOD_CTRL;
#endif

Uint32 window_id;

void pump()
{
    lv_tick_inc(10);
    lv_timer_handler();
}

void key(SDL_Keycode keycode, SDL_Keymod modifiers = KMOD_NONE)
{
    SDL_Event event{};
    event.type           = SDL_KEYDOWN;
    event.key.windowID   = window_id;
    event.key.state      = SDL_PRESSED;
    event.key.keysym.sym = keycode;
    event.key.keysym.scancode = SDL_GetScancodeFromKey(keycode);
    event.key.keysym.mod = modifiers;
    assert(SDL_PushEvent(&event) == 1);
    pump();
}

void text(const char *value)
{
#if defined(RADIO_TEST_SDL3_LIBRARY)
    push_compat_text_event(window_id, value);
#else
    SDL_Event event{};
    event.type          = SDL_TEXTINPUT;
    event.text.windowID = window_id;
    assert(std::strlen(value) < sizeof(event.text.text));
    SDL_strlcpy(event.text.text, value, sizeof(event.text.text));
    assert(SDL_PushEvent(&event) == 1);
#endif
    pump();
}

lv_obj_t *find(const char *name)
{
    auto *object = lv_obj_find_by_name(lv_screen_active(), name);
    assert(object);
    return object;
}

void click(const char *name)
{
    lv_obj_send_event(find(name), LV_EVENT_CLICKED, nullptr);
}

void expect_text(lv_obj_t *input, const char *expected)
{
    if (std::string(lv_textarea_get_text(input)) != expected)
        std::fprintf(stderr, "Expected [%s], got [%s]\n", expected, lv_textarea_get_text(input));
    assert(std::string(lv_textarea_get_text(input)) == expected);
}

void expect_clipboard(const char *expected)
{
    char *clipboard = SDL_GetClipboardText();
    assert(clipboard && std::string(clipboard) == expected);
    SDL_free(clipboard);
}

}  // namespace

int main()
{
    // Use SDL's isolated clipboard so tests never modify the user's clipboard.
    assert(SDL_setenv("SDL_VIDEODRIVER", "dummy", 1) == 0);
    lv_init();
    auto *display = lv_sdl_window_create(1232, 568);
    assert(display);
    lv_tick_set_cb(nullptr);
    window_id = SDL_GetWindowID(lv_sdl_window_get_window(display));
    {
        TestPlayer player;
        TestDirectory directory;
        RadioViewModel model(player, directory, "");
        RadioView view(model);
        view.create(lv_screen_active(), "Internet Radio");
        auto *input = find("radio_url_input");
        click("radio_custom");

        text("https://example.com/liv");
        text("e");
        expect_text(input, "https://example.com/live");
        key(SDLK_LEFT);
        key(SDLK_BACKSPACE);
        expect_text(input, "https://example.com/lie");
        key(SDLK_DELETE);
        text("ve");
        expect_text(input, "https://example.com/live");

        key(SDLK_a, kShortcut);
        assert(lv_textarea_text_is_selected(input));
        key(SDLK_c, kShortcut);
        expect_clipboard("https://example.com/live");
        key(SDLK_x, kShortcut);
        expect_text(input, "");
        key(SDLK_v, kShortcut);
        expect_text(input, "https://example.com/live");
        key(SDLK_a, kShortcut);
        key(SDLK_v, kShortcut);
        expect_text(input, "https://example.com/live");

        // UTF-8 selection uses character offsets, including committed IME text.
        key(SDLK_a, kShortcut);
        text("\xe4\xb8\xad\xe6\x96\x87");
        key(SDLK_LEFT, KMOD_SHIFT);
        key(SDLK_c, kShortcut);
        expect_clipboard("\xe6\x96\x87");
        key(SDLK_BACKSPACE);
        expect_text(input, "\xe4\xb8\xad");
        key(SDLK_HOME);
        key(SDLK_DELETE);
        expect_text(input, "");

        // Invalid URLs stay open; a valid URL submits via the physical Enter key.
        key(SDLK_RETURN);
        assert(!lv_obj_has_flag(find("radio_url_overlay"), LV_OBJ_FLAG_HIDDEN));
        assert(SDL_SetClipboardText("https://example.com/live?a=1&b=2") == 0);
        key(SDLK_v, kShortcut);
        key(SDLK_RETURN);
        assert(player.last_url == "https://example.com/live?a=1&b=2");
        assert(lv_obj_has_flag(find("radio_url_overlay"), LV_OBJ_FLAG_HIDDEN));
        text("hidden");
        expect_text(input, "https://example.com/live?a=1&b=2");

        click("radio_tab_stations");
        click("radio_search_input");
        auto *search = find("radio_search_input");
        assert(lv_obj_get_parent(search) == find("radio_search"));
        assert(!lv_obj_find_by_name(lv_screen_active(), "radio_search_overlay"));
        assert(!lv_obj_has_flag(find("radio_search_keyboard"), LV_OBJ_FLAG_HIDDEN));
        text("Jazz");
        // Events belonging to another window must not edit the focused input.
        ++window_id;
        text("wrong window");
        --window_id;
        expect_text(search, "Jazz");
        key(SDLK_RETURN);
        assert(directory.last_query == "Jazz");
        assert(lv_obj_has_flag(find("radio_search_keyboard"), LV_OBJ_FLAG_HIDDEN));
        assert(!lv_obj_has_state(search, LV_STATE_FOCUSED));
        text("unfocused");
        expect_text(search, "Jazz");

        click("radio_search_input");
        key(SDLK_a, kShortcut);
        const std::string long_query(150, 'a');
        assert(SDL_SetClipboardText(long_query.c_str()) == 0);
        key(SDLK_v, kShortcut);
        assert(std::strlen(lv_textarea_get_text(search)) == 120);
        key(SDLK_ESCAPE);
        assert(lv_obj_has_flag(find("radio_search_keyboard"), LV_OBJ_FLAG_HIDDEN));
        assert(directory.last_query == "Jazz");

        click("radio_search_input");
        key(SDLK_a, kShortcut);
        text("Draft");
        // Programmatic overlay changes must release focus even when the keyboard covers its trigger.
        click("radio_sleep");
        assert(lv_obj_has_flag(find("radio_search_keyboard"), LV_OBJ_FLAG_HIDDEN));
        assert(!lv_obj_has_state(search, LV_STATE_FOCUSED));
        assert(lv_obj_is_visible(find("radio_sleep_overlay")));
        text("under overlay");
        expect_text(search, "Draft");
        click("radio_sleep_close");
        click("radio_search_input");
        // A programmatic page change must also detach the keyboard from the hidden search input.
        click("radio_tab_settings");
        assert(lv_obj_has_flag(find("radio_search_keyboard"), LV_OBJ_FLAG_HIDDEN));
        assert(lv_obj_is_visible(find("radio_settings_panel")));
        text("unfocused");
        expect_text(search, "Draft");
        assert(directory.last_query == "Jazz");
        click("radio_tab_stations");
        click("radio_search_input");
        key(SDLK_RETURN);
        assert(directory.last_query == "Draft");

        // Teardown with an open dialog must detach its queued keyboard events.
        click("radio_custom");
        view.destroy();
        text("after destroy");
    }
    lilygo_ui_fonts_deinit();
    lv_deinit();
    return 0;
}
