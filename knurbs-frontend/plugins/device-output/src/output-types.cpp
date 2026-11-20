#include "output-types.hpp"

#include <media-io/audio-io.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace knurbs::device_output {

namespace {

// ASIO channel routing is stored per device channel as a bitmask whose upper
// bits name the mix (track). The mix after the last regular one is the
// monitoring mix, which the win-asio output feeds from a different producer.
int asio_track_from_bitmask(int64_t bitmask)
{
	if (bitmask < 0) {
		return -1;
	}
	for (int track = 0; track < MAX_AUDIO_MIXES + 1; track++) {
		for (int channel = 0; channel < MAX_AUDIO_CHANNELS; channel++) {
			if (bitmask == (int64_t)channel + (1LL << (track + 4))) {
				return track;
			}
		}
	}
	return -1;
}

bool asio_is_monitoring_track(int track)
{
	return track == MAX_AUDIO_MIXES;
}

constexpr int kAsioMaxDeviceChannels = 32;

// A device change, or a channel moving onto or off the monitoring mix, needs
// the ASIO output to restart so that only one producer writes to its buffer.
// Everything else applies live through obs_output_update.
bool asio_needs_restart(obs_data_t *old_settings, obs_data_t *new_settings)
{
	if (std::strcmp(obs_data_get_string(old_settings, "device_name"),
			obs_data_get_string(new_settings, "device_name")) != 0) {
		return true;
	}

	for (int i = 0; i < kAsioMaxDeviceChannels; i++) {
		char key[32];
		std::snprintf(key, sizeof(key), "device_ch%d", i);
		const int old_track = asio_track_from_bitmask(obs_data_get_int(old_settings, key));
		const int new_track = asio_track_from_bitmask(obs_data_get_int(new_settings, key));
		if (old_track != new_track &&
		    (asio_is_monitoring_track(old_track) || asio_is_monitoring_track(new_track))) {
			return true;
		}
	}
	return false;
}

} // namespace

const char *slot_key(Slot slot)
{
	switch (slot) {
	case Slot::Program:
		return "program";
	case Slot::Preview:
		return "preview";
	case Slot::Audio:
		return "audio";
	}
	return "program";
}

const std::vector<OutputType> &known_types()
{
	// Output ids are the ones the device plugins register today. When those
	// plugins move to reverse-DNS ids, the old ids become aliases and these
	// entries switch to the new ids.
	static const std::vector<OutputType> types = {
		{
			"decklink_output",
			"Type.DeckLink",
			true,
			true,
			false,
			Feed::Rendered,
			false,
			nullptr,
		},
		{
			"aja_output",
			"Type.AJA",
			true,
			true,
			false,
			Feed::Direct,
			false,
			nullptr,
		},
		{
			"asio_output",
			"Type.ASIO",
			false,
			false,
			true,
			Feed::AudioOnly,
			true,
			asio_needs_restart,
		},
	};
	return types;
}

const OutputType *find_type(std::string_view id)
{
	for (const OutputType &type : known_types()) {
		if (id == type.id) {
			return &type;
		}
	}
	return nullptr;
}

bool supports(const OutputType &type, Slot slot)
{
	switch (slot) {
	case Slot::Program:
		return type.program;
	case Slot::Preview:
		return type.preview;
	case Slot::Audio:
		return type.audio;
	}
	return false;
}

Feed feed_for(const OutputType &type, Slot slot)
{
	switch (slot) {
	case Slot::Program:
		return type.program_feed;
	case Slot::Preview:
		// The preview scene is not what libobs outputs, so it is always
		// rendered by the plugin.
		return Feed::Rendered;
	case Slot::Audio:
		return Feed::AudioOnly;
	}
	return Feed::Direct;
}

std::vector<const OutputType *> types_for(Slot slot, const std::function<bool(const char *id)> &registered)
{
	std::vector<const OutputType *> result;
	for (const OutputType &type : known_types()) {
		if (supports(type, slot) && registered(type.id)) {
			result.push_back(&type);
		}
	}
	return result;
}

} // namespace knurbs::device_output
