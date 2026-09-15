#include "components/host_text_input.hpp"

#if defined(CM0_APP_SIMULATOR)
#include <src/misc/lv_text_private.h>

#include <algorithm>
#include <string>

HostTextInput::~HostTextInput()
{
    focus(nullptr);
    if (watching_) SDL_DelEventWatch(watch_event, this);
    if (timer_) lv_timer_delete(timer_);
}

void HostTextInput::focus(lv_obj_t *input)
{
    if (input_) {
        lv_textarea_clear_selection(input_);
        lv_obj_remove_state(input_, LV_STATE_FOCUSED);
    }
    input_ = input;
    if (input_) {
        lv_obj_add_state(input_, LV_STATE_FOCUSED);
        lv_textarea_set_text_selection(input_, true);
    }

    Uint32 window_id = 0;
    if (SDL_WasInit(SDL_INIT_VIDEO)) {
        if (input_) {
            auto *window = lv_sdl_window_get_window(lv_obj_get_display(input_));
            if (window) window_id = SDL_GetWindowID(window);
            SDL_StartTextInput();
        } else {
            SDL_StopTextInput();
        }
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        window_id_ = window_id;
        events_.clear();
    }
    if (window_id && !watching_) {
        SDL_AddEventWatch(watch_event, this);
        watching_ = true;
        timer_    = lv_timer_create(poll_events, 5, this);
    }
}

int HostTextInput::watch_event(void *context, SDL_Event *event)
{
    if (event->type != SDL_KEYDOWN && event->type != SDL_TEXTINPUT) return 1;
    auto *self             = static_cast<HostTextInput *>(context);
    const Uint32 window_id = event->type == SDL_KEYDOWN ? event->key.windowID : event->text.windowID;
    // SDL can invoke event watches off-thread; apply edits only in the LVGL timer.
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (self->window_id_ && self->window_id_ == window_id) self->events_.push_back(*event);
    return 1;
}

void HostTextInput::poll_events(lv_timer_t *timer)
{
    auto *self = static_cast<HostTextInput *>(lv_timer_get_user_data(timer));
    std::vector<SDL_Event> events;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        events.swap(self->events_);
    }
    for (const auto &event : events) {
        if (!self->input_) break;
        if (event.type == SDL_TEXTINPUT) {
            self->erase_selection();
            lv_textarea_add_text(self->input_, event.text.text);
        } else {
            self->handle_key(event.key);
        }
    }
}

bool HostTextInput::erase_selection()
{
    auto *label      = lv_textarea_get_label(input_);
    const auto start = lv_label_get_text_selection_start(label);
    const auto end   = lv_label_get_text_selection_end(label);
    if (start == LV_LABEL_TEXT_SELECTION_OFF || end == LV_LABEL_TEXT_SELECTION_OFF || start == end) return false;
    std::string text      = lv_textarea_get_text(input_);
    const auto first      = std::min(start, end);
    const auto byte_start = lv_text_encoded_get_byte_id(text.c_str(), first);
    const auto byte_end   = lv_text_encoded_get_byte_id(text.c_str(), std::max(start, end));
    text.erase(byte_start, byte_end - byte_start);
    lv_textarea_set_text(input_, text.c_str());
    lv_textarea_set_cursor_pos(input_, static_cast<int>(first));
    return true;
}

void HostTextInput::copy_selection(bool cut)
{
    auto *label      = lv_textarea_get_label(input_);
    const auto start = lv_label_get_text_selection_start(label);
    const auto end   = lv_label_get_text_selection_end(label);
    if (start == LV_LABEL_TEXT_SELECTION_OFF || end == LV_LABEL_TEXT_SELECTION_OFF || start == end) return;
    const char *text      = lv_textarea_get_text(input_);
    const auto byte_start = lv_text_encoded_get_byte_id(text, std::min(start, end));
    const auto byte_end   = lv_text_encoded_get_byte_id(text, std::max(start, end));
    const std::string selected(text + byte_start, byte_end - byte_start);
    if (SDL_SetClipboardText(selected.c_str()) == 0 && cut) erase_selection();
}

