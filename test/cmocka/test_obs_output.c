#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-core-test.h"

#include <util/dstr.h>

/* ------------------------------------------------------------------------- */
/* fake encoders: a video encoder that emits a keyframe every 10 frames and an audio encoder */

struct fake_encoder {
	obs_encoder_t *encoder;
	int64_t frames;
	uint8_t packet[16];
	int updates;
};

static long long video_encodes, audio_encodes;

static void *fake_enc_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	UNUSED_PARAMETER(settings);
	struct fake_encoder *fe = bzalloc(sizeof(*fe));
	fe->encoder = encoder;
	return fe;
}

static void fake_enc_destroy(void *data)
{
	bfree(data);
}

static bool video_encoder_fail;

static bool fake_video_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet,
			      bool *received_packet)
{
	struct fake_encoder *fe = data;
	if (video_encoder_fail)
		return false;
	bool key = fe->frames % 10 == 0;
	static const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0x88, 0x84, 0x10};
	static const uint8_t slice[] = {0, 0, 0, 1, 0x41, 0x9a, 0x02, 0x10};
	memcpy(fe->packet, key ? idr : slice, sizeof(idr));
	packet->data = fe->packet;
	packet->size = sizeof(idr);
	packet->pts = frame->pts;
	packet->dts = frame->pts;
	packet->keyframe = key;
	packet->type = OBS_ENCODER_VIDEO;
	*received_packet = true;
	fe->frames++;
	video_encodes++;
	return true;
}

static bool fake_audio_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet,
			      bool *received_packet)
{
	struct fake_encoder *fe = data;
	memset(fe->packet, 0x11, 8);
	packet->data = fe->packet;
	packet->size = 8;
	packet->pts = frame->pts;
	packet->dts = frame->pts;
	packet->type = OBS_ENCODER_AUDIO;
	*received_packet = true;
	fe->frames++;
	audio_encodes++;
	return true;
}

static size_t fake_audio_frame_size(void *data)
{
	UNUSED_PARAMETER(data);
	return 1024;
}

static bool fake_enc_update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	((struct fake_encoder *)data)->updates++;
	return true;
}

static void fake_enc_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "bitrate", 2500);
}

static obs_properties_t *fake_enc_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "bitrate", "Bitrate", 0, 100000, 1);
	return props;
}

static bool fake_enc_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	static uint8_t header[] = {0x01, 0x64, 0x00, 0x1f, 0xff};
	UNUSED_PARAMETER(data);
	*extra_data = header;
	*size = sizeof(header);
	return true;
}

static void fake_enc_video_info(void *data, struct video_scale_info *info)
{
	UNUSED_PARAMETER(data);
	info->format = VIDEO_FORMAT_NV12;
}

static void fake_enc_audio_info(void *data, struct audio_convert_info *info)
{
	UNUSED_PARAMETER(data);
	info->format = AUDIO_FORMAT_FLOAT_PLANAR;
}

/* ------------------------------------------------------------------------- */
/* fake outputs */

struct fake_output {
	obs_output_t *output;
	bool encoded;
	long long raw_video, raw_audio, video_packets, audio_packets, keyframes;
	volatile long long captioned_packets;
	int starts, stops;
	bool fail_start;
};

static void *fake_output_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);
	struct fake_output *fo = bzalloc(sizeof(*fo));
	fo->output = output;
	fo->encoded = (obs_output_get_flags(output) & OBS_OUTPUT_ENCODED) != 0;
	return fo;
}

static void fake_output_destroy(void *data)
{
	bfree(data);
}

static bool fake_output_start(void *data)
{
	struct fake_output *fo = data;
	fo->starts++;
	if (fo->fail_start)
		return false;
	if (!obs_output_can_begin_data_capture(fo->output, 0))
		return false;
	if (fo->encoded && !obs_output_initialize_encoders(fo->output, 0))
		return false;
	return obs_output_begin_data_capture(fo->output, 0);
}

static void fake_output_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(ts);
	struct fake_output *fo = data;
	fo->stops++;
	obs_output_end_data_capture(fo->output);
}

static void fake_raw_video(void *data, struct video_data *frame)
{
	UNUSED_PARAMETER(frame);
	((struct fake_output *)data)->raw_video++;
}

static void fake_raw_audio(void *data, struct audio_data *frames)
{
	UNUSED_PARAMETER(frames);
	((struct fake_output *)data)->raw_audio++;
}

static void fake_encoded_packet(void *data, struct encoder_packet *packet)
{
	struct fake_output *fo = data;
	if (!packet)
		return;
	if (packet->type == OBS_ENCODER_VIDEO) {
		/* the fake encoder's packets are 8 bytes; captions add an SEI NAL */
		if (packet->size > 8)
			fo->captioned_packets++;
		fo->video_packets++;
		if (packet->keyframe)
			fo->keyframes++;
	} else {
		fo->audio_packets++;
	}
}

static uint64_t fake_total_bytes(void *data)
{
	struct fake_output *fo = data;
	return (uint64_t)(fo->video_packets + fo->audio_packets) * 8;
}

/* ------------------------------------------------------------------------- */
/* fake service */

struct fake_service {
	obs_service_t *service;
	char *url;
};

static void *fake_service_create(obs_data_t *settings, obs_service_t *service)
{
	struct fake_service *fs = bzalloc(sizeof(*fs));
	fs->service = service;
	fs->url = bstrdup(obs_data_get_string(settings, "server"));
	return fs;
}

static void fake_service_destroy(void *data)
{
	struct fake_service *fs = data;
	bfree(fs->url);
	bfree(fs);
}

static void fake_service_update(void *data, obs_data_t *settings)
{
	struct fake_service *fs = data;
	bfree(fs->url);
	fs->url = bstrdup(obs_data_get_string(settings, "server"));
}

static const char *fake_service_connect_info(void *data, uint32_t type)
{
	struct fake_service *fs = data;
	switch (type) {
	case OBS_SERVICE_CONNECT_INFO_SERVER_URL:
		return fs->url;
	case OBS_SERVICE_CONNECT_INFO_STREAM_KEY:
		return "key";
	default:
		return NULL;
	}
}

static const char *fake_service_protocol(void *data)
{
	UNUSED_PARAMETER(data);
	return "FAKE";
}

static const char *fake_service_output_type(void *data)
{
	UNUSED_PARAMETER(data);
	return "fake_encoded_output";
}

static bool fake_service_can_connect(void *data)
{
	struct fake_service *fs = data;
	return fs->url && *fs->url;
}

static const char *video_codecs[] = {"h264", NULL};
static const char **fake_service_video_codecs(void *data)
{
	UNUSED_PARAMETER(data);
	return video_codecs;
}

static void fake_service_max_bitrate(void *data, int *video, int *audio)
{
	UNUSED_PARAMETER(data);
	*video = 6000;
	*audio = 320;
}

static void fake_service_apply(void *data, obs_data_t *video, obs_data_t *audio)
{
	UNUSED_PARAMETER(data);
	if (video)
		obs_data_set_int(video, "bitrate", 6000);
	if (audio)
		obs_data_set_int(audio, "bitrate", 160);
}

/* a texture encoder for the GPU encoding path */
static volatile long long texture_encodes;
static bool texture_encoder_fail;

static bool fake_texture_encode(void *data, struct encoder_texture *texture, int64_t pts, uint64_t lock_key,
				uint64_t *next_key, struct encoder_packet *packet, bool *received_packet)
{
	UNUSED_PARAMETER(lock_key);
	UNUSED_PARAMETER(next_key);
	struct fake_encoder *fe = data;
	if (!texture->tex[0] || !texture->tex[1])
		return false;
	if (texture_encoder_fail)
		return false;

	static const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0x88, 0x84, 0x10};
	memcpy(fe->packet, idr, sizeof(idr));
	packet->data = fe->packet;
	packet->size = sizeof(idr);
	packet->pts = pts;
	packet->dts = pts;
	packet->keyframe = true;
	packet->type = OBS_ENCODER_VIDEO;
	*received_packet = true;
	texture_encodes++;
	return true;
}

