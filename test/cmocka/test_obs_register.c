#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-core-test.h"

/* obs-module.c: obs_register_source/output/encoder/service validation */

static int freed_type_data;

static void free_type_data(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	freed_type_data++;
}

static const char *reg_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "Registered";
}

static void *reg_create(obs_data_t *settings, void *owner)
{
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(owner);
	return bzalloc(1);
}

static void reg_destroy(void *data)
{
	bfree(data);
}

static uint32_t reg_size(void *data)
{
	UNUSED_PARAMETER(data);
	return 4;
}

static bool reg_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio_output,
			     uint32_t mixers, size_t channels, size_t sample_rate)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(ts_out);
	UNUSED_PARAMETER(audio_output);
	UNUSED_PARAMETER(mixers);
	UNUSED_PARAMETER(channels);
	UNUSED_PARAMETER(sample_rate);
	return false;
}

static int type_data_value;

/* registers info and reports whether it was accepted */
static bool source_registered(struct obs_source_info info, size_t size)
{
	info.type_data = &type_data_value;
	info.free_type_data = free_type_data;
	int before = freed_type_data;
	obs_register_source_s(&info, size);
	/* a rejected type frees its type data right away, an accepted one at shutdown */
	bool registered = freed_type_data == before;
	if (registered)
		assert_non_null(obs_source_get_display_name(info.id));
	else
		assert_int_equal(freed_type_data, before + 1);
	return registered;
}

static void source_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	struct obs_source_info base = {
		.id = "reg_input",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_VIDEO,
		.get_name = reg_name,
		.create = (void *(*)(obs_data_t *, obs_source_t *))reg_create,
		.destroy = reg_destroy,
		.get_width = reg_size,
		.get_height = reg_size,
	};
	const size_t size = sizeof(base);
	assert_true(source_registered(base, size));
	/* the same id again */
	assert_false(source_registered(base, size));

	struct obs_source_info info = base;
	info.id = "reg_unknown_type";
	info.type = (enum obs_source_type)42;
	assert_false(source_registered(info, size));

	info = base;
	info.id = "reg_too_big";
	assert_false(source_registered(info, size + 8));

	info = base;
	info.id = "reg_no_name";
	info.get_name = NULL;
	assert_false(source_registered(info, size));

	/* synchronous video inputs need a size */
	info = base;
	info.id = "reg_no_width";
	info.get_width = NULL;
	assert_false(source_registered(info, size));
	info.id = "reg_no_height";
	info.get_width = reg_size;
	info.get_height = NULL;
	assert_false(source_registered(info, size));
	info.id = "reg_async_no_size";
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO;
	info.get_width = NULL;
	assert_true(source_registered(info, size));

	/* composite sources render their own audio and are never async */
	info = base;
	info.id = "reg_composite_audio";
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_COMPOSITE | OBS_SOURCE_AUDIO;
	info.audio_render = reg_audio_render;
	assert_false(source_registered(info, size));
	info.id = "reg_composite_async";
	info.output_flags = OBS_SOURCE_COMPOSITE | OBS_SOURCE_ASYNC_VIDEO;
	assert_false(source_registered(info, size));
	info.id = "reg_composite_no_render";
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_COMPOSITE;
	info.audio_render = NULL;
	assert_false(source_registered(info, size));
	info.id = "reg_composite";
	info.audio_render = reg_audio_render;
	assert_true(source_registered(info, size));

	/* transitions ignore a size and are always composite video */
	info = base;
	info.id = "reg_transition";
	info.type = OBS_SOURCE_TYPE_TRANSITION;
	info.output_flags = 0;
	info.audio_render = reg_audio_render;
	assert_true(source_registered(info, size));
	uint32_t flags = obs_get_source_output_flags("reg_transition");
	assert_true((flags & OBS_SOURCE_COMPOSITE) && (flags & OBS_SOURCE_VIDEO) && (flags & OBS_SOURCE_CUSTOM_DRAW));

	/* audio-only filters are async */
	info = base;
	info.id = "reg_audio_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_AUDIO;
	assert_true(source_registered(info, size));
	assert_true(obs_get_source_output_flags("reg_audio_filter") & OBS_SOURCE_ASYNC);

	/* versions get their own ids */
	info = base;
	info.id = "reg_versioned";
	info.version = 2;
	info.type_data = &type_data_value;
	obs_register_source(&info);
	assert_string_equal(obs_get_latest_input_type_id("reg_versioned"), "reg_versioned_v2");
	assert_non_null(obs_source_get_display_name("reg_versioned_v2"));

	core_stop();
}

