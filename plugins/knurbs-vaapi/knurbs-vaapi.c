#include <stdlib.h>

#include <obs-module.h>
#include <libavcodec/avcodec.h>

#include "vaapi-ids.h"
#include "vaapi-utils.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("knurbs-vaapi", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "VA-API hardware encoders";
}

extern struct obs_encoder_info h264_vaapi_encoder_info;
extern struct obs_encoder_info h264_vaapi_encoder_tex_info;
extern struct obs_encoder_info av1_vaapi_encoder_info;
extern struct obs_encoder_info av1_vaapi_encoder_tex_info;
#ifdef ENABLE_HEVC
extern struct obs_encoder_info hevc_vaapi_encoder_info;
extern struct obs_encoder_info hevc_vaapi_encoder_tex_info;
#endif

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

/* Aliases from the old OBS encoder IDs. The settings didn't change, and the
 * texture and system memory variants both map to the one public ID per
 * codec. */
static void register_encoder_aliases(void)
{
	obs_register_encoder_alias("ffmpeg_vaapi_tex", VAAPI_ID_H264, NULL, NULL);
	obs_register_encoder_alias("ffmpeg_vaapi", VAAPI_ID_H264, NULL, NULL);
	obs_register_encoder_alias("av1_ffmpeg_vaapi_tex", VAAPI_ID_AV1, NULL, NULL);
	obs_register_encoder_alias("av1_ffmpeg_vaapi", VAAPI_ID_AV1, NULL, NULL);
#ifdef ENABLE_HEVC
	obs_register_encoder_alias("hevc_ffmpeg_vaapi_tex", VAAPI_ID_HEVC, NULL, NULL);
	obs_register_encoder_alias("hevc_ffmpeg_vaapi", VAAPI_ID_HEVC, NULL, NULL);
#endif
}

bool obs_module_load(void)
{
	const char *libva_env = getenv("LIBVA_DRIVER_NAME");
	if (!!libva_env)
		blog(LOG_WARNING, "LIBVA_DRIVER_NAME variable is set,"
				  " this could prevent VA-API from working correctly");

	if (h264_vaapi_supported()) {
		blog(LOG_INFO, "VA-API H264 encoding supported");
		obs_register_encoder(&h264_vaapi_encoder_info);
		obs_register_encoder(&h264_vaapi_encoder_tex_info);
	} else {
		blog(LOG_INFO, "VA-API H264 encoding not supported");
	}

	if (av1_vaapi_supported()) {
		blog(LOG_INFO, "VA-API AV1 encoding supported");
		obs_register_encoder(&av1_vaapi_encoder_info);
		obs_register_encoder(&av1_vaapi_encoder_tex_info);
	} else {
		blog(LOG_INFO, "VA-API AV1 encoding not supported");
	}

#ifdef ENABLE_HEVC
	if (hevc_vaapi_supported()) {
		blog(LOG_INFO, "VA-API HEVC encoding supported");
		obs_register_encoder(&hevc_vaapi_encoder_info);
		obs_register_encoder(&hevc_vaapi_encoder_tex_info);
	} else {
		blog(LOG_INFO, "VA-API HEVC encoding not supported");
	}
#endif

	register_encoder_aliases();
	return true;
}
