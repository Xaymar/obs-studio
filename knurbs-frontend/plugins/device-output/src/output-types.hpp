#pragma once

#include <obs-data.h>

#include <functional>
#include <string_view>
#include <vector>

namespace knurbs::device_output {

// The jobs a device output can do. Each one is a row in the dock.
enum class Slot {
	// Follows the program (the main canvas).
	Program,
	// Follows the preview scene in studio mode, otherwise the program scene.
	Preview,
	// Audio only, for example an ASIO interface.
	Audio,
};

const char *slot_key(Slot slot);

// How frames reach the output.
enum class Feed {
	// The output takes video and audio straight from libobs and converts
	// them itself (obs_output_set_video_conversion).
	Direct,
	// The plugin renders, scales and reads back the picture itself and
	// hands the output its own video_t. Needed for the preview, and for
	// outputs that want premultiplied alpha removed or HDR tone mapped.
	Rendered,
	// Audio only. The output reads libobs audio itself.
	AudioOnly,
};

// What the plugin knows about an output type it can drive.
struct OutputType {
	// The libobs output id.
	const char *id;
	// Locale key for the name shown in the dock.
	const char *label;
	// Where the type can be used, and how its frames are fed per slot.
	bool program;
	bool preview;
	bool audio;
	Feed program_feed;
	// True if obs_output_update applies settings to a running output.
	// Otherwise settings apply on the next start.
	bool live_update;
	// For live_update types: true if going from old to new settings needs
	// a restart anyway. May be null (never needs one).
	bool (*needs_restart)(obs_data_t *old_settings, obs_data_t *new_settings);
};

// All types the plugin knows, in display order.
const std::vector<OutputType> &known_types();

const OutputType *find_type(std::string_view id);

Feed feed_for(const OutputType &type, Slot slot);

bool supports(const OutputType &type, Slot slot);

// The known types usable in a slot whose output id is registered.
// `registered` is normally a check against libobs; tests pass their own.
std::vector<const OutputType *> types_for(Slot slot, const std::function<bool(const char *id)> &registered);

} // namespace knurbs::device_output
