#include <obs-module.h>
#include <util/platform.h>
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#if !defined(_WIN32) && !defined(__APPLE__)
#include "vaapi-utils.h"

#define LIBAVUTIL_VAAPI_AVAILABLE
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-ffmpeg", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "FFmpeg based sources/outputs/encoders";
}

extern struct obs_source_info ffmpeg_source;
extern struct obs_output_info ffmpeg_output;
extern struct obs_output_info ffmpeg_muxer;
extern struct obs_output_info ffmpeg_mpegts_muxer;
extern struct obs_output_info replay_buffer;
extern struct obs_output_info ffmpeg_hls_muxer;
extern struct obs_encoder_info aac_encoder_info;
extern struct obs_encoder_info opus_encoder_info;
extern struct obs_encoder_info pcm_encoder_info;
extern struct obs_encoder_info pcm24_encoder_info;
extern struct obs_encoder_info pcm32_encoder_info;
extern struct obs_encoder_info alac_encoder_info;
extern struct obs_encoder_info flac_encoder_info;
extern struct obs_encoder_info openh264_encoder_info;
extern struct obs_encoder_info svt_av1_encoder_info;
extern struct obs_encoder_info aom_av1_encoder_info;

#ifdef LIBAVUTIL_VAAPI_AVAILABLE
extern struct obs_encoder_info h264_vaapi_encoder_info;
extern struct obs_encoder_info h264_vaapi_encoder_tex_info;
extern struct obs_encoder_info av1_vaapi_encoder_info;
extern struct obs_encoder_info av1_vaapi_encoder_tex_info;
#ifdef ENABLE_HEVC
extern struct obs_encoder_info hevc_vaapi_encoder_info;
extern struct obs_encoder_info hevc_vaapi_encoder_tex_info;
#endif
#endif

#ifdef LIBAVUTIL_VAAPI_AVAILABLE
static bool h264_vaapi_supported(void)
{
	const AVCodec *vaenc = avcodec_find_encoder_by_name("h264_vaapi");

	if (!vaenc)
		return false;

	/* NOTE: If default device is NULL, it means there is no device
	 * that support H264. */
	return vaapi_get_h264_default_device() != NULL;
}

static bool av1_vaapi_supported(void)
{
	const AVCodec *vaenc = avcodec_find_encoder_by_name("av1_vaapi");

	if (!vaenc)
		return false;

	/* NOTE: If default device is NULL, it means there is no device
	 * that support AV1. */
	return vaapi_get_av1_default_device() != NULL;
}

#ifdef ENABLE_HEVC
static bool hevc_vaapi_supported(void)
{
	const AVCodec *vaenc = avcodec_find_encoder_by_name("hevc_vaapi");

	if (!vaenc)
		return false;

	/* NOTE: If default device is NULL, it means there is no device
	 * that support HEVC. */
	return vaapi_get_hevc_default_device() != NULL;
}
#endif
#endif

#if defined(_WIN32) && !defined(_M_ARM64)
extern void amf_load(void);
extern void amf_unload(void);
#endif

#if ENABLE_FFMPEG_LOGGING
extern void obs_ffmpeg_load_logging(void);
extern void obs_ffmpeg_unload_logging(void);
#endif

static void register_encoder_if_available(struct obs_encoder_info *info, const char *id)
{
	const AVCodec *c = avcodec_find_encoder_by_name(id);
	if (c) {
		obs_register_encoder(info);
	}
}

bool obs_module_load(void)
{
	obs_register_source(&ffmpeg_source);
	obs_register_output(&ffmpeg_output);
	obs_register_output(&ffmpeg_muxer);
	obs_register_output(&ffmpeg_mpegts_muxer);
	obs_register_output(&ffmpeg_hls_muxer);
	obs_register_output(&replay_buffer);
	obs_register_encoder(&aac_encoder_info);
	register_encoder_if_available(&openh264_encoder_info, "libopenh264");
	register_encoder_if_available(&svt_av1_encoder_info, "libsvtav1");
	register_encoder_if_available(&aom_av1_encoder_info, "libaom-av1");
	obs_register_encoder(&opus_encoder_info);
	obs_register_encoder(&pcm_encoder_info);
	obs_register_encoder(&pcm24_encoder_info);
	obs_register_encoder(&pcm32_encoder_info);
	obs_register_encoder(&alac_encoder_info);
	obs_register_encoder(&flac_encoder_info);

#if defined(_WIN32) && !defined(_M_ARM64)
	amf_load();
#endif

#ifdef LIBAVUTIL_VAAPI_AVAILABLE
	const char *libva_env = getenv("LIBVA_DRIVER_NAME");
	if (!!libva_env)
		blog(LOG_WARNING, "LIBVA_DRIVER_NAME variable is set,"
				  " this could prevent FFmpeg VAAPI from working correctly");

	if (h264_vaapi_supported()) {
		blog(LOG_INFO, "FFmpeg VAAPI H264 encoding supported");
		obs_register_encoder(&h264_vaapi_encoder_info);
		obs_register_encoder(&h264_vaapi_encoder_tex_info);
	} else {
		blog(LOG_INFO, "FFmpeg VAAPI H264 encoding not supported");
	}

	if (av1_vaapi_supported()) {
		blog(LOG_INFO, "FFmpeg VAAPI AV1 encoding supported");
		obs_register_encoder(&av1_vaapi_encoder_info);
		obs_register_encoder(&av1_vaapi_encoder_tex_info);
	} else {
		blog(LOG_INFO, "FFmpeg VAAPI AV1 encoding not supported");
	}

#ifdef ENABLE_HEVC
	if (hevc_vaapi_supported()) {
		blog(LOG_INFO, "FFmpeg VAAPI HEVC encoding supported");
		obs_register_encoder(&hevc_vaapi_encoder_info);
		obs_register_encoder(&hevc_vaapi_encoder_tex_info);
	} else {
		blog(LOG_INFO, "FFmpeg VAAPI HEVC encoding not supported");
	}
#endif
#endif

#if ENABLE_FFMPEG_LOGGING
	obs_ffmpeg_load_logging();
#endif
	return true;
}

void obs_module_unload(void)
{
#if ENABLE_FFMPEG_LOGGING
	obs_ffmpeg_unload_logging();
#endif

#if defined(_WIN32) && !defined(_M_ARM64)
	amf_unload();
#endif
}
