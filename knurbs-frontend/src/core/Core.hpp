#pragma once

#include <obs.hpp>
#include <util/config-file.h>

#include <QObject>
#include <QString>

#include <string>
#include <vector>

namespace knurbs {

class Paths;

// Owns the libobs lifecycle and the state plugins and the UI share: configs,
// scenes, transitions, studio mode. Has no widgets, so it can also run
// headless. Commands are plain calls; changes are announced through signals.
class Core : public QObject {
	Q_OBJECT

public:
	explicit Core(const Paths &paths, QObject *parent = nullptr);
	~Core() override;

	// Loads app and user config. Returns false if a file cannot be opened.
	bool loadConfig();

	// obs_startup with the Wayland display on Linux. Graphics stay unset
	// until resetVideo.
	bool startup(const std::string &locale);

	// Resets video on libobs-vulkan. Returns an OBS_VIDEO_* code.
	int resetVideo();
	bool resetAudio();

	// Loads the current scene collection. For now this creates one empty
	// scene and a fade transition when nothing is saved yet.
	void loadSceneCollection();
	void save();

	void shutdown();

	config_t *appConfig() const { return appConfig_; }
	config_t *userConfig() const { return userConfig_; }
	config_t *profileConfig() const { return profileConfig_; }

	std::string profileName() const { return profileName_; }
	std::string profilePath() const;
	std::string sceneCollectionName() const { return collectionName_; }

	std::vector<obs_source_t *> scenes() const;
	obs_source_t *currentScene() const { return programScene_; }
	void setCurrentScene(obs_source_t *scene);

	std::vector<obs_source_t *> transitions() const;
	obs_source_t *currentTransition() const { return transition_; }
	void setCurrentTransition(obs_source_t *transition);
	int transitionDuration() const { return transitionDurationMs_; }
	void setTransitionDuration(int ms) { transitionDurationMs_ = ms; }

	bool studioMode() const { return studioMode_; }
	void setStudioMode(bool enable);
	obs_source_t *previewScene() const { return previewScene_; }
	void setPreviewScene(obs_source_t *scene);
	void transitionToProgram();

signals:
	void sceneChanged();
	void previewSceneChanged();
	void transitionChanged();
	void studioModeChanged(bool enabled);

private:
	const Paths &paths_;
	bool started_ = false;

	config_t *appConfig_ = nullptr;
	config_t *userConfig_ = nullptr;
	config_t *profileConfig_ = nullptr;
	std::string profileName_ = "Untitled";
	std::string collectionName_ = "Untitled";

	std::vector<OBSSource> scenes_;
	std::vector<OBSSource> transitions_;
	OBSSource programScene_;
	OBSSource previewScene_;
	OBSSource transition_;
	int transitionDurationMs_ = 300;
	bool studioMode_ = false;
};

} // namespace knurbs