static void register_output_types(void)
{
	struct obs_encoder_info venc = {
		.id = "fake_video_enc",
		.type = OBS_ENCODER_VIDEO,
		.codec = "h264",
		.get_name = fake_get_name,
		.create = fake_enc_create,
		.destroy = fake_enc_destroy,
		.encode = fake_video_encode,
		.update = fake_enc_update,
		.get_defaults = fake_enc_defaults,
		.get_properties = fake_enc_properties,
		.get_extra_data = fake_enc_extra_data,
		.get_video_info = fake_enc_video_info,
		.caps = OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_ROI,
	};
	obs_register_encoder(&venc);

	struct obs_encoder_info tenc = venc;
	tenc.id = "fake_texture_enc";
	tenc.encode_texture2 = fake_texture_encode;
	tenc.caps = OBS_ENCODER_CAP_PASS_TEXTURE;
	obs_register_encoder(&tenc);

	struct obs_encoder_info aenc = {
		.id = "fake_audio_enc",
		.type = OBS_ENCODER_AUDIO,
		.codec = "aac",
		.get_name = fake_get_name,
		.create = fake_enc_create,
		.destroy = fake_enc_destroy,
		.encode = fake_audio_encode,
		.get_frame_size = fake_audio_frame_size,
		.update = fake_enc_update,
		.get_defaults = fake_enc_defaults,
		.get_audio_info = fake_enc_audio_info,
	};
	obs_register_encoder(&aenc);

	struct obs_output_info raw = {
		.id = "fake_raw_output",
		.flags = OBS_OUTPUT_AV,
		.get_name = fake_get_name,
		.create = fake_output_create,
		.destroy = fake_output_destroy,
		.start = fake_output_start,
		.stop = fake_output_stop,
		.raw_video = fake_raw_video,
		.raw_audio = fake_raw_audio,
	};
	obs_register_output(&raw);

	struct obs_output_info encoded = {
		.id = "fake_encoded_output",
		.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE | OBS_OUTPUT_CAN_PAUSE,
		.get_name = fake_get_name,
		.create = fake_output_create,
		.destroy = fake_output_destroy,
		.start = fake_output_start,
		.stop = fake_output_stop,
		.encoded_packet = fake_encoded_packet,
		.get_total_bytes = fake_total_bytes,
		.encoded_video_codecs = "h264",
		.encoded_audio_codecs = "aac",
		.protocols = "FAKE",
	};
	obs_register_output(&encoded);

	struct obs_service_info service = {
		.id = "fake_service",
		.get_name = fake_get_name,
		.create = fake_service_create,
		.destroy = fake_service_destroy,
		.update = fake_service_update,
		.get_connect_info = fake_service_connect_info,
		.get_protocol = fake_service_protocol,
		.get_output_type = fake_service_output_type,
		.can_try_to_connect = fake_service_can_connect,
		.get_supported_video_codecs = fake_service_video_codecs,
		.get_max_bitrate = fake_service_max_bitrate,
		.apply_encoder_settings = fake_service_apply,
	};
	obs_register_service(&service);
}

static void start(void)
{
	core_start();
	register_output_types();
	video_encodes = audio_encodes = 0;
}

static struct fake_output *fake_output_of(obs_output_t *output)
{
	return obs_obj_get_data(output);
}

static bool wait_for(volatile long long *counter, long long count)
{
	for (int i = 0; i < 400 && *counter < count; i++)
		os_sleep_ms(5);
	return *counter >= count;
}

static bool wait_inactive(obs_output_t *output)
{
	for (int i = 0; i < 400 && obs_output_active(output); i++)
		os_sleep_ms(5);
	return !obs_output_active(output);
}

static int output_signals;
static void on_output_signal(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);
	output_signals++;
}

static void raw_output_test(void **state)
{
	UNUSED_PARAMETER(state);
	start();

	obs_source_t *src = obs_source_create("fake_input", "src", NULL, NULL);
	obs_set_output_source(0, src);

	assert_int_equal(obs_get_output_flags("fake_raw_output"), OBS_OUTPUT_AV);
	assert_string_equal(obs_output_get_display_name("fake_raw_output"), "Fake");
	obs_output_t *out = obs_output_create("fake_raw_output", "raw", NULL, NULL);
	assert_non_null(out);
	assert_string_equal(obs_output_get_name(out), "raw");
	assert_string_equal(obs_output_get_id(out), "fake_raw_output");
	obs_output_t *found = obs_get_output_by_name("raw");
	assert_ptr_equal(found, out);
	obs_output_release(found);
	assert_false(obs_output_can_pause(out));

	signal_handler_t *sh = obs_output_get_signal_handler(out);
	output_signals = 0;
	signal_handler_connect(sh, "start", on_output_signal, NULL);
	signal_handler_connect(sh, "stop", on_output_signal, NULL);
	signal_handler_connect(sh, "activate", on_output_signal, NULL);
	signal_handler_connect(sh, "deactivate", on_output_signal, NULL);

	obs_output_set_media(out, obs_get_video(), obs_get_audio());
	assert_ptr_equal(obs_output_video(out), obs_get_video());
	assert_ptr_equal(obs_output_audio(out), obs_get_audio());
	obs_output_set_mixers(out, 0x3);
	assert_int_equal(obs_output_get_mixers(out), 0x3);
	obs_output_set_mixer(out, 0);
	assert_int_equal(obs_output_get_mixer(out), 0);

	assert_true(obs_output_start(out));
	assert_true(obs_output_active(out));
	assert_true(obs_video_active());
	struct fake_output *fo = fake_output_of(out);
	assert_true(wait_for(&fo->raw_video, 5));
	assert_true(wait_for(&fo->raw_audio, 2));
	assert_true(obs_output_get_total_frames(out) > 0);
	assert_int_equal(obs_output_get_width(out), 640);
	assert_int_equal(obs_output_get_height(out), 360);

	/* video cannot be reset while an output is active */
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	assert_int_equal(obs_reset_video(&ovi), OBS_VIDEO_CURRENTLY_ACTIVE);

	obs_output_stop(out);
	assert_true(wait_inactive(out));
	assert_int_equal(fo->stops, 1);
	assert_true(output_signals >= 4);
	signal_handler_disconnect(sh, "start", on_output_signal, NULL);
	signal_handler_disconnect(sh, "stop", on_output_signal, NULL);
	signal_handler_disconnect(sh, "activate", on_output_signal, NULL);
	signal_handler_disconnect(sh, "deactivate", on_output_signal, NULL);

	/* a failing start reports an error */
	fo->fail_start = true;
	assert_false(obs_output_start(out));
	assert_false(obs_output_active(out));
	obs_output_set_last_error(out, "broken");
	assert_string_equal(obs_output_get_last_error(out), "broken");
	fo->fail_start = false;

	/* preferred size and conversion */
	obs_output_set_preferred_size(out, 320, 180);
	assert_true(obs_output_start(out));
	assert_int_equal(obs_output_get_width(out), 320);
	assert_int_equal(obs_output_get_height(out), 180);
	/* the data capture ends on its own thread */
	obs_output_force_stop(out);
	assert_true(wait_inactive(out));

	obs_output_release(out);
	obs_set_output_source(0, NULL);
	obs_source_release(src);
	core_stop();
}

