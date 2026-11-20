#include "captions.hpp"

#include <obs-frontend-api.h>

namespace knurbs::captions {

Captions::Captions() = default;

Captions::~Captions()
{
	detach();
}

void Captions::apply(const CaptionsConfig &config)
{
	if (config.sourceUuid != config_.sourceUuid || !source_) {
		detachSource();
		attachSource(config.sourceUuid);
	}
	config_ = config;
	router_.setEnabled(Target::Stream, config.stream);
	router_.setEnabled(Target::Recording, config.recording);
}

void Captions::refreshOutputs()
{
	OBSOutputAutoRelease stream = obs_frontend_get_streaming_output();
	OBSOutputAutoRelease recording = obs_frontend_get_recording_output();
	router_.setSink(Target::Stream, output_sink(stream));
	router_.setSink(Target::Recording, output_sink(recording));
}

void Captions::detach()
{
	detachSource();
	router_.setSink(Target::Stream, {});
	router_.setSink(Target::Recording, {});
}

void Captions::attachSource(const std::string &uuid)
{
	if (uuid.empty()) {
		return;
	}
	OBSSourceAutoRelease source = obs_get_source_by_uuid(uuid.c_str());
	if (!source) {
		blog(LOG_INFO, "[knurbs-captions] Caption source %s is not in this scene collection", uuid.c_str());
		return;
	}
	if (!(obs_source_get_output_flags(source) & OBS_SOURCE_CEA_708)) {
		blog(LOG_WARNING, "[knurbs-captions] '%s' does not carry captions", obs_source_get_name(source));
		return;
	}
	obs_source_add_caption_callback(source, sourceCaptions, this);
	source_ = OBSGetWeakRef(source);
}

void Captions::detachSource()
{
	OBSSourceAutoRelease source = obs_weak_source_get_source(source_);
	if (source) {
		obs_source_remove_caption_callback(source, sourceCaptions, this);
	}
	source_ = nullptr;
}

void Captions::sourceCaptions(void *param, obs_source_t *, const obs_source_cea_708 *captions)
{
	if (captions) {
		static_cast<Captions *>(param)->router_.deliver(*captions);
	}
}

void Captions::submitTextProc(void *data, calldata_t *cd)
{
	auto *self = static_cast<Captions *>(data);
	const char *text = calldata_string(cd, "text");
	const double duration = calldata_float(cd, "duration");

	bool delivered = false;
	if (self && text && duration > 0.0) {
		delivered = self->router_.deliverText(text, duration);
	}
	calldata_set_bool(cd, "delivered", delivered);
}

} // namespace knurbs::captions
