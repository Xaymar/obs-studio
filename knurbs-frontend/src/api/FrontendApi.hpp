#pragma once

#include <obs-frontend-internal.hpp>

#include <QMutex>

#include <string>
#include <vector>

namespace knurbs {
class Core;
class MainWindow;
} // namespace knurbs

namespace knurbs::api {

// Implements obs-frontend-api on top of Core and the Shell, so existing
// plugins keep working through the same exported C functions.
//
// While modules load in parallel, only registration calls (event, save and
// preload callbacks) are accepted from worker threads. They are queued and
// applied in module order at the barrier, so the result is the same on every
// run. Calls that need the UI are refused during load and logged with the
// module's name.
class FrontendApi final : public obs_frontend_callbacks {
public:
	FrontendApi(Core &core, MainWindow &window, bool safeMode);
	~FrontendApi() override;

	// Installs a new instance as the obs-frontend-api backend.
	static FrontendApi *install(Core &core, MainWindow &window, bool safeMode);
	static FrontendApi *instance();

	// Module load phase bookkeeping, used by ModuleLoader.
	static void beginModuleLoad();
	static void endModuleLoad();
	static void setLoadingModule(int index, const std::string &name);

	void emitEvent(obs_frontend_event event) { on_event(event); }

	// obs_frontend_callbacks
	void *obs_frontend_get_main_window(void) override;
	void *obs_frontend_get_main_window_handle(void) override;
	void *obs_frontend_get_system_tray(void) override;

	void obs_frontend_get_scenes(struct obs_frontend_source_list *sources) override;
	obs_source_t *obs_frontend_get_current_scene(void) override;
	void obs_frontend_set_current_scene(obs_source_t *scene) override;

	void obs_frontend_get_transitions(struct obs_frontend_source_list *sources) override;
	obs_source_t *obs_frontend_get_current_transition(void) override;
	void obs_frontend_set_current_transition(obs_source_t *transition) override;
	int obs_frontend_get_transition_duration(void) override;
	void obs_frontend_set_transition_duration(int duration) override;
	void obs_frontend_release_tbar(void) override;
	int obs_frontend_get_tbar_position(void) override;
	void obs_frontend_set_tbar_position(int position) override;

	void obs_frontend_get_scene_collections(std::vector<std::string> &strings) override;
	char *obs_frontend_get_current_scene_collection(void) override;
	void obs_frontend_set_current_scene_collection(const char *collection) override;
	bool obs_frontend_add_scene_collection(const char *name) override;

	void obs_frontend_get_profiles(std::vector<std::string> &strings) override;
	char *obs_frontend_get_current_profile(void) override;
	char *obs_frontend_get_current_profile_path(void) override;
	void obs_frontend_set_current_profile(const char *profile) override;
	void obs_frontend_create_profile(const char *name) override;
	void obs_frontend_duplicate_profile(const char *name) override;
	void obs_frontend_delete_profile(const char *profile) override;

	void obs_frontend_streaming_start(void) override;
	void obs_frontend_streaming_stop(void) override;
	bool obs_frontend_streaming_active(void) override;

	void obs_frontend_recording_start(void) override;
	void obs_frontend_recording_stop(void) override;
	bool obs_frontend_recording_active(void) override;
	void obs_frontend_recording_pause(bool pause) override;
	bool obs_frontend_recording_paused(void) override;
	bool obs_frontend_recording_split_file(void) override;
	bool obs_frontend_recording_add_chapter(const char *name) override;

	void obs_frontend_replay_buffer_start(void) override;
	void obs_frontend_replay_buffer_save(void) override;
	void obs_frontend_replay_buffer_stop(void) override;
	bool obs_frontend_replay_buffer_active(void) override;

	void *obs_frontend_add_tools_menu_qaction(const char *name) override;
	void obs_frontend_add_tools_menu_item(const char *name, obs_frontend_cb callback, void *private_data) override;

	bool obs_frontend_add_dock_by_id(const char *id, const char *title, void *widget) override;
	void obs_frontend_remove_dock(const char *id) override;
	bool obs_frontend_add_custom_qdock(const char *id, void *dock) override;

	void obs_frontend_add_event_callback(obs_frontend_event_cb callback, void *private_data) override;
	void obs_frontend_remove_event_callback(obs_frontend_event_cb callback, void *private_data) override;

	obs_output_t *obs_frontend_get_streaming_output(void) override;
	obs_output_t *obs_frontend_get_recording_output(void) override;
	obs_output_t *obs_frontend_get_replay_buffer_output(void) override;

