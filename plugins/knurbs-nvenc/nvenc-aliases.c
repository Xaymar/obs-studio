#include "nvenc-helpers.h"

#include <stdint.h>

/*
 * Aliases from older encoder IDs to the current encoders.
 *
 * - obs_nvenc_*_tex and obs_nvenc_*_soft (31.0 and later) use the current
 *   settings, so they only need a new ID. The texture and system memory
 *   variants both map to the one public ID per codec, which picks its input
 *   path itself.
 * - jim_*nvenc, obs_nvenc_*_cuda and ffmpeg_*nvenc (before 31.0) also get
 *   their settings converted, see nvenc_migrate_legacy_settings.
 */

static void migrate_legacy(void *param, const char *old_id, obs_data_t *settings)
{
	UNUSED_PARAMETER(old_id);
	nvenc_migrate_legacy_settings(settings, (enum nvenc_legacy_codec)(intptr_t)param, num_encoder_devices());
}

#define LEGACY(codec) migrate_legacy, (void *)(intptr_t)(codec)

void register_encoder_aliases(void)
{
	obs_register_encoder_alias("obs_nvenc_h264_tex", NVENC_ID_H264, NULL, NULL);
	obs_register_encoder_alias("obs_nvenc_h264_soft", NVENC_ID_H264, NULL, NULL);
	obs_register_encoder_alias("jim_nvenc", NVENC_ID_H264, LEGACY(NVENC_LEGACY_H264));
	obs_register_encoder_alias("obs_nvenc_h264_cuda", NVENC_ID_H264, LEGACY(NVENC_LEGACY_H264));
	obs_register_encoder_alias("ffmpeg_nvenc", NVENC_ID_H264, LEGACY(NVENC_LEGACY_H264));

#ifdef ENABLE_HEVC
	obs_register_encoder_alias("obs_nvenc_hevc_tex", NVENC_ID_HEVC, NULL, NULL);
	obs_register_encoder_alias("obs_nvenc_hevc_soft", NVENC_ID_HEVC, NULL, NULL);
	obs_register_encoder_alias("jim_hevc_nvenc", NVENC_ID_HEVC, LEGACY(NVENC_LEGACY_HEVC));
	obs_register_encoder_alias("obs_nvenc_hevc_cuda", NVENC_ID_HEVC, LEGACY(NVENC_LEGACY_HEVC));
	obs_register_encoder_alias("ffmpeg_hevc_nvenc", NVENC_ID_HEVC, LEGACY(NVENC_LEGACY_HEVC));
#endif

	obs_register_encoder_alias("obs_nvenc_av1_tex", NVENC_ID_AV1, NULL, NULL);
	obs_register_encoder_alias("obs_nvenc_av1_soft", NVENC_ID_AV1, NULL, NULL);
	obs_register_encoder_alias("jim_av1_nvenc", NVENC_ID_AV1, LEGACY(NVENC_LEGACY_AV1));
	obs_register_encoder_alias("obs_nvenc_av1_cuda", NVENC_ID_AV1, LEGACY(NVENC_LEGACY_AV1));
}
