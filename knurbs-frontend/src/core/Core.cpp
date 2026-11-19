#include "Core.hpp"

#include "api/FrontendApi.hpp"
#include "app/Paths.hpp"

#include <QDir>
#include <QGuiApplication>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <obs-nix-platform.h>
#include <QtGui/qguiapplication_platform.h>
#endif

namespace knurbs {

static config_t *open_config(const QString &path)
{
	config_t *config = nullptr;
	if (config_open(&config, path.toUtf8().constData(), CONFIG_OPEN_ALWAYS) != CONFIG_SUCCESS) {
		return nullptr;
	}
	return config;
}

Core::Core(const Paths &paths, QObject *parent) : QObject(parent), paths_(paths) {}

Core::~Core()
{
	shutdown();
}

bool Core::loadConfig()
{
	appConfig_ = open_config(QDir(paths_.roamingConfig()).filePath(QStringLiteral("global.ini")));
	userConfig_ = open_config(QDir(paths_.roamingConfig()).filePath(QStringLiteral("user.ini")));

	const QString profileDir = QDir(paths_.profiles()).filePath(QString::fromStdString(profileName_));
	QDir().mkpath(profileDir);
	profileConfig_ = open_config(QDir(profileDir).filePath(QStringLiteral("basic.ini")));

	if (!appConfig_ || !userConfig_ || !profileConfig_) {
		return false;
	}

	config_set_default_uint(profileConfig_, "Video", "BaseCX", 1920);
	config_set_default_uint(profileConfig_, "Video", "BaseCY", 1080);
	config_set_default_uint(profileConfig_, "Video", "OutputCX", 1920);
	config_set_default_uint(profileConfig_, "Video", "OutputCY", 1080);
	config_set_default_uint(profileConfig_, "Video", "FPSNum", 60);
	config_set_default_uint(profileConfig_, "Video", "FPSDen", 1);
	config_set_default_uint(profileConfig_, "Video", "Adapter", 0);
	config_set_default_uint(profileConfig_, "Audio", "SampleRate", 48000);
	return true;
}

std::string Core::profilePath() const
{
	return QDir(paths_.profiles()).filePath(QString::fromStdString(profileName_)).toStdString();
}

bool Core::startup(const std::string &locale)
{
#if !defined(_WIN32) && !defined(__APPLE__)
	// Linux is Wayland only. Offscreen (headless tests) runs libobs without a
	// display connection; hotkeys then never fire.
	if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
		blog(LOG_WARNING, "[knurbs] Running offscreen, without a display connection");
		obs_set_nix_platform(OBS_NIX_PLATFORM_WAYLAND);
		obs_set_nix_platform_display(nullptr);
	} else {
		if (!QGuiApplication::platformName().contains(QStringLiteral("wayland"))) {
			blog(LOG_ERROR, "[knurbs] Qt platform is '%s'; Knurbs needs Wayland",
			     QGuiApplication::platformName().toUtf8().constData());
			return false;
		}
		auto *wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
		if (!wayland) {
			blog(LOG_ERROR, "[knurbs] Wayland native interface is not available");
			return false;
		}
		obs_set_nix_platform(OBS_NIX_PLATFORM_WAYLAND);
		obs_set_nix_platform_display(wayland->display());
	}
#endif

	if (!obs_startup(locale.c_str(), paths_.pluginConfig().toUtf8().constData(), nullptr)) {
		return false;
	}
	started_ = true;
	return true;
}

int Core::resetVideo()
{
	obs_video_info ovi = {};
	ovi.graphics_module = "libobs-vulkan";
	ovi.fps_num = uint32_t(config_get_uint(profileConfig_, "Video", "FPSNum"));
	ovi.fps_den = uint32_t(config_get_uint(profileConfig_, "Video", "FPSDen"));
	ovi.base_width = uint32_t(config_get_uint(profileConfig_, "Video", "BaseCX"));
	ovi.base_height = uint32_t(config_get_uint(profileConfig_, "Video", "BaseCY"));
	ovi.output_width = uint32_t(config_get_uint(profileConfig_, "Video", "OutputCX"));
	ovi.output_height = uint32_t(config_get_uint(profileConfig_, "Video", "OutputCY"));
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.adapter = uint32_t(config_get_uint(profileConfig_, "Video", "Adapter"));
	ovi.gpu_conversion = true;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;

	return obs_reset_video(&ovi);
}