static int roi_count;
static struct obs_encoder_roi last_roi;
static void count_roi(void *param, struct obs_encoder_roi *roi)
{
	UNUSED_PARAMETER(param);
	roi_count++;
	last_roi = *roi;
}

static void encoder_test(void **state)
{
	UNUSED_PARAMETER(state);
	start();

	assert_string_equal(obs_get_encoder_codec("fake_video_enc"), "h264");
	assert_int_equal(obs_get_encoder_type("fake_audio_enc"), OBS_ENCODER_AUDIO);
	assert_int_equal(obs_get_encoder_caps("fake_video_enc"), OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_ROI);
	assert_null(obs_get_encoder_codec("missing"));
	obs_data_t *defaults = obs_encoder_defaults("fake_video_enc");
	assert_int_equal(obs_data_get_int(defaults, "bitrate"), 2500);
	obs_data_release(defaults);
	obs_properties_t *props = obs_get_encoder_properties("fake_video_enc");
	assert_non_null(obs_properties_get(props, "bitrate"));
	obs_properties_destroy(props);

	obs_encoder_t *venc = obs_video_encoder_create("fake_video_enc", "venc", NULL, NULL);
	obs_encoder_t *aenc = obs_audio_encoder_create("fake_audio_enc", "aenc", NULL, 1, NULL);
	assert_non_null(venc);
	assert_non_null(aenc);
	assert_string_equal(obs_encoder_get_name(venc), "venc");
	obs_encoder_set_name(venc, "video");
	assert_string_equal(obs_encoder_get_name(venc), "video");
	assert_string_equal(obs_encoder_get_codec(venc), "h264");
	assert_string_equal(obs_encoder_get_id(aenc), "fake_audio_enc");
	assert_int_equal(obs_encoder_get_type(venc), OBS_ENCODER_VIDEO);
	assert_int_equal(obs_encoder_get_mixer_index(aenc), 1);
	assert_int_equal(obs_encoder_get_caps(venc), OBS_ENCODER_CAP_DYN_BITRATE | OBS_ENCODER_CAP_ROI);
	obs_encoder_t *found = obs_get_encoder_by_name("video");
	assert_ptr_equal(found, venc);
	obs_encoder_release(found);

	obs_data_t *settings = obs_encoder_get_settings(venc);
	assert_int_equal(obs_data_get_int(settings, "bitrate"), 2500);
	obs_data_release(settings);
	settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", 1000);
	obs_encoder_update(venc, settings);
	obs_data_release(settings);
	props = obs_encoder_properties(venc);
	obs_properties_destroy(props);

	/* the encoder instance, and so its extra data, exists only once an output initializes it */
	uint8_t *extra;
	size_t extra_size;
	assert_false(obs_encoder_get_extra_data(venc, &extra, &extra_size));

	obs_encoder_set_video(venc, obs_get_video());
	obs_encoder_set_audio(aenc, obs_get_audio());
	assert_ptr_equal(obs_encoder_video(venc), obs_get_video());
	assert_ptr_equal(obs_encoder_audio(aenc), obs_get_audio());
	assert_int_equal(obs_encoder_get_width(venc), 640);
	assert_int_equal(obs_encoder_get_sample_rate(aenc), 48000);

	obs_encoder_set_scaled_size(venc, 320, 180);
	assert_true(obs_encoder_scaling_enabled(venc));
	assert_int_equal(obs_encoder_get_width(venc), 320);
	assert_int_equal(obs_encoder_get_height(venc), 180);
	obs_encoder_set_scaled_size(venc, 0, 0);
	obs_encoder_set_gpu_scale_type(venc, OBS_SCALE_LANCZOS);
	assert_int_equal(obs_encoder_get_scale_type(venc), OBS_SCALE_LANCZOS);
	assert_true(obs_encoder_set_frame_rate_divisor(venc, 2));
	assert_int_equal(obs_encoder_get_frame_rate_divisor(venc), 2);
	/* Regression: the divisor output reported the full frame rate and frame time of its root */
	video_t *half = obs_encoder_video(venc);
	assert_ptr_not_equal(half, obs_get_video());
	assert_true(video_output_get_frame_rate(half) * 2.0 == video_output_get_frame_rate(obs_get_video()));
	assert_int_equal(video_output_get_frame_time(half), video_output_get_frame_time(obs_get_video()) * 2);
	assert_int_equal(video_output_get_info(half)->fps_den, video_output_get_info(obs_get_video())->fps_den * 2);
	assert_int_equal(video_output_get_width(half), 640);
	assert_false(obs_encoder_set_frame_rate_divisor(venc, 0));
	assert_true(obs_encoder_set_frame_rate_divisor(venc, 1));
	obs_encoder_set_preferred_video_format(venc, VIDEO_FORMAT_I420);
	assert_int_equal(obs_encoder_get_preferred_video_format(venc), VIDEO_FORMAT_I420);
	obs_encoder_set_preferred_video_format(venc, VIDEO_FORMAT_NONE);
	obs_encoder_set_preferred_color_space(venc, VIDEO_CS_709);
	assert_int_equal(obs_encoder_get_preferred_color_space(venc), VIDEO_CS_709);
	obs_encoder_set_preferred_range(venc, VIDEO_RANGE_FULL);
	assert_int_equal(obs_encoder_get_preferred_range(venc), VIDEO_RANGE_FULL);

	/* regions of interest */
	struct obs_encoder_roi roi = {.top = 0, .bottom = 16, .left = 0, .right = 16, .priority = 0.5f};
	uint32_t increment = obs_encoder_get_roi_increment(venc);
	assert_true(obs_encoder_add_roi(venc, &roi));
	assert_true(obs_encoder_has_roi(venc));
	assert_true(obs_encoder_get_roi_increment(venc) > increment);
	roi_count = 0;
	obs_encoder_enum_roi(venc, count_roi, NULL);
	assert_int_equal(roi_count, 1);
	/* scaled encoders get the region scaled to their size */
	obs_encoder_set_scaled_size(venc, 320, 180);
	obs_encoder_enum_roi(venc, count_roi, NULL);
	assert_int_equal(roi_count, 2);
	assert_int_equal(last_roi.right, 8);
	obs_encoder_set_scaled_size(venc, 0, 0);
	roi.priority = 2.0f;
	assert_false(obs_encoder_add_roi(venc, &roi));
	roi.priority = 0.0f;
	roi.bottom = 8;
	assert_false(obs_encoder_add_roi(venc, &roi));
	/* Regression: an inverted region underflowed the size check and was accepted */
	roi.top = 32;
	roi.bottom = 0;
	assert_false(obs_encoder_add_roi(venc, &roi));
	obs_encoder_clear_roi(venc);
	assert_false(obs_encoder_has_roi(venc));
	/* Regression: the ROI functions dereferenced a NULL encoder */
	assert_false(obs_encoder_add_roi(NULL, &roi));
	assert_false(obs_encoder_has_roi(NULL));
	obs_encoder_clear_roi(NULL);
	obs_encoder_enum_roi(NULL, count_roi, NULL);
	assert_int_equal(obs_encoder_get_roi_increment(NULL), 0);

	assert_false(obs_encoder_active(venc));
	assert_false(obs_encoder_paused(venc));
	/* the frame size comes from the encoder instance too */
	assert_int_equal(obs_encoder_get_frame_size(aenc), 0);

	obs_encoder_release(venc);
	obs_encoder_release(aenc);
	core_stop();
}

