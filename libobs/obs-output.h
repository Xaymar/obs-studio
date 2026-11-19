/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* obs_output_info.flags definitions */
#define OBS_OUTPUT_VIDEO (1 << 0)
#define OBS_OUTPUT_AUDIO (1 << 1)
#define OBS_OUTPUT_AV (OBS_OUTPUT_VIDEO | OBS_OUTPUT_AUDIO)
#define OBS_OUTPUT_ENCODED (1 << 2)
#define OBS_OUTPUT_SERVICE (1 << 3)
#define OBS_OUTPUT_MULTI_TRACK (1 << 4)
#define OBS_OUTPUT_CAN_PAUSE (1 << 5)
#define OBS_OUTPUT_MULTI_TRACK_AUDIO OBS_OUTPUT_MULTI_TRACK
#define OBS_OUTPUT_MULTI_TRACK_VIDEO (1 << 6)
#define OBS_OUTPUT_MULTI_TRACK_AV (OBS_OUTPUT_MULTI_TRACK_AUDIO | OBS_OUTPUT_MULTI_TRACK_VIDEO)

#define MAX_OUTPUT_AUDIO_ENCODERS 6
#define MAX_OUTPUT_VIDEO_ENCODERS 10

struct encoder_packet;

struct obs_output_info {
	/* required */
	const char *id;

	uint32_t flags;

	const char *(*get_name)(void *type_data);

	void *(*create)(obs_data_t *settings, obs_output_t *output);
	void (*destroy)(void *data);

	bool (*start)(void *data);
	void (*stop)(void *data, uint64_t ts);

	void (*raw_video)(void *data, struct video_data *frame);
	void (*raw_audio)(void *data, struct audio_data *frames);

	void (*encoded_packet)(void *data, struct encoder_packet *packet);

	/* optional */
	void (*update)(void *data, obs_data_t *settings);

	void (*get_defaults)(obs_data_t *settings);

	obs_properties_t *(*get_properties)(void *data);

	void (*unused1)(void *data);

	uint64_t (*get_total_bytes)(void *data);

	int (*get_dropped_frames)(void *data);

	void *type_data;
	void (*free_type_data)(void *type_data);

	float (*get_congestion)(void *data);
	int (*get_connect_time_ms)(void *data);

	/* only used with encoded outputs, separated with semicolon */
	const char *encoded_video_codecs;
	const char *encoded_audio_codecs;

	/* raw audio callback for multi track outputs */
	void (*raw_audio2)(void *data, size_t idx, struct audio_data *frames);

	/* required if OBS_OUTPUT_SERVICE */
	const char *protocols;

	/* raw audio callback for mixed monitoring outputs with low latency */
	void (*raw_audio_monitoring)(void *data, struct audio_data *frames);
};

EXPORT void obs_register_output_s(const struct obs_output_info *info, size_t size);

#define obs_register_output(info) obs_register_output_s(info, sizeof(struct obs_output_info))

/**
 * Called when an output is created through an alias, before the new output
 * type sees the settings. Rename or convert keys here so the new output type
 * understands settings that were saved for the old one.
 *
 * @param  param     The param given to obs_register_output_alias
 * @param  old_id    The ID the output was requested with
 * @param  settings  A private copy of the settings, safe to modify
 */
typedef void (*obs_output_alias_migrate_t)(void *param, const char *old_id, obs_data_t *settings);

/**
 * Registers an alias, so that outputs requested with an old output ID are
 * created as another, registered output type instead. Queries by ID
 * (display name, flags, codecs, defaults, properties) follow aliases too.
 *
 * Aliases are resolved when used, never at registration, so the target may be
 * registered by another module and in any order. Output types registered
 * under old_id always win over the alias. An alias may point at another alias.
 *
 * The created output reports the new ID through obs_output_get_id.
 *
 * @param  old_id   ID to replace
 * @param  new_id   ID of the replacement
 * @param  migrate  Optional settings migration, run once per created output
 * @param  param    Passed to migrate
 * @return          false if the arguments are invalid, old_id is already an
 *                  alias, or an output type is already registered as old_id
 */
EXPORT bool obs_register_output_alias(const char *old_id, const char *new_id, obs_output_alias_migrate_t migrate,
				      void *param);

#ifdef __cplusplus
}
#endif