bool Core::resetAudio()
{
	obs_audio_info2 ai = {};
	ai.samples_per_sec = uint32_t(config_get_uint(profileConfig_, "Audio", "SampleRate"));
	ai.speakers = SPEAKERS_STEREO;
	return obs_reset_audio2(&ai);
}

void Core::loadSceneCollection()
{
	// TODO: read and write scene collection files; until then start with one
	// empty scene so plugins and the UI have something to work with.
	if (scenes_.empty()) {
		OBSSceneAutoRelease scene = obs_scene_create("Scene");
		scenes_.emplace_back(obs_scene_get_source(scene));
	}
	if (transitions_.empty()) {
		OBSSourceAutoRelease fade = obs_source_create_private("fade_transition", "Fade", nullptr);
		if (fade) {
			transitions_.emplace_back(fade.Get());
		}
	}

	if (!transitions_.empty()) {
		setCurrentTransition(transitions_.front());
	}
	setCurrentScene(scenes_.front());
}

void Core::save()
{
	if (auto *api = api::FrontendApi::instance()) {
		OBSDataAutoRelease data = obs_data_create();
		api->on_save(data);
	}
	if (appConfig_) {
		config_save_safe(appConfig_, "tmp", nullptr);
	}
	if (userConfig_) {
		config_save_safe(userConfig_, "tmp", nullptr);
	}
	if (profileConfig_) {
		config_save_safe(profileConfig_, "tmp", nullptr);
	}
}

void Core::shutdown()
{
	if (!started_) {
		return;
	}

	obs_set_output_source(0, nullptr);
	previewScene_ = nullptr;
	programScene_ = nullptr;
	transition_ = nullptr;
	for (OBSSource &scene : scenes_) {
		obs_source_remove(scene);
	}
	scenes_.clear();
	transitions_.clear();

	obs_shutdown();
	started_ = false;

	config_close(profileConfig_);
	config_close(userConfig_);
	config_close(appConfig_);
	profileConfig_ = userConfig_ = appConfig_ = nullptr;
}

std::vector<obs_source_t *> Core::scenes() const
{
	std::vector<obs_source_t *> out;
	for (const OBSSource &s : scenes_) {
		out.push_back(s);
	}
	return out;
}

std::vector<obs_source_t *> Core::transitions() const
{
	std::vector<obs_source_t *> out;
	for (const OBSSource &s : transitions_) {
		out.push_back(s);
	}
	return out;
}

void Core::setCurrentScene(obs_source_t *scene)
{
	if (!scene || scene == programScene_) {
		return;
	}

	programScene_ = scene;
	if (transition_) {
		obs_transition_start(transition_, OBS_TRANSITION_MODE_AUTO, uint32_t(transitionDurationMs_), scene);
	} else {
		obs_set_output_source(0, scene);
	}

	emit sceneChanged();
	if (auto *api = api::FrontendApi::instance()) {
		api->emitEvent(OBS_FRONTEND_EVENT_SCENE_CHANGED);
	}
}

void Core::setCurrentTransition(obs_source_t *transition)
{
	if (!transition || transition == transition_) {
		return;
	}

	if (transition_ && programScene_) {
		obs_transition_set(transition, programScene_);
	}
	transition_ = transition;
	obs_set_output_source(0, transition);

	emit transitionChanged();
	if (auto *api = api::FrontendApi::instance()) {
		api->emitEvent(OBS_FRONTEND_EVENT_TRANSITION_CHANGED);
	}
}

void Core::setStudioMode(bool enable)
{
	if (enable == studioMode_) {
		return;
	}

	studioMode_ = enable;
	if (enable) {
		previewScene_ = programScene_;
	} else {
		previewScene_ = nullptr;
	}

	emit studioModeChanged(enable);
	if (auto *api = api::FrontendApi::instance()) {
		api->emitEvent(enable ? OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED
				      : OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED);
	}
}

void Core::setPreviewScene(obs_source_t *scene)
{
	if (!studioMode_ || !scene || scene == previewScene_) {
		return;
	}

	previewScene_ = scene;
	emit previewSceneChanged();
	if (auto *api = api::FrontendApi::instance()) {
		api->emitEvent(OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED);
	}
}

void Core::transitionToProgram()
{
	if (studioMode_ && previewScene_) {
		setCurrentScene(previewScene_);
	}
}

} // namespace knurbs
