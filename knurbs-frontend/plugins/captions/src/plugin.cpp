// knurbs-captions: passes captions on to the stream and the recording.
// Replaces decklink-captions, which despite its name took CEA-708 captions
// from any source that carries them, and only ever sent them to the stream.
//
// Captions come from:
// - a source that carries CEA-708 captions (DeckLink and others), picked in
//   the dock and saved with the scene collection;
// - plugins that produce caption text, such as speech recognition, through
//   knurbs_captions_submit_text() in knurbs-captions-api.h.
//
// Loading registers libobs and frontend callbacks only. Qt objects are made
// after OBS_FRONTEND_EVENT_FINISHED_LOADING.

#include "captions-dock.hpp"
#include "captions.hpp"

#include <knurbs-captions-api.h>

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QMetaObject>
#include <QPointer>
#include <QWidget>

#include <memory>
#include <mutex>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("knurbs-captions", "en-US")

using knurbs::captions::Captions;
using knurbs::captions::CaptionsConfig;
using knurbs::captions::CaptionsDock;

namespace {

constexpr const char *kDockId = "com.xaymar.knurbs.dock.captions";

// The procedure handler has no way to remove a procedure, so the procedure
// stays registered after unload. It reaches the instance only through this
// pointer, which unload clears.
std::mutex instance_mutex;
Captions *instance = nullptr;

std::unique_ptr<Captions> captions;
QPointer<CaptionsDock> dock;

void submit_text(void *, calldata_t *cd)
{
	std::lock_guard lock(instance_mutex);
	if (instance) {
		Captions::submitTextProc(instance, cd);
	} else {
		calldata_set_bool(cd, "delivered", false);
	}
}

std::string uuid_for_name(const char *name)
{
	OBSSourceAutoRelease source = obs_get_source_by_name(name);
	return source ? obs_source_get_uuid(source) : std::string();
}

void refresh_dock_later()
{
	// Source signals arrive on whatever thread made the change.
	if (dock) {
		QMetaObject::invokeMethod(
			dock.data(),
			[]() {
				if (dock) {
					dock->refresh();
				}
			},
			Qt::QueuedConnection);
	}
}

void on_source_list_changed(void *, calldata_t *)
{
	refresh_dock_later();
}

void setup_ui()
{
	auto *window = static_cast<QWidget *>(obs_frontend_get_main_window());
	auto *widget = new CaptionsDock(*captions, window);
	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("DockTitle"), widget)) {
		blog(LOG_WARNING, "[knurbs-captions] The frontend did not accept the dock");
		delete widget;
		return;
	}
	dock = widget;

	signal_handler_t *handler = obs_get_signal_handler();
	signal_handler_connect(handler, "source_create", on_source_list_changed, nullptr);
	signal_handler_connect(handler, "source_destroy", on_source_list_changed, nullptr);
	signal_handler_connect(handler, "source_rename", on_source_list_changed, nullptr);
}

void on_save(obs_data_t *data, bool saving, void *)
{
	if (saving) {
		knurbs::captions::config_to_data(captions->config(), data);
		return;
	}

	captions->detach();
	captions->apply(knurbs::captions::config_from_data(data, uuid_for_name));
	captions->refreshOutputs();
	if (dock) {
		dock->refresh();
	}
}

void on_event(obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		captions->refreshOutputs();
		if (!dock) {
			setup_ui();
		}
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
	case OBS_FRONTEND_EVENT_RECORDING_STARTING:
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		// The frontend may make new output objects for each run.
		captions->refreshOutputs();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		// Its sources are about to go away; the next load re-attaches.
		captions->detach();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		captions->detach();
		if (dock) {
			signal_handler_t *handler = obs_get_signal_handler();
			signal_handler_disconnect(handler, "source_create", on_source_list_changed, nullptr);
			signal_handler_disconnect(handler, "source_destroy", on_source_list_changed, nullptr);
			signal_handler_disconnect(handler, "source_rename", on_source_list_changed, nullptr);
		}
		break;
	default:
		break;
	}
}

} // namespace

bool obs_module_load(void)
{
	captions = std::make_unique<Captions>();
	{
		std::lock_guard lock(instance_mutex);
		instance = captions.get();
	}

	proc_handler_add(obs_get_proc_handler(), KNURBS_CAPTIONS_SUBMIT_TEXT_DECL, submit_text, nullptr);
	obs_frontend_add_event_callback(on_event, nullptr);
	obs_frontend_add_save_callback(on_save, nullptr);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_save_callback(on_save, nullptr);
	obs_frontend_remove_event_callback(on_event, nullptr);
	{
		std::lock_guard lock(instance_mutex);
		instance = nullptr;
	}
	captions.reset();
}

const char *obs_module_name(void)
{
	return "Captions";
}

const char *obs_module_description(void)
{
	return "Passes captions from sources and caption plugins on to the stream and the recording.";
}