	config_t *obs_frontend_get_profile_config(void) override;
	config_t *obs_frontend_get_app_config(void) override;
	config_t *obs_frontend_get_user_config(void) override;

	void obs_frontend_open_projector(const char *type, int monitor, const char *geometry,
					 const char *name) override;
	void obs_frontend_save(void) override;
	void obs_frontend_defer_save_begin(void) override;
	void obs_frontend_defer_save_end(void) override;
	void obs_frontend_add_save_callback(obs_frontend_save_cb callback, void *private_data) override;
	void obs_frontend_remove_save_callback(obs_frontend_save_cb callback, void *private_data) override;
	void obs_frontend_add_preload_callback(obs_frontend_save_cb callback, void *private_data) override;
	void obs_frontend_remove_preload_callback(obs_frontend_save_cb callback, void *private_data) override;

	void obs_frontend_push_ui_translation(obs_frontend_translate_ui_cb translate) override;
	void obs_frontend_pop_ui_translation(void) override;

	obs_service_t *obs_frontend_get_streaming_service(void) override;
	void obs_frontend_set_streaming_service(obs_service_t *service) override;
	void obs_frontend_save_streaming_service() override;

	bool obs_frontend_preview_program_mode_active(void) override;
	void obs_frontend_set_preview_program_mode(bool enable) override;
	void obs_frontend_preview_program_trigger_transition(void) override;

	bool obs_frontend_preview_enabled(void) override;
	void obs_frontend_set_preview_enabled(bool enable) override;

	obs_source_t *obs_frontend_get_current_preview_scene(void) override;
	void obs_frontend_set_current_preview_scene(obs_source_t *scene) override;

	void on_load(obs_data_t *settings) override;
	void on_preload(obs_data_t *settings) override;
	void on_save(obs_data_t *settings) override;
	void on_event(enum obs_frontend_event event) override;

	void obs_frontend_take_screenshot() override;
	void obs_frontend_take_source_screenshot(obs_source_t *source) override;

	obs_output_t *obs_frontend_get_virtualcam_output(void) override;
	void obs_frontend_start_virtualcam(void) override;
	void obs_frontend_stop_virtualcam(void) override;
	bool obs_frontend_virtualcam_active(void) override;

	void obs_frontend_reset_video(void) override;

	void obs_frontend_open_source_properties(obs_source_t *source) override;
	void obs_frontend_open_source_filters(obs_source_t *source) override;
	void obs_frontend_open_source_interaction(obs_source_t *source) override;
	void obs_frontend_open_sceneitem_edit_transform(obs_sceneitem_t *item) override;

	char *obs_frontend_get_current_record_output_path(void) override;
	const char *obs_frontend_get_locale_string(const char *string) override;

	bool obs_frontend_is_theme_dark(void) override;

	char *obs_frontend_get_last_recording(void) override;
	char *obs_frontend_get_last_screenshot(void) override;
	char *obs_frontend_get_last_replay(void) override;

	void obs_frontend_add_undo_redo_action(const char *name, const undo_redo_cb undo, const undo_redo_cb redo,
					       const char *undo_data, const char *redo_data, bool repeatable) override;

	obs_canvas_t *obs_frontend_add_canvas(const char *name, obs_video_info *ovi, int flags) override;
	bool obs_frontend_remove_canvas(obs_canvas_t *canvas) override;
	void obs_frontend_get_canvases(obs_frontend_canvas_list *canvas_list) override;

	void obs_frontend_copy_sceneitem(obs_sceneitem_t *item) override;
	bool obs_frontend_can_paste_sceneitem(bool duplicate) override;
	void obs_frontend_paste_sceneitem(obs_scene_t *scene, bool duplicate) override;
	bool obs_frontend_is_safe_mode_enabled(void) override;

private:
	template<typename T> struct Callback {
		T callback;
		void *data;
		bool operator==(const Callback &o) const { return callback == o.callback && data == o.data; }
	};

	// True (and logs) if a call that needs the UI arrives during module load.
	bool refuseDuringLoad(const char *function) const;

	Core &core_;
	MainWindow &window_;
	bool safeMode_;

	mutable QMutex mutex_;
	std::vector<Callback<obs_frontend_event_cb>> eventCallbacks_;
	std::vector<Callback<obs_frontend_save_cb>> saveCallbacks_;
	std::vector<Callback<obs_frontend_save_cb>> preloadCallbacks_;
	std::vector<obs_frontend_translate_ui_cb> translators_;
	int deferSave_ = 0;
};

} // namespace knurbs::api
