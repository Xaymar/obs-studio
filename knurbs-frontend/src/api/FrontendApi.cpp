#include "FrontendApi.hpp"

#include "core/Core.hpp"
#include "shell/MainWindow.hpp"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QMenu>
#include <QPalette>
#include <QThread>

#include <util/bmem.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <set>

namespace knurbs::api {

namespace {

FrontendApi *current = nullptr;

// Module load phase state. A worker thread knows which module it is loading,
// so queued registrations can be applied in module order at the barrier.
thread_local int loadingIndex = -1;
thread_local std::string loadingName;

struct Pending {
	int module;
	uint64_t seq;
	std::function<void()> apply;
};

QMutex pendingMutex;
std::atomic<bool> loading{false};
std::atomic<uint64_t> pendingSeq{0};
std::vector<Pending> pending;

bool on_gui_thread()
{
	return QThread::currentThread() == qApp->thread();
}

// Runs `apply` now, or queues it if called from a module loading on a worker.
void apply_or_queue(std::function<void()> apply)
{
	if (loading.load() && !on_gui_thread()) {
		QMutexLocker lock(&pendingMutex);
		pending.push_back({loadingIndex, pendingSeq.fetch_add(1), std::move(apply)});
		return;
	}
	apply();
}

void not_implemented(const char *function)
{
	static QMutex mutex;
	static std::set<std::string> reported;
	QMutexLocker lock(&mutex);
	if (reported.insert(function).second) {
		blog(LOG_WARNING, "[knurbs] obs-frontend-api: %s is not implemented yet", function);
	}
}

#define NOT_IMPLEMENTED() not_implemented(__func__)

char *dup(const std::string &s)
{
	return bstrdup(s.c_str());
}

void add_sources(obs_frontend_source_list *list, const std::vector<obs_source_t *> &sources)
{
	for (obs_source_t *source : sources) {
		if (obs_source_t *ref = obs_source_get_ref(source)) {
			da_push_back(list->sources, &ref);
		}
	}
}

} // namespace

FrontendApi::FrontendApi(Core &core, MainWindow &window, bool safeMode)
	: core_(core),
	  window_(window),
	  safeMode_(safeMode)
{
}

FrontendApi::~FrontendApi()
{
	if (current == this) {
		current = nullptr;
	}
}

FrontendApi *FrontendApi::install(Core &core, MainWindow &window, bool safeMode)
{
	auto *api = new FrontendApi(core, window, safeMode);
	current = api;
	obs_frontend_set_callbacks_internal(api); // takes ownership
	return api;
}

FrontendApi *FrontendApi::instance()
{
	return current;
}

void FrontendApi::beginModuleLoad()
{
	loading.store(true);
}

void FrontendApi::endModuleLoad()
{
	loading.store(false);

	std::vector<Pending> work;
	{
		QMutexLocker lock(&pendingMutex);
		work.swap(pending);
	}
	std::stable_sort(work.begin(), work.end(), [](const Pending &a, const Pending &b) {
		return a.module != b.module ? a.module < b.module : a.seq < b.seq;
	});
	for (Pending &p : work) {
		p.apply();
	}
}

void FrontendApi::setLoadingModule(int index, const std::string &name)
{
	loadingIndex = index;
	loadingName = name;
}

bool FrontendApi::refuseDuringLoad(const char *function) const
{
	if (!loading.load() || on_gui_thread()) {
		return false;
	}

	blog(LOG_ERROR,
	     "[knurbs] Module '%s' called %s while loading. UI work must wait for "
	     "OBS_FRONTEND_EVENT_FINISHED_LOADING; the call was ignored.",
	     loadingName.c_str(), function);
	return true;
}

// ---------------------------------------------------------------- window

void *FrontendApi::obs_frontend_get_main_window(void)
{
	return static_cast<QMainWindow *>(&window_);
}

void *FrontendApi::obs_frontend_get_main_window_handle(void)
{
	if (refuseDuringLoad(__func__)) {
		return nullptr;
	}
	return reinterpret_cast<void *>(window_.winId());
}

void *FrontendApi::obs_frontend_get_system_tray(void)
{
	return nullptr;
}

// ---------------------------------------------------------------- scenes

void FrontendApi::obs_frontend_get_scenes(obs_frontend_source_list *sources)
{
	add_sources(sources, core_.scenes());
}

obs_source_t *FrontendApi::obs_frontend_get_current_scene(void)
{
	return obs_source_get_ref(core_.currentScene());
}

void FrontendApi::obs_frontend_set_current_scene(obs_source_t *scene)
{
	if (refuseDuringLoad(__func__)) {
		return;
	}
	core_.setCurrentScene(scene);
}

void FrontendApi::obs_frontend_get_transitions(obs_frontend_source_list *sources)
{
	add_sources(sources, core_.transitions());
}

obs_source_t *FrontendApi::obs_frontend_get_current_transition(void)
{
	return obs_source_get_ref(core_.currentTransition());
}

void FrontendApi::obs_frontend_set_current_transition(obs_source_t *transition)
{
	if (refuseDuringLoad(__func__)) {
		return;
	}
	core_.setCurrentTransition(transition);
}

int FrontendApi::obs_frontend_get_transition_duration(void)
{
	return core_.transitionDuration();
}

void FrontendApi::obs_frontend_set_transition_duration(int duration)
{
	core_.setTransitionDuration(duration);
}

void FrontendApi::obs_frontend_release_tbar(void)
{
	NOT_IMPLEMENTED();
}

int FrontendApi::obs_frontend_get_tbar_position(void)
{
	NOT_IMPLEMENTED();
	return 0;
}

void FrontendApi::obs_frontend_set_tbar_position(int)
{
	NOT_IMPLEMENTED();
}

// ---------------------------------------------------------------- collections and profiles

void FrontendApi::obs_frontend_get_scene_collections(std::vector<std::string> &strings)
{
	strings.push_back(core_.sceneCollectionName());
}

char *FrontendApi::obs_frontend_get_current_scene_collection(void)
{
	return dup(core_.sceneCollectionName());
}

void FrontendApi::obs_frontend_set_current_scene_collection(const char *)
{
	NOT_IMPLEMENTED();
}

bool FrontendApi::obs_frontend_add_scene_collection(const char *)
{
	NOT_IMPLEMENTED();
	return false;
}

void FrontendApi::obs_frontend_get_profiles(std::vector<std::string> &strings)
{
	strings.push_back(core_.profileName());
}

char *FrontendApi::obs_frontend_get_current_profile(void)
{
	return dup(core_.profileName());
}

char *FrontendApi::obs_frontend_get_current_profile_path(void)
{
	return dup(core_.profilePath());
}

void FrontendApi::obs_frontend_set_current_profile(const char *)
{
	NOT_IMPLEMENTED();
}

void FrontendApi::obs_frontend_create_profile(const char *)
{
	NOT_IMPLEMENTED();
}

void FrontendApi::obs_frontend_duplicate_profile(const char *)
{
	NOT_IMPLEMENTED();
}

void FrontendApi::obs_frontend_delete_profile(const char *)
{
	NOT_IMPLEMENTED();
}

// ---------------------------------------------------------------- outputs

void FrontendApi::obs_frontend_streaming_start(void)
{
	NOT_IMPLEMENTED();
}

void FrontendApi::obs_frontend_streaming_stop(void)
{
	NOT_IMPLEMENTED();
}

bool FrontendApi::obs_frontend_streaming_active(void)
{
	return false;
}

void FrontendApi::obs_frontend_recording_start(void)
{
	NOT_IMPLEMENTED();
}

void FrontendApi::obs_frontend_recording_stop(void)
{
	NOT_IMPLEMENTED();
}

bool FrontendApi::obs_frontend_recording_active(void)
{
	return false;
}

void FrontendApi::obs_frontend_recording_pause(bool)
{
	NOT_IMPLEMENTED();
}

bool FrontendApi::obs_frontend_recording_paused(void)
{
	return false;
}

bool FrontendApi::obs_frontend_recording_split_file(void)
{
	NOT_IMPLEMENTED();
	return false;
}

bool FrontendApi::obs_frontend_recording_add_chapter(const char *)
{
	NOT_IMPLEMENTED();
	return false;
}

void FrontendApi::obs_frontend_replay_buffer_start(void)
{
	NOT_IMPLEMENTED();
}

void FrontendApi::obs_frontend_replay_buffer_save(void)
{
	NOT_IMPLEMENTED();
}

void FrontendApi::obs_frontend_replay_buffer_stop(void)
{
	NOT_IMPLEMENTED();
}

bool FrontendApi::obs_frontend_replay_buffer_active(void)
{
	return false;
}

obs_output_t *FrontendApi::obs_frontend_get_streaming_output(void)
{
	return nullptr;
}

obs_output_t *FrontendApi::obs_frontend_get_recording_output(void)
{
	return nullptr;
}

obs_output_t *FrontendApi::obs_frontend_get_replay_buffer_output(void)
{
	return nullptr;
}

obs_output_t *FrontendApi::obs_frontend_get_virtualcam_output(void)
{
	return nullptr;
}

void FrontendApi::obs_frontend_start_virtualcam(void)
{
	NOT_IMPLEMENTED();
}

void FrontendApi::obs_frontend_stop_virtualcam(void)
{
	NOT_IMPLEMENTED();
}

bool FrontendApi::obs_frontend_virtualcam_active(void)
{
	return false;
}

// ---------------------------------------------------------------- menus and docks

void *FrontendApi::obs_frontend_add_tools_menu_qaction(const char *name)
{
	if (refuseDuringLoad(__func__)) {
		return nullptr;
	}
	return window_.toolsMenu()->addAction(QString::fromUtf8(name));
}

void FrontendApi::obs_frontend_add_tools_menu_item(const char *name, obs_frontend_cb callback, void *private_data)
{
	if (refuseDuringLoad(__func__)) {
		return;
	}
	QAction *action = window_.toolsMenu()->addAction(QString::fromUtf8(name));
	QObject::connect(action, &QAction::triggered, action, [callback, private_data]() { callback(private_data); });
}

bool FrontendApi::obs_frontend_add_dock_by_id(const char *id, const char *title, void *widget)
{
	if (refuseDuringLoad(__func__)) {
		return false;
	}
	return window_.addPluginDock(QString::fromUtf8(id), QString::fromUtf8(title), static_cast<QWidget *>(widget));
}

void FrontendApi::obs_frontend_remove_dock(const char *id)
{
	if (refuseDuringLoad(__func__)) {
		return;
	}
	window_.removePluginDock(QString::fromUtf8(id));
}

bool FrontendApi::obs_frontend_add_custom_qdock(const char *id, void *dock)
{
	if (refuseDuringLoad(__func__)) {
		return false;
	}
	return window_.addCustomQDock(QString::fromUtf8(id), static_cast<QDockWidget *>(dock));
}

// ---------------------------------------------------------------- callbacks

void FrontendApi::obs_frontend_add_event_callback(obs_frontend_event_cb callback, void *private_data)
{
	apply_or_queue([this, callback, private_data]() {
		QMutexLocker lock(&mutex_);
		Callback<obs_frontend_event_cb> cb{callback, private_data};
		if (std::find(eventCallbacks_.begin(), eventCallbacks_.end(), cb) == eventCallbacks_.end()) {
			eventCallbacks_.push_back(cb);
		}
	});
}

void FrontendApi::obs_frontend_remove_event_callback(obs_frontend_event_cb callback, void *private_data)
{
	apply_or_queue([this, callback, private_data]() {
		QMutexLocker lock(&mutex_);
		std::erase(eventCallbacks_, Callback<obs_frontend_event_cb>{callback, private_data});
	});
}

void FrontendApi::obs_frontend_add_save_callback(obs_frontend_save_cb callback, void *private_data)
{
	apply_or_queue([this, callback, private_data]() {
		QMutexLocker lock(&mutex_);
		Callback<obs_frontend_save_cb> cb{callback, private_data};
		if (std::find(saveCallbacks_.begin(), saveCallbacks_.end(), cb) == saveCallbacks_.end()) {
			saveCallbacks_.push_back(cb);
		}
	});
}

void FrontendApi::obs_frontend_remove_save_callback(obs_frontend_save_cb callback, void *private_data)
{
	apply_or_queue([this, callback, private_data]() {
		QMutexLocker lock(&mutex_);
		std::erase(saveCallbacks_, Callback<obs_frontend_save_cb>{callback, private_data});
	});
}

void FrontendApi::obs_frontend_add_preload_callback(obs_frontend_save_cb callback, void *private_data)
{
	apply_or_queue([this, callback, private_data]() {
		QMutexLocker lock(&mutex_);
		Callback<obs_frontend_save_cb> cb{callback, private_data};
		if (std::find(preloadCallbacks_.begin(), preloadCallbacks_.end(), cb) == preloadCallbacks_.end()) {
			preloadCallbacks_.push_back(cb);
		}
	});
}

void FrontendApi::obs_frontend_remove_preload_callback(obs_frontend_save_cb callback, void *private_data)
{
	apply_or_queue([this, callback, private_data]() {
		QMutexLocker lock(&mutex_);
		std::erase(preloadCallbacks_, Callback<obs_frontend_save_cb>{callback, private_data});
	});
}

void FrontendApi::on_load(obs_data_t *settings)
{
	std::vector<Callback<obs_frontend_save_cb>> callbacks;
	{
		QMutexLocker lock(&mutex_);
		callbacks = saveCallbacks_;
	}
	for (auto it = callbacks.rbegin(); it != callbacks.rend(); ++it) {
		it->callback(settings, false, it->data);
	}
}

void FrontendApi::on_preload(obs_data_t *settings)
{
	std::vector<Callback<obs_frontend_save_cb>> callbacks;
	{
		QMutexLocker lock(&mutex_);
		callbacks = preloadCallbacks_;
	}
	for (auto it = callbacks.rbegin(); it != callbacks.rend(); ++it) {
		it->callback(settings, false, it->data);
	}
}

void FrontendApi::on_save(obs_data_t *settings)
{
	std::vector<Callback<obs_frontend_save_cb>> callbacks;
	{
		QMutexLocker lock(&mutex_);
		callbacks = saveCallbacks_;
	}
	for (auto it = callbacks.rbegin(); it != callbacks.rend(); ++it) {
		it->callback(settings, true, it->data);
	}
}

void FrontendApi::on_event(enum obs_frontend_event event)
{
	std::vector<Callback<obs_frontend_event_cb>> callbacks;
	{
		QMutexLocker lock(&mutex_);
		callbacks = eventCallbacks_;
	}
	// Newest first, as OBS Studio does.
	for (auto it = callbacks.rbegin(); it != callbacks.rend(); ++it) {
		it->callback(event, it->data);
	}
}

// ---------------------------------------------------------------- config and saving

config_t *FrontendApi::obs_frontend_get_profile_config(void)
{
	return core_.profileConfig();
}

config_t *FrontendApi::obs_frontend_get_app_config(void)
{
	return core_.appConfig();
}

config_t *FrontendApi::obs_frontend_get_user_config(void)
{
	return core_.userConfig();
}

void FrontendApi::obs_frontend_open_projector(const char *, int, const char *, const char *)
{
	NOT_IMPLEMENTED();
}

void FrontendApi::obs_frontend_save(void)
{
	if (deferSave_ > 0) {
		return;
	}
	core_.save();
}

void FrontendApi::obs_frontend_defer_save_begin(void)
{
	++deferSave_;
}

void FrontendApi::obs_frontend_defer_save_end(void)
{
	if (deferSave_ > 0 && --deferSave_ == 0) {
		core_.save();
	}
}

void FrontendApi::obs_frontend_push_ui_translation(obs_frontend_translate_ui_cb translate)
{
	QMutexLocker lock(&mutex_);
	translators_.push_back(translate);
}

void FrontendApi::obs_frontend_pop_ui_translation(void)
{
	QMutexLocker lock(&mutex_);
	if (!translators_.empty()) {
		translators_.pop_back();
	}
}

// ---------------------------------------------------------------- service

obs_service_t *FrontendApi::obs_frontend_get_streaming_service(void)
{
	return nullptr;
}

void FrontendApi::obs_frontend_set_streaming_service(obs_service_t *)
{
	NOT_IMPLEMENTED();
}

void FrontendApi::obs_frontend_save_streaming_service()
{
	NOT_IMPLEMENTED();
}

// ---------------------------------------------------------------- studio mode and preview

bool FrontendApi::obs_frontend_preview_program_mode_active(void)
{
	return core_.studioMode();
}

void FrontendApi::obs_frontend_set_preview_program_mode(bool enable)
{
	if (refuseDuringLoad(__func__)) {
		return;
	}
	core_.setStudioMode(enable);
}

void FrontendApi::obs_frontend_preview_program_trigger_transition(void)
{
	if (refuseDuringLoad(__func__)) {
		return;
	}
	core_.transitionToProgram();
}

bool FrontendApi::obs_frontend_preview_enabled(void)
{
	return true;
}

void FrontendApi::obs_frontend_set_preview_enabled(bool)
{
	NOT_IMPLEMENTED();
}

obs_source_t *FrontendApi::obs_frontend_get_current_preview_scene(void)
{
	return core_.studioMode() ? obs_source_get_ref(core_.previewScene()) : nullptr;
}

void FrontendApi::obs_frontend_set_current_preview_scene(obs_source_t *scene)
{
	if (refuseDuringLoad(__func__)) {
		return;
	}
	if (core_.studioMode()) {
		core_.setPreviewScene(scene);
	}
}

// ---------------------------------------------------------------- misc

void FrontendApi::obs_frontend_take_screenshot()
{
	NOT_IMPLEMENTED();
}

void FrontendApi::obs_frontend_take_source_screenshot(obs_source_t *)
{
	NOT_IMPLEMENTED();
}

void FrontendApi::obs_frontend_reset_video(void)
{
	if (refuseDuringLoad(__func__)) {
		return;
	}
	core_.resetVideo();
}

void FrontendApi::obs_frontend_open_source_properties(obs_source_t *)
{
	NOT_IMPLEMENTED();
}

void FrontendApi::obs_frontend_open_source_filters(obs_source_t *)
{
	NOT_IMPLEMENTED();
}

void FrontendApi::obs_frontend_open_source_interaction(obs_source_t *)
{
	NOT_IMPLEMENTED();
}

void FrontendApi::obs_frontend_open_sceneitem_edit_transform(obs_sceneitem_t *)
{
	NOT_IMPLEMENTED();
}

char *FrontendApi::obs_frontend_get_current_record_output_path(void)
{
	NOT_IMPLEMENTED();
	return nullptr;
}

const char *FrontendApi::obs_frontend_get_locale_string(const char *string)
{
	// No frontend translations yet; return the key so callers show something.
	return string;
}

bool FrontendApi::obs_frontend_is_theme_dark(void)
{
	const QColor window = QApplication::palette().color(QPalette::Window);
	return window.lightness() < 128;
}

char *FrontendApi::obs_frontend_get_last_recording(void)
{
	return nullptr;
}

char *FrontendApi::obs_frontend_get_last_screenshot(void)
{
	return nullptr;
}

char *FrontendApi::obs_frontend_get_last_replay(void)
{
	return nullptr;
}

void FrontendApi::obs_frontend_add_undo_redo_action(const char *, const undo_redo_cb, const undo_redo_cb, const char *,
						    const char *, bool)
{
	NOT_IMPLEMENTED();
}

obs_canvas_t *FrontendApi::obs_frontend_add_canvas(const char *, obs_video_info *, int)
{
	NOT_IMPLEMENTED();
	return nullptr;
}

bool FrontendApi::obs_frontend_remove_canvas(obs_canvas_t *)
{
	NOT_IMPLEMENTED();
	return false;
}

void FrontendApi::obs_frontend_get_canvases(obs_frontend_canvas_list *)
{
	NOT_IMPLEMENTED();
}

void FrontendApi::obs_frontend_copy_sceneitem(obs_sceneitem_t *)
{
	NOT_IMPLEMENTED();
}

bool FrontendApi::obs_frontend_can_paste_sceneitem(bool)
{
	return false;
}

void FrontendApi::obs_frontend_paste_sceneitem(obs_scene_t *, bool)
{
	NOT_IMPLEMENTED();
}

bool FrontendApi::obs_frontend_is_safe_mode_enabled(void)
{
	return safeMode_;
}

} // namespace knurbs::api
