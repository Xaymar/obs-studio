#pragma once

#include <obs-data.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Encoder IDs. One public ID per codec: the encoder picks texture or system
 * memory input itself and reroutes to the internal ".raw" ID when it can't
 * use textures. */
#define NVENC_ID_H264 "com.xaymar.knurbs.nvenc.h264"
#define NVENC_ID_HEVC "com.xaymar.knurbs.nvenc.hevc"
#define NVENC_ID_AV1 "com.xaymar.knurbs.nvenc.av1"
#define NVENC_ID_H264_RAW NVENC_ID_H264 ".raw"
#define NVENC_ID_HEVC_RAW NVENC_ID_HEVC ".raw"
#define NVENC_ID_AV1_RAW NVENC_ID_AV1 ".raw"

enum nvenc_legacy_codec {
	NVENC_LEGACY_H264,
	NVENC_LEGACY_HEVC,
	NVENC_LEGACY_AV1,
};

/* Converts settings saved by the pre-31.0 encoders (jim_*nvenc,
 * obs_nvenc_*_cuda, ffmpeg_*nvenc) to the current encoder's settings.
 *
 * Old settings only hold the values the user changed, so every key the old
 * encoder had a different default for is first filled in with that old
 * default, which keeps the encoder configured exactly as before.
 *
 * num_devices is the number of NVENC capable GPUs; the old "gpu" index only
 * carries over as "device" when there is more than one. */
void nvenc_migrate_legacy_settings(obs_data_t *settings, enum nvenc_legacy_codec codec, int num_devices);

#ifdef __cplusplus
}
#endif