static void encoded_output_test(void **state)
{
	UNUSED_PARAMETER(state);
	start();

	obs_source_t *src = obs_source_create("fake_input", "src", NULL, NULL);
	obs_set_output_source(0, src);

	obs_output_t *out = obs_output_create("fake_encoded_output", "enc", NULL, NULL);
	obs_encoder_t *venc = obs_video_encoder_create("fake_video_enc", "venc", NULL, NULL);
	obs_encoder_t *aenc = obs_audio_encoder_create("fake_audio_enc", "aenc", NULL, 0, NULL);
	obs_encoder_set_video(venc, obs_get_video());
	obs_encoder_set_audio(aenc, obs_get_audio());
	obs_output_set_video_encoder(out, venc);
	obs_output_set_audio_encoder(out, aenc, 0);
	assert_ptr_equal(obs_output_get_video_encoder(out), venc);
	assert_ptr_equal(obs_output_get_audio_encoder(out, 0), aenc);
	assert_null(obs_output_get_audio_encoder(out, 1));
	assert_true(obs_output_can_pause(out));
	assert_string_equal(obs_output_get_supported_video_codecs(out), "h264");
	assert_string_equal(obs_output_get_supported_audio_codecs(out), "aac");
	assert_string_equal(obs_get_output_supported_video_codecs("fake_encoded_output"), "h264");

	/* the output needs a service before it starts */
	obs_data_t *ss = obs_data_create();
	obs_data_set_string(ss, "server", "fake://server");
	obs_service_t *service = obs_service_create("fake_service", "svc", ss, NULL);
	obs_data_release(ss);
	obs_output_set_service(out, service);
	assert_ptr_equal(obs_output_get_service(out), service);

	assert_true(obs_output_start(out));
	assert_true(obs_output_active(out));
	assert_true(obs_encoder_active(venc));
	struct fake_output *fo = fake_output_of(out);
	assert_true(wait_for(&fo->video_packets, 15));
	assert_true(wait_for(&fo->audio_packets, 3));
	assert_true(fo->keyframes >= 1);
	assert_true(obs_output_get_total_bytes(out) > 0);
	assert_true(obs_encoder_get_encoded_frames(venc) > 0);
	uint8_t *extra;
	size_t extra_size;
	assert_true(obs_encoder_get_extra_data(venc, &extra, &extra_size));
	assert_int_equal(extra_size, 5);
	assert_false(obs_encoder_get_extra_data(aenc, &extra, &extra_size));
	assert_int_equal(obs_encoder_get_frame_size(aenc), 1024);

	/* encoders cannot change while active */
	obs_encoder_set_scaled_size(venc, 100, 100);
	assert_int_equal(obs_encoder_get_width(venc), 640);

	/* pausing stops the packets */
	assert_true(obs_output_pause(out, true));
	assert_true(obs_output_paused(out));
	assert_true(obs_encoder_paused(venc));
	os_sleep_ms(100);
	assert_true(obs_output_pause(out, false));
	assert_false(obs_output_paused(out));
	long long before = fo->video_packets;
	assert_true(wait_for(&fo->video_packets, before + 5));
	/* encoded outputs pause their encoders, which keep the offset */
	assert_true(obs_encoder_get_pause_offset(venc) > 0);

	obs_output_stop(out);
	assert_true(wait_inactive(out));
	assert_false(obs_encoder_active(venc));

	/* an encode error stops the output */
	assert_true(obs_output_start(out));
	before = fo->video_packets;
	assert_true(wait_for(&fo->video_packets, before + 2));
	video_encoder_fail = true;
	assert_true(wait_inactive(out));
	video_encoder_fail = false;
	assert_false(obs_encoder_active(venc));

	obs_output_release(out);
	obs_encoder_release(venc);
	obs_encoder_release(aenc);
	obs_service_release(service);
	obs_set_output_source(0, NULL);
	obs_source_release(src);
	core_stop();
}

/* NV12 output with two-plane textures: encoders that take textures run on the GPU encode thread */
static void gpu_encode_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_set_env("NULL_GRAPHICS_YUV_TEXTURES", "1");
	start();
	texture_encodes = 0;
	texture_encoder_fail = false;

	obs_source_t *src = obs_source_create("fake_input", "src", NULL, NULL);
	obs_set_output_source(0, src);

	obs_output_t *out = obs_output_create("fake_encoded_output", "enc", NULL, NULL);
	obs_encoder_t *venc = obs_video_encoder_create("fake_texture_enc", "tenc", NULL, NULL);
	obs_encoder_t *aenc = obs_audio_encoder_create("fake_audio_enc", "aenc", NULL, 0, NULL);
	obs_encoder_set_video(venc, obs_get_video());
	obs_encoder_set_audio(aenc, obs_get_audio());
	obs_output_set_video_encoder(out, venc);
	obs_output_set_audio_encoder(out, aenc, 0);
	obs_data_t *ss = obs_data_create();
	obs_data_set_string(ss, "server", "fake://server");
	obs_service_t *service = obs_service_create("fake_service", "svc", ss, NULL);
	obs_data_release(ss);
	obs_output_set_service(out, service);

	assert_true(obs_output_start(out));
	struct fake_output *fo = fake_output_of(out);
	assert_true(wait_for(&fo->video_packets, 10));
	assert_true(texture_encodes >= 10);
	/* the raw video encoder path was not used */
	assert_true(obs_encoder_get_encoded_frames(venc) > 0);

	/* pausing stops the texture encodes too */
	assert_true(obs_output_pause(out, true));
	os_sleep_ms(50);
	long long paused = texture_encodes;
	os_sleep_ms(100);
	assert_true(texture_encodes <= paused + 1);
	assert_true(obs_output_pause(out, false));
	long long before = fo->video_packets;
	assert_true(wait_for(&fo->video_packets, before + 5));

	/* half the frame rate */
	obs_output_stop(out);
	assert_true(wait_inactive(out));
	assert_true(obs_encoder_set_frame_rate_divisor(venc, 2));
	assert_true(obs_output_start(out));
	before = fo->video_packets;
	assert_true(wait_for(&fo->video_packets, before + 5));

	/* a failing encode stops the output with an error */
	texture_encoder_fail = true;
	assert_true(wait_inactive(out));
	texture_encoder_fail = false;

	obs_output_stop(out);
	assert_true(wait_inactive(out));

	obs_output_release(out);
	obs_encoder_release(venc);
	obs_encoder_release(aenc);
	obs_service_release(service);
	obs_set_output_source(0, NULL);
	obs_source_release(src);
	core_stop();
	core_set_env("NULL_GRAPHICS_YUV_TEXTURES", NULL);
}

/* captions are inserted into the H.264 packets as SEI NAL units */
static void captions_test(void **state)
{
	UNUSED_PARAMETER(state);
	start();

	obs_source_t *src = obs_source_create("fake_input", "src", NULL, NULL);
	obs_set_output_source(0, src);
	obs_output_t *out = obs_output_create("fake_encoded_output", "enc", NULL, NULL);
	obs_encoder_t *venc = obs_video_encoder_create("fake_video_enc", "venc", NULL, NULL);
	obs_encoder_t *aenc = obs_audio_encoder_create("fake_audio_enc", "aenc", NULL, 0, NULL);
	obs_encoder_set_video(venc, obs_get_video());
	obs_encoder_set_audio(aenc, obs_get_audio());
	obs_output_set_video_encoder(out, venc);
	obs_output_set_audio_encoder(out, aenc, 0);
	obs_data_t *ss = obs_data_create();
	obs_data_set_string(ss, "server", "fake://server");
	obs_service_t *service = obs_service_create("fake_service", "svc", ss, NULL);
	obs_data_release(ss);
	obs_output_set_service(out, service);

	assert_true(obs_output_start(out));
	struct fake_output *fo = fake_output_of(out);
	assert_true(wait_for(&fo->video_packets, 3));

	/* text, split into lines, with a display duration */
	obs_output_output_caption_text2(out, "Hello captions, this line is long enough to be split in two parts", 0.2);
	assert_true(wait_for(&fo->captioned_packets, 1));
	obs_output_output_caption_text1(out, "short");
	long long before = fo->captioned_packets;
	assert_true(wait_for(&fo->captioned_packets, before + 1));

	/* raw CEA-708 data */
	static const uint8_t cea[] = {0xfc, 0x94, 0x2c, 0xfc, 0x94, 0x2c};
	struct obs_source_cea_708 captions = {.data = cea, .packets = 2, .timestamp = os_gettime_ns()};
	before = fo->captioned_packets;
	obs_output_caption(out, &captions);
	assert_true(wait_for(&fo->captioned_packets, before + 1));

	obs_output_stop(out);
	assert_true(wait_inactive(out));
	obs_output_release(out);
	obs_encoder_release(venc);
	obs_encoder_release(aenc);
	obs_service_release(service);
	obs_set_output_source(0, NULL);
	obs_source_release(src);
	core_stop();
}