static bool reg_start(void *data)
{
	UNUSED_PARAMETER(data);
	return false;
}

static void reg_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(ts);
}

static void reg_raw_video(void *data, struct video_data *frame)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(frame);
}

static void reg_raw_audio(void *data, struct audio_data *frames)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(frames);
}

static void reg_raw_audio2(void *data, size_t idx, struct audio_data *frames)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(idx);
	UNUSED_PARAMETER(frames);
}

static void reg_encoded_packet(void *data, struct encoder_packet *packet)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(packet);
}

static bool output_registered(struct obs_output_info info, size_t size)
{
	info.type_data = &type_data_value;
	info.free_type_data = free_type_data;
	int before = freed_type_data;
	obs_register_output_s(&info, size);
	/* a rejected type frees its type data right away, an accepted one at shutdown */
	bool registered = freed_type_data == before;
	if (registered)
		assert_non_null(obs_output_get_display_name(info.id));
	else
		assert_int_equal(freed_type_data, before + 1);
	return registered;
}

static bool has_protocol(const char *name)
{
	char *protocol;
	for (size_t i = 0; obs_enum_output_protocols(i, &protocol); i++) {
		if (strcmp(protocol, name) == 0)
			return true;
	}
	return false;
}

static void output_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	struct obs_output_info base = {
		.id = "reg_output",
		.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_AUDIO,
		.get_name = reg_name,
		.create = (void *(*)(obs_data_t *, obs_output_t *))reg_create,
		.destroy = reg_destroy,
		.start = reg_start,
		.stop = reg_stop,
		.raw_video = reg_raw_video,
		.raw_audio = reg_raw_audio,
	};
	const size_t size = sizeof(base);
	assert_true(output_registered(base, size));
	assert_false(output_registered(base, size));

	struct obs_output_info info = base;
	info.id = "reg_output_big";
	assert_false(output_registered(info, size + 8));

	/* every required callback */
	const char *ids[] = {"reg_no_get_name", "reg_no_create", "reg_no_destroy", "reg_no_start", "reg_no_stop"};
	for (int i = 0; i < 5; i++) {
		info = base;
		info.id = ids[i];
		if (i == 0)
			info.get_name = NULL;
		else if (i == 1)
			info.create = NULL;
		else if (i == 2)
			info.destroy = NULL;
		else if (i == 3)
			info.start = NULL;
		else
			info.stop = NULL;
		assert_false(output_registered(info, size));
	}

	info = base;
	info.id = "reg_no_raw_video";
	info.raw_video = NULL;
	assert_false(output_registered(info, size));
	info = base;
	info.id = "reg_no_raw_audio";
	info.raw_audio = NULL;
	assert_false(output_registered(info, size));
	info.id = "reg_monitoring_audio";
	info.raw_audio_monitoring = reg_raw_audio;
	assert_true(output_registered(info, size));
	info = base;
	info.id = "reg_no_raw_audio2";
	info.flags |= OBS_OUTPUT_MULTI_TRACK;
	assert_false(output_registered(info, size));
	info.id = "reg_multi_track";
	info.raw_audio2 = reg_raw_audio2;
	assert_true(output_registered(info, size));

	info = base;
	info.id = "reg_no_packet";
	info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED;
	assert_false(output_registered(info, size));

	/* service outputs list their protocols once */
	info.encoded_packet = reg_encoded_packet;
	info.flags |= OBS_OUTPUT_SERVICE;
	info.id = "reg_no_protocols";
	assert_false(output_registered(info, size));
	info.id = "reg_protocols_a";
	info.protocols = "REGA;REGB";
	assert_true(output_registered(info, size));
	info.id = "reg_protocols_b";
	info.protocols = "REGB;REGC";
	assert_true(output_registered(info, size));
	assert_true(has_protocol("REGA"));
	assert_true(has_protocol("REGC"));
	int regb = 0;
	char *protocol;
	for (size_t i = 0; obs_enum_output_protocols(i, &protocol); i++)
		regb += strcmp(protocol, "REGB") == 0;
	assert_int_equal(regb, 1);

	core_stop();
}

