#pragma once

#include "output-types.hpp"

#include <obs.hpp>

#include <string>

namespace knurbs::device_output {

// What the dock remembers for one slot.
struct SlotConfig {
	// Output type id, empty for none.
	std::string type;
	bool autoStart = false;
	// The output's own settings, as its properties edit them. Never null.
	OBSData settings;
};

struct Config {
	SlotConfig program;
	SlotConfig preview;
	SlotConfig audio;

	SlotConfig &at(Slot slot);
	const SlotConfig &at(Slot slot) const;
};

// File name of the settings, stored in the profile's folder so each profile
// has its own device outputs.
inline constexpr const char *kConfigFileName = "com.xaymar.knurbs.device-output.json";

// Reads a config from its saved form. Missing or malformed parts read as
// defaults; an unknown output type reads as none.
Config config_from_data(obs_data_t *data);

// The saved form of a config. Keys:
//   { "version": 1,
//     "program": { "type": "...", "auto-start": false, "settings": {...} },
//     "preview": { ... }, "audio": { ... } }
OBSData config_to_data(const Config &config);

// Loads from, and saves to, a JSON file. A missing file loads as defaults.
Config load_config(const std::string &path);
bool save_config(const Config &config, const std::string &path);

// An independent copy of `data`.
OBSData copy_data(obs_data_t *data);

} // namespace knurbs::device_output