static obs_output_t *make_encoded_output(const char *name, obs_encoder_t *venc, obs_encoder_t *aenc,
					 obs_service_t *service)
{
	obs_output_t *out = obs_output_create("fake_encoded_output", name, NULL, NULL);
	obs_output_set_video_encoder(out, venc);
	obs_output_set_audio_encoder(out, aenc, 0);
	obs_output_set_service(out, service);
	return out;
}

/* encoders scaled on the GPU get their own video mix, shared by encoders with the same output; encoders in a
 * group start together */
static void gpu_rescale_group_test(void **state)
{
	UNUSED_PARAMETER(state);
	start();

	obs_source_t *src = obs_source_create("fake_input", "src", NULL, NULL);
	obs_set_output_source(0, src);
	/* a service belongs to one output at a time */
	obs_data_t *ss = obs_data_create();
	obs_data_set_string(ss, "server", "fake://server");
	obs_service_t *services[3];
	for (int i = 0; i < 3; i++)
		services[i] = obs_service_create("fake_service", "svc", ss, NULL);
	obs_data_release(ss);

	obs_encoder_t *v1 = obs_video_encoder_create("fake_video_enc", "v1", NULL, NULL);
	obs_encoder_t *v2 = obs_video_encoder_create("fake_video_enc", "v2", NULL, NULL);
	obs_encoder_t *v3 = obs_video_encoder_create("fake_video_enc", "v3", NULL, NULL);
	obs_encoder_t *a1 = obs_audio_encoder_create("fake_audio_enc", "a1", NULL, 0, NULL);
	obs_encoder_t *a2 = obs_audio_encoder_create("fake_audio_enc", "a2", NULL, 0, NULL);
	obs_encoder_t *a3 = obs_audio_encoder_create("fake_audio_enc", "a3", NULL, 0, NULL);
	obs_encoder_t *venc[] = {v1, v2, v3};
	obs_encoder_t *aenc[] = {a1, a2, a3};
	for (int i = 0; i < 3; i++) {
		obs_encoder_set_video(venc[i], obs_get_video());
		obs_encoder_set_audio(aenc[i], obs_get_audio());
	}

	/* v1 and v2 share a 320x180 mix, v3 gets an I444 one */
	obs_encoder_set_scaled_size(v1, 320, 180);
	obs_encoder_set_gpu_scale_type(v1, OBS_SCALE_BICUBIC);
	assert_true(obs_encoder_gpu_scaling_enabled(v1));
	obs_encoder_set_scaled_size(v2, 320, 180);
	obs_encoder_set_gpu_scale_type(v2, OBS_SCALE_BICUBIC);
	obs_encoder_set_gpu_scale_type(v3, OBS_SCALE_LANCZOS);
	obs_encoder_set_preferred_video_format(v3, VIDEO_FORMAT_I444);
	obs_encoder_set_preferred_color_space(v3, VIDEO_CS_601);
	obs_encoder_set_preferred_range(v3, VIDEO_RANGE_FULL);
	assert_true(obs_encoder_set_frame_rate_divisor(v3, 2));

	/* v1 and v2 in a group */
	obs_encoder_group_t *group = obs_encoder_group_create();
	assert_true(obs_encoder_set_group(v1, group));
	assert_true(obs_encoder_set_group(v2, group));

	obs_output_t *outs[3];
	for (int i = 0; i < 3; i++) {
		char name[8];
		snprintf(name, sizeof(name), "out%d", i);
		outs[i] = make_encoded_output(name, venc[i], aenc[i], services[i]);
	}
	for (int i = 0; i < 3; i++)
		assert_true(obs_output_start(outs[i]));
	for (int i = 0; i < 3; i++)
		assert_true(wait_for(&fake_output_of(outs[i])->video_packets, 5));

	/* scaled encoders report their own video, with the main video as parent */
	assert_ptr_not_equal(obs_encoder_video(v1), obs_get_video());
	assert_non_null(obs_encoder_parent_video(v1));
	assert_int_equal(obs_encoder_get_width(v1), 320);
	assert_true(obs_encoder_scaling_enabled(v1));
	/* settings cannot change while active */
	obs_encoder_set_gpu_scale_type(v1, OBS_SCALE_POINT);
	assert_int_equal(obs_encoder_get_scale_type(v1), OBS_SCALE_BICUBIC);
	assert_false(obs_encoder_set_frame_rate_divisor(v1, 3));
	assert_false(obs_encoder_set_group(v1, NULL));

	/* an update while running is applied by the encoder thread */
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", 1234);
	obs_encoder_update(v1, settings);
	obs_encoder_update(v3, settings);
	obs_data_release(settings);
	long long before = fake_output_of(outs[0])->video_packets;
	assert_true(wait_for(&fake_output_of(outs[0])->video_packets, before + 3));

	for (int i = 0; i < 3; i++) {
		obs_output_stop(outs[i]);
		assert_true(wait_inactive(outs[i]));
	}

	/* Regression: leaving a group kept the group pointer, so leaving again (or destroying the encoder, which
	 * leaves its group) released the encoder a second time and used the group after it was freed */
	assert_true(obs_encoder_set_group(v1, NULL));
	assert_true(obs_encoder_set_group(v1, NULL));
	obs_encoder_group_destroy(group);
	/* v2 still holds the group; it leaves it when it is destroyed */

	for (int i = 0; i < 3; i++) {
		obs_output_release(outs[i]);
		obs_encoder_release(venc[i]);
		obs_encoder_release(aenc[i]);
		obs_service_release(services[i]);
	}
	obs_set_output_source(0, NULL);
	obs_source_release(src);
	core_stop();
}

static long long packet_callbacks;
static void on_packet(obs_output_t *output, struct encoder_packet *pkt, struct encoder_packet_time *pkt_time,
		      void *param)
{
	UNUSED_PARAMETER(output);
	UNUSED_PARAMETER(pkt);
	UNUSED_PARAMETER(pkt_time);
	UNUSED_PARAMETER(param);
	packet_callbacks++;
}

static int reconnects;
static void on_reconnect(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);
	reconnects++;
}