void HostTextInput::move_cursor(int position, bool selecting)
{
    auto *label       = lv_textarea_get_label(input_);
    const auto cursor = lv_textarea_get_cursor_pos(input_);
    auto anchor       = cursor;
    const auto start  = lv_label_get_text_selection_start(label);
    const auto end    = lv_label_get_text_selection_end(label);
    if (start != LV_LABEL_TEXT_SELECTION_OFF && end != LV_LABEL_TEXT_SELECTION_OFF && start != end)
        anchor = cursor == start ? end : start;
    lv_textarea_set_cursor_pos(
        input_, std::clamp(position, 0, static_cast<int>(lv_text_get_encoded_length(lv_textarea_get_text(input_)))));
    lv_textarea_clear_selection(input_);
    const auto next = lv_textarea_get_cursor_pos(input_);
    if (selecting && anchor != next) {
        lv_label_set_text_selection_start(label, std::min(anchor, next));
        lv_label_set_text_selection_end(label, std::max(anchor, next));
    }
}

void HostTextInput::handle_key(const SDL_KeyboardEvent &event)
{
    const auto key = event.keysym.sym;
#if defined(__APPLE__)
    const bool shortcut = (event.keysym.mod & KMOD_GUI) != 0;
#else
    const bool shortcut = (event.keysym.mod & KMOD_CTRL) != 0;
#endif
    const bool shift = (event.keysym.mod & KMOD_SHIFT) != 0;
    const int length = static_cast<int>(lv_text_get_encoded_length(lv_textarea_get_text(input_)));
    const int cursor = static_cast<int>(lv_textarea_get_cursor_pos(input_));
    if (shortcut) {
        if (key == SDLK_a) {
            move_cursor(0, false);
            move_cursor(length, true);
        } else if (key == SDLK_c || key == SDLK_x) {
            copy_selection(key == SDLK_x);
        } else if (key == SDLK_v) {
            char *text = SDL_GetClipboardText();
            if (text && *text) {
                erase_selection();
                lv_textarea_add_text(input_, text);
            }
            SDL_free(text);
        } else if (key == SDLK_LEFT || key == SDLK_RIGHT) {
            move_cursor(key == SDLK_LEFT ? 0 : length, shift);
        }
        return;
    }
    switch (key) {
        case SDLK_LEFT:
        case SDLK_RIGHT: {
            auto *label      = lv_textarea_get_label(input_);
            const auto start = lv_label_get_text_selection_start(label);
            const auto end   = lv_label_get_text_selection_end(label);
            const bool selected =
                start != LV_LABEL_TEXT_SELECTION_OFF && end != LV_LABEL_TEXT_SELECTION_OFF && start != end;
            const int next = selected && !shift
                                 ? static_cast<int>(key == SDLK_LEFT ? std::min(start, end) : std::max(start, end))
                                 : cursor + (key == SDLK_LEFT ? -1 : 1);
            move_cursor(next, shift);
            break;
        }
        case SDLK_HOME:
        case SDLK_UP:
            move_cursor(0, shift);
            break;
        case SDLK_END:
        case SDLK_DOWN:
            move_cursor(length, shift);
            break;
        case SDLK_BACKSPACE:
            if (!erase_selection()) lv_textarea_delete_char(input_);
            break;
        case SDLK_DELETE:
            if (!erase_selection()) lv_textarea_delete_char_forward(input_);
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            lv_obj_send_event(input_, LV_EVENT_READY, nullptr);
            break;
        case SDLK_ESCAPE:
            lv_obj_send_event(input_, LV_EVENT_CANCEL, nullptr);
            break;
        default:
            break;
    }
}
#else
HostTextInput::~HostTextInput() = default;
void HostTextInput::focus(lv_obj_t *)
{
}
#endif