static bool reg_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet, bool *received)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(frame);
	UNUSED_PARAMETER(packet);
	UNUSED_PARAMETER(received);
	return false;
}

static bool reg_encode_texture2(void *data, struct encoder_texture *texture, int64_t pts, uint64_t lock_key,
				uint64_t *next_key, struct encoder_packet *packet, bool *received_packet)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(texture);
	UNUSED_PARAMETER(pts);
	UNUSED_PARAMETER(lock_key);
	UNUSED_PARAMETER(next_key);
	UNUSED_PARAMETER(packet);
	UNUSED_PARAMETER(received_packet);
	return false;
}

static size_t reg_frame_size(void *data)
{
	UNUSED_PARAMETER(data);
	return 1024;
}

static bool encoder_registered(struct obs_encoder_info info, size_t size)
{
	info.type_data = &type_data_value;
	info.free_type_data = free_type_data;
	int before = freed_type_data;
	obs_register_encoder_s(&info, size);
	/* a rejected type frees its type data right away, an accepted one at shutdown */
	bool registered = freed_type_data == before;
	if (registered)
		assert_non_null(obs_encoder_get_display_name(info.id));
	else
		assert_int_equal(freed_type_data, before + 1);
	return registered;
}

static void encoder_service_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	struct obs_encoder_info base = {
		.id = "reg_encoder",
		.type = OBS_ENCODER_VIDEO,
		.codec = "h264",
		.get_name = reg_name,
		.create = (void *(*)(obs_data_t *, obs_encoder_t *))reg_create,
		.destroy = reg_destroy,
		.encode = reg_encode,
	};
	const size_t size = sizeof(base);
	assert_true(encoder_registered(base, size));
	assert_false(encoder_registered(base, size));

	struct obs_encoder_info info = base;
	info.id = "reg_encoder_big";
	assert_false(encoder_registered(info, size + 8));
	info.id = "reg_no_encode";
	info.encode = NULL;
	assert_false(encoder_registered(info, size));
	info.id = "reg_no_encoder_create";
	info.encode = reg_encode;
	info.create = NULL;
	assert_false(encoder_registered(info, size));

	/* texture encoders need a texture encode function and cannot scale */
	info = base;
	info.id = "reg_texture_no_encode";
	info.caps = OBS_ENCODER_CAP_PASS_TEXTURE;
	info.encode = NULL;
	assert_false(encoder_registered(info, size));
	info.id = "reg_texture_scaling";
	info.encode_texture2 = reg_encode_texture2;
	info.caps |= OBS_ENCODER_CAP_SCALING;
	assert_false(encoder_registered(info, size));
	info.id = "reg_texture";
	info.caps = OBS_ENCODER_CAP_PASS_TEXTURE;
	assert_true(encoder_registered(info, size));

	/* audio encoders need a frame size */
	info = base;
	info.id = "reg_audio_no_frame_size";
	info.type = OBS_ENCODER_AUDIO;
	info.codec = "aac";
	assert_false(encoder_registered(info, size));
	info.id = "reg_audio";
	info.get_frame_size = reg_frame_size;
	assert_true(encoder_registered(info, size));

	/* services */
	struct obs_service_info service = {
		.id = "reg_service",
		.get_name = reg_name,
		.create = (void *(*)(obs_data_t *, obs_service_t *))reg_create,
		.destroy = reg_destroy,
		.get_protocol = (const char *(*)(void *))reg_name,
		.type_data = &type_data_value,
		.free_type_data = free_type_data,
	};
	obs_register_service(&service);
	assert_non_null(obs_service_get_display_name("reg_service"));
	int before = freed_type_data;
	obs_register_service(&service);
	assert_int_equal(freed_type_data, before + 1);
	service.id = "reg_service_no_protocol";
	service.get_protocol = NULL;
	obs_register_service(&service);
	assert_null(obs_service_get_display_name("reg_service_no_protocol"));
	service.id = "reg_service_big";
	service.get_protocol = (const char *(*)(void *))reg_name;
	obs_register_service_s(&service, sizeof(service) + 8);
	assert_null(obs_service_get_display_name("reg_service_big"));
	assert_int_equal(freed_type_data, before + 3);

	core_stop();
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(source_test),
		cmocka_unit_test(output_test),
		cmocka_unit_test(encoder_service_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
