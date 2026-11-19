#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-core-test.h"

#include <math.h>

static bool near(float a, float b, float eps)
{
	return fabsf(a - b) < eps;
}

/* an audio output that records the peak of what it gets from the mix and the monitoring mix */
struct audio_output {
	obs_output_t *output;
	volatile long long mixes, monitor_mixes;
	float peak;
};

static void *ao_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);
	struct audio_output *ao = bzalloc(sizeof(*ao));
	ao->output = output;
	return ao;
}

static void ao_destroy(void *data)
{
	bfree(data);
}

static bool ao_start(void *data)
{
	struct audio_output *ao = data;
	if (!obs_output_can_begin_data_capture(ao->output, 0))
		return false;
	return obs_output_begin_data_capture(ao->output, 0);
}

static void ao_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(ts);
	obs_output_end_data_capture(((struct audio_output *)data)->output);
}

static void ao_raw_audio(void *data, struct audio_data *frames)
{
	struct audio_output *ao = data;
	const float *samples = (const float *)frames->data[0];
	for (uint32_t i = 0; i < frames->frames; i++) {
		if (fabsf(samples[i]) > ao->peak)
			ao->peak = fabsf(samples[i]);
	}
	ao->mixes++;
}

static void ao_raw_audio_monitoring(void *data, struct audio_data *frames)
{
	UNUSED_PARAMETER(frames);
	((struct audio_output *)data)->monitor_mixes++;
}

static void register_audio_output(void)
{
	struct obs_output_info info = {
		.id = "fake_audio_output",
		.flags = OBS_OUTPUT_AUDIO,
		.get_name = fake_get_name,
		.create = ao_create,
		.destroy = ao_destroy,
		.start = ao_start,
		.stop = ao_stop,
		.raw_audio = ao_raw_audio,
		.raw_audio_monitoring = ao_raw_audio_monitoring,
	};
	obs_register_output(&info);
}

static float tone[2][480];

/* pushes 10 ms of a constant 0.5 signal per call, timestamped from now */
static void push_audio(obs_source_t *src, int blocks)
{
	for (int i = 0; i < 480; i++)
		tone[0][i] = tone[1][i] = 0.5f;
	uint64_t ts = os_gettime_ns();
	for (int b = 0; b < blocks; b++) {
		struct obs_source_audio audio = {
			.data = {(uint8_t *)tone[0], (uint8_t *)tone[1]},
			.frames = 480,
			.speakers = SPEAKERS_STEREO,
			.format = AUDIO_FORMAT_FLOAT_PLANAR,
			.samples_per_sec = 48000,
			.timestamp = ts + b * 10000000ULL,
		};
		obs_source_output_audio(src, &audio);
	}
}

static bool wait_count(volatile long long *counter, long long count)
{
	for (int i = 0; i < 400 && *counter < count; i++)
		os_sleep_ms(5);
	return *counter >= count;
}

static void conversion_test(void **state)
{
	UNUSED_PARAMETER(state);

	assert_true(near(obs_db_to_mul(0.0f), 1.0f, 1e-6f));
	assert_true(near(obs_db_to_mul(-20.0f), 0.1f, 1e-6f));
	assert_true(obs_db_to_mul(-INFINITY) == 0.0f);
	assert_true(near(obs_mul_to_db(1.0f), 0.0f, 1e-6f));
	assert_true(near(obs_mul_to_db(0.1f), -20.0f, 1e-4f));
	assert_true(obs_mul_to_db(0.0f) == -INFINITY);
}

static int fader_changes;
static float fader_db;
static void on_fader(void *param, float db)
{
	UNUSED_PARAMETER(param);
	fader_changes++;
	fader_db = db;
}

