#pragma once

#include <obs.h>

#include <functional>
#include <string>

namespace knurbs::captions {

// Saved with each scene collection, since the source belongs to one.
struct CaptionsConfig {
	// UUID of the source whose CEA-708 captions are passed on; empty for
	// none. A UUID, not a name, so renaming the source keeps the link.
	std::string sourceUuid;
	bool stream = true;
	bool recording = false;
};

// Key in the scene collection data.
inline constexpr const char *kConfigKey = "com.xaymar.knurbs.captions";
// Key decklink-captions used, read once to migrate.
inline constexpr const char *kLegacyConfigKey = "decklink_captions";

// Reads the config from scene collection data. Without a config of its own,
// reads decklink-captions' (which stored the source by name) and turns the
// name into a UUID with `uuid_for_name` (empty if there is no such source).
CaptionsConfig config_from_data(obs_data_t *collection,
				const std::function<std::string(const char *name)> &uuid_for_name);

// Writes the config into scene collection data under kConfigKey.
void config_to_data(const CaptionsConfig &config, obs_data_t *collection);

} // namespace knurbs::captions
