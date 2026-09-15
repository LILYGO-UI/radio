#include <SDL3/SDL_events.h>
#include <dlfcn.h>

#include <cassert>

// SDL2-compat rejects synthetic SDL_TEXTINPUT events. Inject them through its
// underlying SDL3 runtime, which then delivers the usual SDL2 event watches.
void push_compat_text_event(unsigned int window_id, const char *text)
{
    static void *library = dlopen(RADIO_TEST_SDL3_LIBRARY, RTLD_NOW | RTLD_LOCAL);
    assert(library);
    using PushEvent  = bool (*)(SDL_Event *);
    static auto push = reinterpret_cast<PushEvent>(dlsym(library, "SDL_PushEvent"));
    assert(push);
    SDL_Event event{};
    event.type          = SDL_EVENT_TEXT_INPUT;
    event.text.windowID = window_id;
    event.text.text     = text;
    assert(push(&event));
}
