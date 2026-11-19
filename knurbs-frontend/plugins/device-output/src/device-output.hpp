#pragma once

#include "output-types.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>

namespace knurbs::device_output {

// One running device output: one slot in the dock.
//
// All methods except the render callback run on the UI thread. The render
// callback runs on the graphics thread and only reads the followed scene,
// which is guarded by a mutex.
class DeviceOutput {
public:
	explicit DeviceOutput(Slot slot);
	~DeviceOutput();

	DeviceOutput(const DeviceOutput &) = delete;
	DeviceOutput &operator=(const DeviceOutput &) = delete;

	Slot slot() const { return slot_; }

	// Creates and starts an output of `type` with `settings`. Returns false
	// and leaves nothing behind if the output cannot be created or started.
	bool start(const OutputType &type, obs_data_t *settings);

	// Stops the output and frees everything start() made. Safe to call
	// when stopped.
	void stop();

	bool running() const { return output_ != nullptr; }

	// Applies changed settings. Types that update live get
	// obs_output_update, or a restart when the type says the change needs
	// one. Other types keep their settings until the next start.
	// Returns true if the change reached the running output.
	bool apply(obs_data_t *old_settings, obs_data_t *new_settings);

	// Called with true when the output starts and false when it stops.
	// start() and stop() call it on their own thread. When the output stops
	// by itself (device unplugged, error) it is called from the output's
	// thread; the owner then calls stop() on the UI thread to free
	// everything, after checking that `generation` is still current.
	void setStateCallback(std::function<void(bool running, uint64_t generation)> callback)
	{
		stateCallback_ = std::move(callback);
	}

	// Counts starts, so a late report about an earlier run can be told apart.
	uint64_t generation() const { return generation_.load(); }

	// Frontend events that change which scene the preview slot follows.
	void handleFrontendEvent(obs_frontend_event event);

private:
	static constexpr size_t kStageCount = 3;

	bool startRendered();
	void stopRendered();
	void followScene();
	void setFollowedSource(obs_source_t *source);

	static void renderCallback(void *param);
	void render();
	gs_texture_t *renderFollowedSource();

	static void outputStopped(void *param, calldata_t *data);

	Slot slot_;
	const OutputType *type_ = nullptr;
	obs_output_t *output_ = nullptr;
	std::function<void(bool, uint64_t)> stateCallback_;
	std::atomic<uint64_t> generation_{0};

	// Rendered feed. Created on the UI thread under obs_enter_graphics, used
	// only by the render callback while it is registered.
	video_t *video_ = nullptr;
	obs_video_info ovi_{};
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	gs_texrender_t *sceneTexrender_ = nullptr;
	gs_texrender_t *scaleTexrender_ = nullptr;
	std::array<gs_stagesurf_t *, kStageCount> stages_{};
	std::array<bool, kStageCount> staged_{};
	size_t stageIndex_ = 0;

	// The scene the preview slot follows. Held as a strong reference so the
	// render callback can render it; replaced on the UI thread.
	std::mutex sourceMutex_;
	obs_source_t *source_ = nullptr;
};

} // namespace knurbs::device_output