static void fader_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	static const enum obs_fader_type types[] = {OBS_FADER_CUBIC, OBS_FADER_IEC, OBS_FADER_LOG};
	for (size_t t = 0; t < 3; t++) {
		obs_fader_t *fader = obs_fader_create(types[t]);
		assert_non_null(fader);
		assert_non_null(obs_fader_db_to_def(fader));

		/* deflection and dB convert both ways */
		assert_true(obs_fader_set_deflection(fader, 1.0f));
		assert_true(near(obs_fader_get_db(fader), 0.0f, 1e-3f));
		assert_true(near(obs_fader_get_mul(fader), 1.0f, 1e-3f));
		assert_true(obs_fader_set_deflection(fader, 0.5f));
		float db = obs_fader_get_db(fader);
		assert_true(db < 0.0f);
		assert_true(near(obs_fader_get_deflection(fader), 0.5f, 1e-3f));
		obs_fader_set_db(fader, db);
		assert_true(near(obs_fader_get_deflection(fader), 0.5f, 1e-3f));
		assert_true(obs_fader_set_mul(fader, 0.5f));
		assert_true(near(obs_fader_get_mul(fader), 0.5f, 1e-3f));
		/* out of range values are clamped */
		obs_fader_set_deflection(fader, 2.0f);
		assert_true(near(obs_fader_get_deflection(fader), 1.0f, 1e-3f));
		obs_fader_set_deflection(fader, -1.0f);
		assert_true(obs_fader_get_deflection(fader) == 0.0f);
		obs_fader_destroy(fader);
	}

	/* an attached fader follows and sets the source volume */
	obs_source_t *src = obs_source_create("fake_async", "src", NULL, NULL);
	obs_fader_t *fader = obs_fader_create(OBS_FADER_CUBIC);
	assert_true(obs_fader_attach_source(fader, src));
	fader_changes = 0;
	obs_fader_add_callback(fader, on_fader, NULL);
	obs_fader_set_mul(fader, 0.25f);
	assert_true(near(obs_source_get_volume(src), 0.25f, 1e-4f));
	obs_source_set_volume(src, 0.5f);
	assert_true(fader_changes > 0);
	assert_true(near(fader_db, obs_mul_to_db(0.5f), 1e-3f));
	assert_true(near(obs_fader_get_mul(fader), 0.5f, 1e-3f));
	obs_fader_remove_callback(fader, on_fader, NULL);
	obs_fader_detach_source(fader);
	obs_fader_set_mul(fader, 1.0f);
	assert_true(near(obs_source_get_volume(src), 0.5f, 1e-4f));

	/* destroying the source detaches it */
	obs_fader_attach_source(fader, src);
	obs_source_release(src);
	obs_fader_set_mul(fader, 0.3f);
	obs_fader_destroy(fader);

	/* NULL is ignored */
	obs_fader_destroy(NULL);
	assert_false(obs_fader_set_db(NULL, 0.0f));
	assert_true(obs_fader_get_db(NULL) == 0.0f);
	assert_false(obs_fader_attach_source(NULL, NULL));

	core_stop();
}

static volatile long long volmeter_updates;
static float last_peak[MAX_AUDIO_CHANNELS], last_magnitude[MAX_AUDIO_CHANNELS];
static void on_volmeter(void *param, const float magnitude[MAX_AUDIO_CHANNELS], const float peak[MAX_AUDIO_CHANNELS],
			const float input_peak[MAX_AUDIO_CHANNELS])
{
	UNUSED_PARAMETER(param);
	UNUSED_PARAMETER(input_peak);
	memcpy(last_peak, peak, sizeof(last_peak));
	memcpy(last_magnitude, magnitude, sizeof(last_magnitude));
	volmeter_updates++;
}