static void delay_reconnect_test(void **state)
{
	UNUSED_PARAMETER(state);
	start();

	obs_source_t *src = obs_source_create("fake_input", "src", NULL, NULL);
	obs_set_output_source(0, src);

	obs_output_t *out = obs_output_create("fake_encoded_output", "enc", NULL, NULL);
	obs_encoder_t *venc = obs_video_encoder_create("fake_video_enc", "venc", NULL, NULL);
	obs_encoder_t *aenc = obs_audio_encoder_create("fake_audio_enc", "aenc", NULL, 0, NULL);
	obs_encoder_set_video(venc, obs_get_video());
	obs_encoder_set_audio(aenc, obs_get_audio());
	obs_output_set_video_encoder(out, venc);
	obs_output_set_audio_encoder(out, aenc, 0);
	obs_data_t *ss = obs_data_create();
	obs_data_set_string(ss, "server", "fake://server");
	obs_service_t *service = obs_service_create("fake_service", "svc", ss, NULL);
	obs_data_release(ss);
	obs_output_set_service(out, service);
	struct fake_output *fo = fake_output_of(out);

	/* packets go through the delay buffer */
	obs_output_set_delay(out, 1, 0);
	assert_int_equal(obs_output_get_delay(out), 1);
	packet_callbacks = 0;
	obs_output_add_packet_callback(out, on_packet, NULL);
	assert_true(obs_output_start(out));
	os_sleep_ms(300);
	/* nothing has come out of the 1 s delay yet */
	assert_int_equal(fo->video_packets, 0);
	assert_true(obs_output_get_active_delay(out) <= 1);
	assert_true(wait_for(&fo->video_packets, 5));
	assert_true(packet_callbacks > 0);
	obs_output_stop(out);
	assert_true(wait_inactive(out));
	obs_output_remove_packet_callback(out, on_packet, NULL);
	obs_output_set_delay(out, 0, 0);

	/* a disconnect reconnects */
	signal_handler_t *sh = obs_output_get_signal_handler(out);
	reconnects = 0;
	signal_handler_connect(sh, "reconnect", on_reconnect, NULL);
	obs_output_set_reconnect_settings(out, 2, 1);
	assert_true(obs_output_start(out));
	assert_true(wait_for(&fo->video_packets, 10));
	int starts = fo->starts;
	obs_output_signal_stop(out, OBS_OUTPUT_DISCONNECTED);
	for (int i = 0; i < 400 && reconnects == 0; i++)
		os_sleep_ms(5);
	assert_true(reconnects >= 1);
	for (int i = 0; i < 600 && fo->starts == starts; i++)
		os_sleep_ms(5);
	assert_true(fo->starts > starts);
	obs_output_stop(out);
	assert_true(wait_inactive(out));
	signal_handler_disconnect(sh, "reconnect", on_reconnect, NULL);

	/* other errors stop the output */
	obs_output_set_reconnect_settings(out, 0, 0);
	assert_true(obs_output_start(out));
	obs_output_signal_stop(out, OBS_OUTPUT_ERROR);
	assert_true(wait_inactive(out));

	obs_output_release(out);
	obs_encoder_release(venc);
	obs_encoder_release(aenc);
	obs_service_release(service);
	obs_set_output_source(0, NULL);
	obs_source_release(src);
	core_stop();
}

static void service_test(void **state)
{
	UNUSED_PARAMETER(state);
	start();

	assert_string_equal(obs_service_get_display_name("fake_service"), "Fake");
	obs_data_t *ss = obs_data_create();
	obs_data_set_string(ss, "server", "fake://a");
	obs_service_t *service = obs_service_create("fake_service", "svc", ss, NULL);
	obs_data_release(ss);
	assert_non_null(service);
	assert_string_equal(obs_service_get_name(service), "svc");
	assert_string_equal(obs_service_get_id(service), "fake_service");
	assert_string_equal(obs_service_get_type(service), "fake_service");
	assert_string_equal(obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_SERVER_URL), "fake://a");
	assert_string_equal(obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_STREAM_KEY), "key");
	assert_null(obs_service_get_connect_info(service, OBS_SERVICE_CONNECT_INFO_USERNAME));
	assert_true(obs_service_can_try_to_connect(service));
	assert_string_equal(obs_service_get_protocol(service), "FAKE");
	assert_string_equal(obs_service_get_preferred_output_type(service), "fake_encoded_output");
	const char **codecs = obs_service_get_supported_video_codecs(service);
	assert_string_equal(codecs[0], "h264");
	assert_null(obs_service_get_supported_audio_codecs(service));
	int vb = 0, ab = 0;
	obs_service_get_max_bitrate(service, &vb, &ab);
	assert_int_equal(vb, 6000);
	assert_int_equal(ab, 320);
	int fps = -1;
	obs_service_get_max_fps(service, &fps);
	assert_int_equal(fps, 0);

	obs_data_t *v = obs_data_create(), *a = obs_data_create();
	obs_service_apply_encoder_settings(service, v, a);
	assert_int_equal(obs_data_get_int(v, "bitrate"), 6000);
	assert_int_equal(obs_data_get_int(a, "bitrate"), 160);
	obs_data_release(v);
	obs_data_release(a);

	ss = obs_data_create();
	obs_data_set_string(ss, "server", "");
	obs_service_update(service, ss);
	obs_data_release(ss);
	assert_false(obs_service_can_try_to_connect(service));

	obs_service_t *found = obs_get_service_by_name("svc");
	assert_ptr_equal(found, service);
	obs_service_release(found);
	obs_service_t *priv = obs_service_create_private("fake_service", "priv", NULL);
	assert_null(obs_get_service_by_name("priv"));
	obs_service_release(priv);

	/* an output only takes a service whose protocol it supports */
	obs_output_t *raw = obs_output_create("fake_raw_output", "raw", NULL, NULL);
	obs_output_set_service(raw, service);
	assert_null(obs_output_get_service(raw));
	obs_output_release(raw);

	obs_service_release(service);
	core_stop();
}

/* a service type with only the required callbacks, and one with the optional ones fake_service lacks */
static const char *bare_service_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "Bare";
}

static void *bare_service_create(obs_data_t *settings, obs_service_t *service)
{
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(service);
	return bzalloc(1);
}

static void *failing_service_create(obs_data_t *settings, obs_service_t *service)
{
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(service);
	return NULL;
}

static void bare_service_destroy(void *data)
{
	bfree(data);
}

static const char *bare_service_protocol(void *data)
{
	UNUSED_PARAMETER(data);
	return "BARE";
}

static void extra_service_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "fps", 30);
}

static obs_properties_t *extra_service_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_add_int(props, "fps", "FPS", 1, 120, 1);
	return props;
}

static struct obs_service_resolution extra_resolutions[] = {{1920, 1080}, {1280, 720}};

static void extra_service_resolutions(void *data, struct obs_service_resolution **resolutions, size_t *count)
{
	UNUSED_PARAMETER(data);
	*resolutions = bmemdup(extra_resolutions, sizeof(extra_resolutions));
	*count = 2;
}

static void extra_service_max_fps(void *data, int *fps)
{
	UNUSED_PARAMETER(data);
	*fps = 60;
}

static const char **extra_service_audio_codecs(void *data)
{
	UNUSED_PARAMETER(data);
	static const char *codecs[] = {"opus", NULL};
	return codecs;
}

static int service_type_data;

