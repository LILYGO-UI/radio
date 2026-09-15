#include "app.hpp"

#include "app_identity.h"
#include "pages/radio/radio_view.hpp"
#include "services/stream_player.hpp"

#include <memory>

namespace {

struct AppState {
    std::unique_ptr<StreamPlayer> player;
    std::unique_ptr<RadioViewModel> radio_view_model;
    std::unique_ptr<RadioView> radio_view;
};

AppState app_state;

void app_open(const cm0_app_context_t *context)
{
    app_state.player           = std::make_unique<StreamPlayer>();
    app_state.radio_view_model = std::make_unique<RadioViewModel>(*app_state.player);
    (void)app_state.radio_view_model->load_radio_browser_stations();
    app_state.radio_view       = std::make_unique<RadioView>(*app_state.radio_view_model);
    app_state.radio_view->create(context->root, LILYGO_UI_RADIO_APP_NAME);
}

void app_close()
{
    if (app_state.radio_view) app_state.radio_view->destroy();
    app_state.radio_view.reset();
    app_state.radio_view_model.reset();
    if (app_state.player) app_state.player->stop();
    app_state.player.reset();
}

const cm0_app_descriptor_t app_descriptor{
    CM0_APP_API_VERSION, sizeof(cm0_app_descriptor_t), LILYGO_UI_RADIO_APP_ID, app_open, app_close,
};

}  // namespace

extern "C" const cm0_app_descriptor_t *cm0_app_get_descriptor()
{
    return &app_descriptor;
}
