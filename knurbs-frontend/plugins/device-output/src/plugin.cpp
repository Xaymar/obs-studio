// knurbs-device-output: sends the program, the preview or the audio mix to
// a device (DeckLink, AJA, ASIO). Replaces decklink-output-ui, aja-output-ui
// and asio-output-ui.
//
// Loading only registers a frontend event callback. Qt objects are created
// once the frontend reports OBS_FRONTEND_EVENT_FINISHED_LOADING; until then
// Qt is not guaranteed to exist and the frontend refuses UI calls.

#include "output-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QPointer>
#include <QWidget>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("knurbs-device-output", "en-US")

using knurbs::device_output::OutputDock;

namespace {

constexpr const char *kDockId = "com.xaymar.knurbs.dock.device-output";

QPointer<OutputDock> dock;

// Everything that needs Qt, in one place, so it can move to the knurbs_
// frontend API when that exists.
void setup_ui()
{
	auto *window = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *widget = new OutputDock(window);
	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("DockTitle"), widget)) {
		blog(LOG_WARNING, "[knurbs-device-output] The frontend did not accept the dock");
		delete widget;
		return;
	}
	dock = widget;
	dock->loadProfile();
}

void on_event(obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		if (!dock) {
			setup_ui();
		}
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
		if (dock) {
			dock->stopAll();
		}
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		if (dock) {
			dock->loadProfile();
		}
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		// Sent before libobs shuts down: the last point where outputs can
		// be stopped and their settings saved.
		if (dock) {
			dock->stopAll();
		}
		break;
	default:
		if (dock) {
			dock->handleFrontendEvent(event);
		}
		break;
	}
}

} // namespace

bool obs_module_load(void)
{
	obs_frontend_add_event_callback(on_event, nullptr);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_event, nullptr);
}

const char *obs_module_name(void)
{
	return "Device Outputs";
}

const char *obs_module_description(void)
{
	return "Sends the program, the preview or the audio mix to DeckLink, AJA and ASIO devices.";
}