static void service_extra_test(void **state)
{
	UNUSED_PARAMETER(state);
	start();

	struct obs_service_info bare = {
		.id = "bare_service",
		.get_name = bare_service_name,
		.create = bare_service_create,
		.destroy = bare_service_destroy,
		.get_protocol = bare_service_protocol,
	};
	obs_register_service(&bare);
	struct obs_service_info extra = bare;
	extra.id = "extra_service";
	extra.type_data = &service_type_data;
	extra.get_defaults = extra_service_defaults;
	extra.get_properties = extra_service_properties;
	extra.get_supported_resolutions = extra_service_resolutions;
	extra.get_max_fps = extra_service_max_fps;
	extra.get_supported_audio_codecs = extra_service_audio_codecs;
	obs_register_service(&extra);
	struct obs_service_info failing = bare;
	failing.id = "failing_service";
	failing.create = failing_service_create;
	obs_register_service(&failing);

	/* by type */
	assert_null(obs_service_get_display_name("missing"));
	assert_null(obs_service_defaults("missing"));
	assert_null(obs_get_service_properties("missing"));
	assert_null(obs_get_service_properties("bare_service"));
	assert_null(obs_service_create("missing", "x", NULL, NULL));
	/* registered by the test, not by a module */
	assert_null(obs_service_get_module("extra_service"));
	assert_int_equal(obs_service_load_state("extra_service"), OBS_MODULE_MISSING);

	obs_data_t *defaults = obs_service_defaults("extra_service");
	assert_int_equal(obs_data_get_int(defaults, "fps"), 30);
	obs_data_release(defaults);
	defaults = obs_service_defaults("bare_service");
	assert_non_null(defaults);
	obs_data_release(defaults);
	obs_properties_t *props = obs_get_service_properties("extra_service");
	assert_non_null(obs_properties_get(props, "fps"));
	obs_properties_destroy(props);

	/* an instance with the optional callbacks */
	obs_service_t *svc = obs_service_create("extra_service", "extra", NULL, NULL);
	assert_non_null(svc);
	assert_ptr_equal(obs_service_get_type_data(svc), &service_type_data);
	props = obs_service_properties(svc);
	assert_non_null(props);
	obs_properties_destroy(props);
	/* Regression: the type's defaults were not applied to the instance's settings */
	obs_data_t *settings = obs_service_get_settings(svc);
	assert_int_equal(obs_data_get_int(settings, "fps"), 30);
	obs_data_release(settings);

	struct obs_service_resolution *res = NULL;
	size_t count = 0;
	obs_service_get_supported_resolutions(svc, &res, &count);
	assert_int_equal(count, 2);
	assert_int_equal(res[1].cy, 720);
	bfree(res);
	int fps = 0;
	obs_service_get_max_fps(svc, &fps);
	assert_int_equal(fps, 60);
	assert_string_equal(obs_service_get_supported_audio_codecs(svc)[0], "opus");
	assert_null(obs_service_get_supported_video_codecs(svc));

	/* references */
	obs_weak_service_t *weak = obs_service_get_weak_service(svc);
	assert_true(obs_weak_service_references_service(weak, svc));
	obs_weak_service_addref(weak);
	obs_weak_service_release(weak);
	obs_service_t *ref = obs_service_get_ref(svc);
	assert_ptr_equal(ref, svc);
	obs_service_release(ref);
	ref = obs_weak_service_get_service(weak);
	assert_ptr_equal(ref, svc);
	obs_service_release(ref);
	obs_service_release(svc);
	assert_null(obs_weak_service_get_service(weak));
	obs_weak_service_release(weak);

	/* an instance with only the required callbacks gets the defaults */
	svc = obs_service_create("bare_service", "bare", NULL, NULL);
	assert_null(obs_service_properties(svc));
	assert_null(obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_SERVER_URL));
	assert_true(obs_service_can_try_to_connect(svc));
	assert_null(obs_service_get_preferred_output_type(svc));
	assert_null(obs_service_get_supported_video_codecs(svc));
	assert_null(obs_service_get_supported_audio_codecs(svc));
	res = (void *)1;
	obs_service_get_supported_resolutions(svc, &res, &count);
	assert_null(res);
	assert_int_equal(count, 0);
	fps = -1;
	obs_service_get_max_fps(svc, &fps);
	assert_int_equal(fps, 0);
	int vb = -1, ab = -1;
	obs_service_get_max_bitrate(svc, &vb, &ab);
	assert_int_equal(vb, 0);
	assert_int_equal(ab, 0);
	obs_data_t *v = obs_data_create();
	obs_service_apply_encoder_settings(svc, v, NULL);
	assert_int_equal(obs_data_get_int(v, "bitrate"), 0);
	obs_data_release(v);
	obs_service_get_supported_resolutions(svc, NULL, &count);
	obs_service_get_supported_resolutions(svc, &res, NULL);
	obs_service_get_max_fps(svc, NULL);
	obs_service_release(svc);

	/* a create callback that fails still gives a service, without data */
	svc = obs_service_create("failing_service", "failing", NULL, NULL);
	assert_non_null(svc);
	assert_string_equal(obs_service_get_protocol(svc), "BARE");
	obs_service_release(svc);

	/* NULL */
	assert_null(obs_service_get_name(NULL));
	assert_null(obs_service_get_id(NULL));
	assert_null(obs_service_get_type(NULL));
	assert_null(obs_service_properties(NULL));
	assert_null(obs_service_get_settings(NULL));
	assert_null(obs_service_get_type_data(NULL));
	assert_null(obs_service_get_protocol(NULL));
	assert_null(obs_service_get_preferred_output_type(NULL));
	assert_null(obs_service_get_connect_info(NULL, 0));
	assert_false(obs_service_can_try_to_connect(NULL));
	/* Regression: these two dereferenced NULL */
	assert_null(obs_service_get_supported_video_codecs(NULL));
	assert_null(obs_service_get_supported_audio_codecs(NULL));
	obs_service_update(NULL, NULL);
	obs_service_apply_encoder_settings(NULL, NULL, NULL);
	vb = ab = -1;
	obs_service_get_max_bitrate(NULL, &vb, &ab);
	assert_int_equal(vb, 0);
	obs_service_get_max_bitrate(NULL, NULL, NULL);
	obs_service_get_max_fps(NULL, &fps);
	obs_service_get_supported_resolutions(NULL, &res, &count);
	assert_null(obs_service_get_ref(NULL));
	assert_null(obs_service_get_weak_service(NULL));
	assert_null(obs_weak_service_get_service(NULL));
	assert_false(obs_weak_service_references_service(NULL, NULL));
	obs_weak_service_addref(NULL);
	obs_weak_service_release(NULL);
	obs_service_release(NULL);

	core_stop();
}

static void on_object_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	UNUSED_PARAMETER(pressed);
}

static bool on_object_pair(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	return pressed;
}

struct hotkey_find {
	const char *name;
	const char *description;
	obs_hotkey_registerer_t type;
	void *registerer;
};

static bool find_named_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *key)
{
	UNUSED_PARAMETER(id);
	struct hotkey_find *find = data;
	if (strcmp(obs_hotkey_get_name(key), find->name) != 0)
		return true;
	find->description = obs_hotkey_get_description(key);
	find->type = obs_hotkey_get_registerer_type(key);
	find->registerer = obs_hotkey_get_registerer(key);
	return false;
}

static struct hotkey_find find_hotkey_named(const char *name)
{
	struct hotkey_find find = {.name = name};
	obs_enum_hotkeys(find_named_hotkey, &find);
	return find;
}

