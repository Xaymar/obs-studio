#include "device-output.hpp"
#include "frame-copy.hpp"

#include <graphics/vec4.h>
#include <media-io/video-frame.h>
#include <media-io/video-io.h>
#include <util/platform.h>

namespace knurbs::device_output {

namespace {

const char *output_name(Slot slot)
{
	switch (slot) {
	case Slot::Program:
		return "com.xaymar.knurbs.device-output.program";
	case Slot::Preview:
		return "com.xaymar.knurbs.device-output.preview";
	case Slot::Audio:
		return "com.xaymar.knurbs.device-output.audio";
	}
	return "com.xaymar.knurbs.device-output";
}

} // namespace

DeviceOutput::DeviceOutput(Slot slot) : slot_(slot) {}

DeviceOutput::~DeviceOutput()
{
	stop();
}

bool DeviceOutput::start(const OutputType &type, obs_data_t *settings)
{
	stop();

	if (!supports(type, slot_)) {
		blog(LOG_ERROR, "[knurbs-device-output] '%s' cannot be used as the %s output", type.id,
		     slot_key(slot_));
		return false;
	}

	output_ = obs_output_create(type.id, output_name(slot_), settings, nullptr);
	if (!output_) {
		blog(LOG_WARNING, "[knurbs-device-output] Could not create a '%s' output", type.id);
		return false;
	}
	type_ = &type;
	generation_.fetch_add(1);

	if (feed_for(type, slot_) == Feed::Rendered && !startRendered()) {
		obs_output_release(output_);
		output_ = nullptr;
		type_ = nullptr;
		return false;
	}

	signal_handler_connect(obs_output_get_signal_handler(output_), "stop", outputStopped, this);

	if (!obs_output_start(output_)) {
		const char *error = obs_output_get_last_error(output_);
		blog(LOG_WARNING, "[knurbs-device-output] The %s output did not start%s%s", slot_key(slot_),
		     error ? ": " : "", error ? error : "");
		stop();
		return false;
	}

	if (stateCallback_) {
		stateCallback_(true, generation_.load());
	}
	return true;
}

void DeviceOutput::stop()
{
	if (!output_) {
		return;
	}

	// Our own stop is not a surprise; only report it once, below.
	signal_handler_disconnect(obs_output_get_signal_handler(output_), "stop", outputStopped, this);

	if (video_) {
		// Removing the callback waits for a render in progress to finish.
		obs_remove_main_rendered_callback(renderCallback, this);
	}

	obs_output_stop(output_);
	// Destroying the output waits for it to finish stopping, so the video
	// queue it reads from can be closed safely afterwards.
	obs_output_release(output_);
	output_ = nullptr;
	type_ = nullptr;

	stopRendered();

	if (stateCallback_) {
		stateCallback_(false, generation_.load());
	}
}

bool DeviceOutput::apply(obs_data_t *old_settings, obs_data_t *new_settings)
{
	if (!output_ || !type_ || !type_->live_update) {
		return false;
	}

	if (type_->needs_restart && type_->needs_restart(old_settings, new_settings)) {
		const OutputType &type = *type_;
		stop();
		return start(type, new_settings);
	}

	obs_output_update(output_, new_settings);
	return true;
}

void DeviceOutput::handleFrontendEvent(obs_frontend_event event)
{
	if (slot_ != Slot::Preview || !video_) {
		return;
	}

	switch (event) {
	case OBS_FRONTEND_EVENT_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_PREVIEW_SCENE_CHANGED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED:
	case OBS_FRONTEND_EVENT_STUDIO_MODE_DISABLED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		followScene();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP:
		// The old collection's scenes are about to go away. Let go of
		// ours so it is not kept alive (and rendered) past its collection.
		setFollowedSource(nullptr);
		break;
	default:
		break;
	}
}

void DeviceOutput::followScene()
{
	obs_source_t *scene = obs_frontend_preview_program_mode_active() ? obs_frontend_get_current_preview_scene()
									  : obs_frontend_get_current_scene();
	setFollowedSource(scene);
	obs_source_release(scene);
}

void DeviceOutput::setFollowedSource(obs_source_t *source)
{
	obs_source_t *previous;
	{
		std::lock_guard lock(sourceMutex_);
		previous = source_;
		source_ = obs_source_get_ref(source);
	}
	// Released outside the lock: the last release can destroy the source.
	obs_source_release(previous);
}

bool DeviceOutput::startRendered()
{
	obs_get_video_info(&ovi_);

	// Outputs set the size and format they want in create(); without one,
	// send the canvas size.
	const video_scale_info *conversion = obs_output_get_video_conversion(output_);
	width_ = (conversion && conversion->width) ? conversion->width : ovi_.output_width;
	height_ = (conversion && conversion->height) ? conversion->height : ovi_.output_height;
	if (!width_ || !height_) {
		blog(LOG_WARNING, "[knurbs-device-output] No video size for the %s output", slot_key(slot_));
		return false;
	}

	obs_enter_graphics();
	sceneTexrender_ = slot_ == Slot::Preview ? gs_texrender_create(GS_BGRA, GS_ZS_NONE) : nullptr;
	scaleTexrender_ = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	bool ok = scaleTexrender_ && (slot_ != Slot::Preview || sceneTexrender_);
	for (gs_stagesurf_t *&stage : stages_) {
		stage = gs_stagesurface_create(width_, height_, GS_BGRA);
		ok = ok && stage;
	}
	obs_leave_graphics();
	staged_.fill(false);
	stageIndex_ = 0;

	video_output_info vi = {};
	vi.name = output_name(slot_);
	vi.format = VIDEO_FORMAT_BGRA;
	vi.width = width_;
	vi.height = height_;
	vi.fps_num = ovi_.fps_num;
	vi.fps_den = ovi_.fps_den;
	vi.cache_size = 16;
	vi.colorspace = VIDEO_CS_DEFAULT;
	vi.range = VIDEO_RANGE_FULL;

	if (!ok || video_output_open(&video_, &vi) != VIDEO_OUTPUT_SUCCESS) {
		blog(LOG_WARNING, "[knurbs-device-output] Could not set up rendering for the %s output",
		     slot_key(slot_));
		video_ = nullptr;
		stopRendered();
		return false;
	}

	if (slot_ == Slot::Preview) {
		followScene();
	}

	obs_output_set_media(output_, video_, obs_get_audio());
	obs_add_main_rendered_callback(renderCallback, this);
	return true;
}

void DeviceOutput::stopRendered()
{
	setFollowedSource(nullptr);

	obs_enter_graphics();
	for (gs_stagesurf_t *&stage : stages_) {
		gs_stagesurface_destroy(stage);
		stage = nullptr;
	}
	gs_texrender_destroy(scaleTexrender_);
	scaleTexrender_ = nullptr;
	gs_texrender_destroy(sceneTexrender_);
	sceneTexrender_ = nullptr;
	obs_leave_graphics();

	if (video_) {
		video_output_close(video_);
		video_ = nullptr;
	}
}

void DeviceOutput::renderCallback(void *param)
{
	static_cast<DeviceOutput *>(param)->render();
}

gs_texture_t *DeviceOutput::renderFollowedSource()
{
	obs_source_t *source;
	{
		std::lock_guard lock(sourceMutex_);
		source = obs_source_get_ref(source_);
	}
	if (!source) {
		return nullptr;
	}

	gs_texture_t *texture = nullptr;
	const uint32_t cx = obs_source_get_base_width(source);
	const uint32_t cy = obs_source_get_base_height(source);

	gs_texrender_reset(sceneTexrender_);
	if (cx && cy && gs_texrender_begin(sceneTexrender_, cx, cy)) {
		vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		obs_source_video_render(source);
		gs_blend_state_pop();

		gs_texrender_end(sceneTexrender_);
		texture = gs_texrender_get_texture(sceneTexrender_);
	}

	obs_source_release(source);
	return texture;
}

void DeviceOutput::render()
{
	gs_texture_t *texture = slot_ == Slot::Preview ? renderFollowedSource() : obs_get_main_texture();
	if (!texture) {
		return;
	}

	const uint32_t tex_cx = gs_texture_get_width(texture);
	const uint32_t tex_cy = gs_texture_get_height(texture);

	// Scale to the output's size, remove premultiplied alpha and, for SDR
	// outputs of an HDR canvas, tone map.
	gs_texrender_reset(scaleTexrender_);
	if (!gs_texrender_begin(scaleTexrender_, width_, height_)) {
		return;
	}

	const video_scale_info *conversion = obs_output_get_video_conversion(output_);
	const bool source_hdr = ovi_.colorspace == VIDEO_CS_2100_PQ || ovi_.colorspace == VIDEO_CS_2100_HLG;
	const bool target_hdr = source_hdr && conversion && conversion->colorspace == VIDEO_CS_2100_PQ;

	// The texrender only sets the viewport; the projection must cover the
	// source texture so the sprite is scaled to the output size.
	gs_ortho(0.0f, (float)tex_cx, 0.0f, (float)tex_cy, -100.0f, 100.0f);

	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(!target_hdr);
	gs_enable_blending(false);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), texture);
	const char *technique = target_hdr ? "DrawAlphaDivideR10L"
					   : (source_hdr ? "DrawAlphaDivideTonemap" : "DrawAlphaDivide");
	while (gs_effect_loop(effect, technique)) {
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"),
				    obs_get_video_sdr_white_level() / 10000.f);
		gs_draw_sprite(texture, 0, tex_cx, tex_cy);
	}

	gs_enable_blending(true);
	gs_enable_framebuffer_srgb(previous_srgb);
	gs_texrender_end(scaleTexrender_);

	// Stage this frame and read back the one staged kStageCount - 1 frames
	// ago, so the GPU never has to finish before the read.
	const size_t write_index = stageIndex_;
	gs_stage_texture(stages_[write_index], gs_texrender_get_texture(scaleTexrender_));
	staged_[write_index] = true;

	const size_t read_index = (write_index + 1) % kStageCount;
	stageIndex_ = read_index;
	if (!staged_[read_index]) {
		return;
	}

	video_frame frame;
	// Stamp the frame with the libobs video clock, the clock its audio is
	// on, so the device gets picture and sound in sync.
	if (!video_output_lock_frame(video_, &frame, 1, obs_get_video_frame_time())) {
		return;
	}

	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	if (gs_stagesurface_map(stages_[read_index], &data, &linesize)) {
		copy_rows(frame.data[0], frame.linesize[0], data, linesize, width_ * 4, height_);
		gs_stagesurface_unmap(stages_[read_index]);
	}
	video_output_unlock_frame(video_);
}

void DeviceOutput::outputStopped(void *param, calldata_t *)
{
	// Runs on the output's thread. The owner moves it to the UI thread and
	// calls stop() there to free everything.
	auto *self = static_cast<DeviceOutput *>(param);
	if (self->stateCallback_) {
		self->stateCallback_(false, self->generation_.load());
	}
}

} // namespace knurbs::device_output
