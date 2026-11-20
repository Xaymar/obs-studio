#include <obs-module.h>

#include "amf-ids.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("knurbs-amf", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "AMD AMF hardware encoders";
}

extern void amf_load(void);
extern void amf_unload(void);

/* Aliases from the old OBS encoder IDs. The settings didn't change, and the
 * texture and system memory variants both map to the one public ID per
 * codec. */
static void register_encoder_aliases(void)
{
	obs_register_encoder_alias("h264_texture_amf", AMF_ID_H264, NULL, NULL);
	obs_register_encoder_alias("h264_fallback_amf", AMF_ID_H264, NULL, NULL);
#if ENABLE_HEVC
	obs_register_encoder_alias("h265_texture_amf", AMF_ID_HEVC, NULL, NULL);
	obs_register_encoder_alias("h265_fallback_amf", AMF_ID_HEVC, NULL, NULL);
#endif
	obs_register_encoder_alias("av1_texture_amf", AMF_ID_AV1, NULL, NULL);
	obs_register_encoder_alias("av1_fallback_amf", AMF_ID_AV1, NULL, NULL);
}

bool obs_module_load(void)
{
	amf_load();
	register_encoder_aliases();
	return true;
}

void obs_module_unload(void)
{
	amf_unload();
}
