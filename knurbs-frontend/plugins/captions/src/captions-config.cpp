#include "captions-config.hpp"

#include <obs.hpp>

namespace knurbs::captions {

CaptionsConfig config_from_data(obs_data_t *collection,
				const std::function<std::string(const char *name)> &uuid_for_name)
{
	CaptionsConfig config;
	if (!collection) {
		return config;
	}

	OBSDataAutoRelease own = obs_data_get_obj(collection, kConfigKey);
	if (own) {
		obs_data_set_default_bool(own, "stream", true);
		obs_data_set_default_bool(own, "recording", false);
		config.sourceUuid = obs_data_get_string(own, "source");
		config.stream = obs_data_get_bool(own, "stream");
		config.recording = obs_data_get_bool(own, "recording");
		return config;
	}

	OBSDataAutoRelease legacy = obs_data_get_obj(collection, kLegacyConfigKey);
	if (legacy) {
		// decklink-captions only ever sent captions to the stream.
		const char *name = obs_data_get_string(legacy, "source");
		if (name && *name && uuid_for_name) {
			config.sourceUuid = uuid_for_name(name);
		}
	}
	return config;
}

void config_to_data(const CaptionsConfig &config, obs_data_t *collection)
{
	if (!collection) {
		return;
	}
	OBSDataAutoRelease own = obs_data_create();
	obs_data_set_string(own, "source", config.sourceUuid.c_str());
	obs_data_set_bool(own, "stream", config.stream);
	obs_data_set_bool(own, "recording", config.recording);
	obs_data_set_obj(collection, kConfigKey, own);
}

} // namespace knurbs::captions