/* hotkeys of encoders, outputs and services, saved and loaded with them; translated key and built-in names */
static void object_hotkeys_test(void **state)
{
	UNUSED_PARAMETER(state);
	start();

	struct obs_hotkeys_translations t = {
		.insert = "Einfg",       .del = "Entf",          .home = "Pos1",
		.end = "Ende",           .page_up = "Bild auf",  .page_down = "Bild ab",
		.num_lock = "Num",       .scroll_lock = "Rollen", .caps_lock = "Feststell",
		.backspace = "Ruecktaste", .tab = "Tabulator",   .print = "Druck",
		.pause = "Pause",        .left = "Links",        .right = "Rechts",
		.up = "Hoch",            .down = "Runter",       .shift = "Umschalt",
		.alt = "Alt",            .control = "Strg",      .meta = "Win",
		.menu = "Menue",         .space = "Leer",        .numpad_num = "Ziffernblock %1",
		.numpad_divide = "ZB /", .numpad_multiply = "ZB *", .numpad_minus = "ZB -",
		.numpad_plus = "ZB +",   .numpad_decimal = "ZB ,", .apple_keypad_num = "%1 (Keypad)",
		.apple_keypad_divide = "/ (Keypad)", .apple_keypad_multiply = "* (Keypad)",
		.apple_keypad_minus = "- (Keypad)", .apple_keypad_plus = "+ (Keypad)",
		.apple_keypad_decimal = ". (Keypad)", .apple_keypad_equal = "= (Keypad)",
		.mouse_num = "Maus %1",  .escape = "Esc",
	};
	obs_hotkeys_set_translations(&t);
	obs_hotkeys_set_translations_s(NULL, 0);
	struct dstr str = {0};
	obs_key_to_str(OBS_KEY_MOUSE3, &str);
	assert_string_equal(str.array, "Maus 3");
	dstr_free(&str);

	obs_hotkeys_set_audio_hotkeys_translations("Stumm", "Laut", "Druecken zum Stummschalten", "Druecken zum Sprechen",
						   "Mithoeren an", "Mithoeren aus");
	obs_hotkeys_set_sceneitem_hotkeys_translations("Zeigen '%1'", "Verbergen '%1'");

	obs_encoder_t *enc = obs_video_encoder_create("fake_video_enc", "enc", NULL, NULL);
	obs_output_t *out = obs_output_create("fake_raw_output", "out", NULL, NULL);
	obs_service_t *svc = obs_service_create("fake_service", "svc", NULL, NULL);

	obs_hotkey_id eid = obs_hotkey_register_encoder(enc, "enc.key", "Encoder key", on_object_hotkey, NULL);
	obs_hotkey_id oid = obs_hotkey_register_output(out, "out.key", "Output key", on_object_hotkey, NULL);
	obs_hotkey_id sid = obs_hotkey_register_service(svc, "svc.key", "Service key", on_object_hotkey, NULL);
	assert_true(eid != OBS_INVALID_HOTKEY_ID && oid != OBS_INVALID_HOTKEY_ID && sid != OBS_INVALID_HOTKEY_ID);
	assert_true(obs_hotkey_pair_register_encoder(enc, "enc.on", "On", "enc.off", "Off", on_object_pair,
						     on_object_pair, NULL, NULL) != OBS_INVALID_HOTKEY_PAIR_ID);
	assert_true(obs_hotkey_pair_register_output(out, "out.on", "On", "out.off", "Off", on_object_pair,
						    on_object_pair, NULL, NULL) != OBS_INVALID_HOTKEY_PAIR_ID);
	assert_true(obs_hotkey_pair_register_service(svc, "svc.on", "On", "svc.off", "Off", on_object_pair,
						     on_object_pair, NULL, NULL) != OBS_INVALID_HOTKEY_PAIR_ID);

	struct hotkey_find found = find_hotkey_named("enc.key");
	assert_int_equal(found.type, OBS_HOTKEY_REGISTERER_ENCODER);
	found = find_hotkey_named("out.key");
	assert_int_equal(found.type, OBS_HOTKEY_REGISTERER_OUTPUT);
	found = find_hotkey_named("svc.key");
	assert_int_equal(found.type, OBS_HOTKEY_REGISTERER_SERVICE);
	assert_non_null(found.registerer);

	/* bindings save and load per object */
	obs_key_combination_t f20 = {INTERACT_CONTROL_KEY, OBS_KEY_F20};
	obs_hotkey_load_bindings(eid, &f20, 1);
	obs_hotkey_load_bindings(oid, &f20, 1);
	obs_hotkey_load_bindings(sid, &f20, 1);
	obs_data_t *eh = obs_hotkeys_save_encoder(enc);
	obs_data_t *oh = obs_hotkeys_save_output(out);
	obs_data_t *sh = obs_hotkeys_save_service(svc);
	assert_true(obs_data_has_user_value(eh, "enc.key"));
	assert_true(obs_data_has_user_value(oh, "out.key"));
	assert_true(obs_data_has_user_value(sh, "svc.key"));
	obs_hotkeys_load_encoder(enc, eh);
	obs_hotkeys_load_output(out, oh);
	obs_hotkeys_load_service(svc, sh);
	obs_data_release(eh);
	obs_data_release(oh);
	obs_data_release(sh);
	/* Regression: the save functions dereferenced NULL */
	assert_null(obs_hotkeys_save_encoder(NULL));
	assert_null(obs_hotkeys_save_source(NULL));
	assert_null(obs_hotkeys_save_output(NULL));
	assert_null(obs_hotkeys_save_service(NULL));
	obs_hotkeys_load_encoder(NULL, NULL);
	obs_hotkeys_load_output(NULL, NULL);
	obs_hotkeys_load_service(NULL, NULL);
	assert_int_equal(obs_hotkey_register_encoder(NULL, "x", "x", on_object_hotkey, NULL), OBS_INVALID_HOTKEY_ID);
	assert_int_equal(obs_hotkey_register_output(NULL, "x", "x", on_object_hotkey, NULL), OBS_INVALID_HOTKEY_ID);
	assert_int_equal(obs_hotkey_register_service(NULL, "x", "x", on_object_hotkey, NULL), OBS_INVALID_HOTKEY_ID);

	/* a source with audio gets the translated built-in hotkeys, a scene item the show/hide ones */
	obs_source_t *src = obs_source_create("fake_input", "mic", NULL, NULL);
	found = find_hotkey_named("libobs.mute");
	assert_string_equal(found.description, "Stumm");
	found = find_hotkey_named("libobs.push-to-talk");
	assert_string_equal(found.description, "Druecken zum Sprechen");
	obs_scene_t *scene = obs_scene_create("scene");
	obs_scene_add(scene, src);
	found = find_hotkey_named("libobs.show_scene_item.1");
	assert_non_null(found.description);
	assert_string_equal(found.description, "Zeigen 'mic'");
	obs_scene_release(scene);
	obs_source_release(src);

	/* the objects' hotkeys go with them */
	obs_encoder_release(enc);
	obs_output_release(out);
	obs_service_release(svc);
	obs_wait_for_destroy_queue();
	assert_null(find_hotkey_named("enc.key").description);
	assert_null(find_hotkey_named("svc.on").description);

	core_stop();
}

static void null_test(void **state)
{
	UNUSED_PARAMETER(state);
	start();

	/* unknown ids create an output that keeps its settings, like sources */
	obs_output_t *missing = obs_output_create("missing", "x", NULL, NULL);
	assert_false(obs_output_start(missing));
	obs_output_release(missing);
	assert_false(obs_output_start(NULL));
	obs_output_stop(NULL);
	assert_false(obs_output_active(NULL));
	obs_output_release(NULL);
	assert_null(obs_output_get_name(NULL));
	obs_encoder_release(NULL);
	assert_null(obs_encoder_get_name(NULL));
	assert_false(obs_encoder_active(NULL));
	obs_service_release(NULL);
	assert_null(obs_service_get_name(NULL));
	obs_encoder_t *missing_enc = obs_video_encoder_create("missing", "x", NULL, NULL);
	obs_encoder_release(missing_enc);

	core_stop();
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(raw_output_test),     cmocka_unit_test(encoder_test),
		cmocka_unit_test(encoded_output_test), cmocka_unit_test(delay_reconnect_test),
		cmocka_unit_test(service_test), cmocka_unit_test(service_extra_test), cmocka_unit_test(object_hotkeys_test), cmocka_unit_test(captions_test), cmocka_unit_test(gpu_rescale_group_test), cmocka_unit_test(gpu_encode_test),        cmocka_unit_test(null_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
