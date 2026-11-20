#include <obs-module.h>
#include <util/platform.h>
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

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
}