static void volmeter_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();

	obs_source_t *src = obs_source_create("fake_async", "src", NULL, NULL);
	static const enum obs_peak_meter_type meters[] = {SAMPLE_PEAK_METER, TRUE_PEAK_METER};
	for (size_t m = 0; m < 2; m++) {
		obs_volmeter_t *vm = obs_volmeter_create(OBS_FADER_LOG);
		obs_volmeter_set_peak_meter_type(vm, meters[m]);
		assert_true(obs_volmeter_attach_source(vm, src));
		volmeter_updates = 0;
		obs_volmeter_add_callback(vm, on_volmeter, NULL);
		push_audio(src, 10);
		assert_true(wait_count(&volmeter_updates, 1));
		/* the channel count is the source's, known once it sent audio */
		assert_int_equal(obs_volmeter_get_nr_channels(vm), 2);
		/* a 0.5 signal peaks at about -6 dB */
		assert_true(near(last_peak[0], -6.02f, 0.5f));
		assert_true(near(last_magnitude[0], -6.02f, 0.5f));
		obs_volmeter_remove_callback(vm, on_volmeter, NULL);
		obs_volmeter_detach_source(vm);
		obs_volmeter_destroy(vm);
	}

	/* a volmeter whose source goes away is detached */
	obs_volmeter_t *vm = obs_volmeter_create(OBS_FADER_CUBIC);
	obs_volmeter_attach_source(vm, src);
	obs_source_release(src);
	/* sources are destroyed on the destruction thread */
	obs_wait_for_destroy_queue();
	assert_int_equal(obs_volmeter_get_nr_channels(vm), 0);
	obs_volmeter_destroy(vm);

	obs_volmeter_destroy(NULL);
	assert_false(obs_volmeter_attach_source(NULL, NULL));
	/* Regression: these dereferenced a NULL volmeter or fader */
	obs_volmeter_set_peak_meter_type(NULL, TRUE_PEAK_METER);
	assert_int_equal(obs_volmeter_get_nr_channels(NULL), 0);
	assert_null(obs_fader_db_to_def(NULL));

	core_stop();
}

static void mix_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_start();
	register_audio_output();

	obs_source_t *src = obs_source_create("fake_async", "src", NULL, NULL);
	obs_set_output_source(0, src);
	obs_output_t *out = obs_output_create("fake_audio_output", "audio", NULL, NULL);
	obs_output_set_media(out, NULL, obs_get_audio());
	assert_true(obs_output_start(out));
	struct audio_output *ao = obs_obj_get_data(out);

	/* the mix carries the source at its volume */
	obs_source_set_volume(src, 0.5f);
	for (int i = 0; i < 10 && ao->peak < 0.2f; i++) {
		push_audio(src, 10);
		os_sleep_ms(100);
	}
	assert_true(near(ao->peak, 0.25f, 0.02f));

	/* muted sources are silent */
	obs_source_set_muted(src, true);
	os_sleep_ms(200);
	ao->peak = 0.0f;
	push_audio(src, 10);
	os_sleep_ms(200);
	assert_true(ao->peak < 0.01f);
	obs_source_set_muted(src, false);

	/* sources that are not on mixer 1 are not in it */
	obs_source_set_audio_mixers(src, 0x2);
	os_sleep_ms(100);
	ao->peak = 0.0f;
	push_audio(src, 10);
	os_sleep_ms(200);
	assert_true(ao->peak < 0.01f);
	obs_source_set_audio_mixers(src, 0x3f);

	/* the monitoring mix reaches outputs that ask for it; the device is one that does not exist, so
	 * nothing plays */
	assert_true(obs_set_audio_monitoring_device("none", "no-such-device"));
	const char *name, *id;
	obs_get_audio_monitoring_device(&name, &id);
	assert_string_equal(id, "no-such-device");
	obs_source_set_monitoring_enabled(src, true);
	assert_true(obs_source_get_monitoring_enabled(src));
	push_audio(src, 20);
	assert_true(wait_count(&ao->monitor_mixes, 1));
	obs_source_set_monitoring_enabled(src, false);
	assert_false(obs_source_get_monitoring_enabled(src));
	obs_reset_audio_monitoring();

	/* sync offsets and balance */
	obs_source_set_sync_offset(src, 50000000);
	obs_source_set_balance_value(src, 1.0f);
	push_audio(src, 10);
	os_sleep_ms(100);
	obs_source_set_sync_offset(src, 0);
	obs_source_set_balance_value(src, 0.5f);

	obs_output_stop(out);
	for (int i = 0; i < 400 && obs_output_active(out); i++)
		os_sleep_ms(5);
	assert_true(ao->mixes > 0);
	obs_output_release(out);
	obs_set_output_source(0, NULL);
	obs_source_release(src);
	core_stop();
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(conversion_test),
		cmocka_unit_test(fader_test),
		cmocka_unit_test(volmeter_test),
		cmocka_unit_test(mix_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
