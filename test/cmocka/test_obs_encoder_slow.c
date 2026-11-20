#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-core-test.h"

#include <stdio.h>
#include <stdlib.h>

#include <util/threading.h>

/* Encoders that fall behind, or stop returning for a while, must not freeze the session: rendering goes on, the
 * API calls a frontend makes every second return, and stopping, releasing and shutting down finish once the
 * encoder returns. Every step runs under a watchdog that names the stuck step and aborts instead of hanging. */

/* ------------------------------------------------------------------------- */
/* watchdog */

static pthread_t watchdog_thread;
static os_event_t *watchdog_quit;
static uint64_t watchdog_base;
static volatile long watchdog_deadline; /* ms after watchdog_base, 0 = idle */

static long watchdog_now(void)
{
	return (long)((os_gettime_ns() - watchdog_base) / 1000000ULL) + 1;
}
static const char *volatile watchdog_step;

static void *watchdog_main(void *param)
{
	UNUSED_PARAMETER(param);
	while (os_event_timedwait(watchdog_quit, 20) == ETIMEDOUT) {
		long deadline = os_atomic_load_long(&watchdog_deadline);
		if (deadline && watchdog_now() > deadline) {
			fprintf(stderr, "watchdog: stuck in \"%s\"\n", watchdog_step);
			fflush(stderr);
			abort();
		}
	}
	return NULL;
}

/* the event outlives every test, so core_stop()'s leak check does not count it */
static void watchdog_start(void)
{
	watchdog_base = os_gettime_ns();
	if (!watchdog_quit)
		assert_int_equal(os_event_init(&watchdog_quit, OS_EVENT_TYPE_MANUAL), 0);
	os_event_reset(watchdog_quit);
	os_atomic_set_long(&watchdog_deadline, 0);
	assert_int_equal(pthread_create(&watchdog_thread, NULL, watchdog_main, NULL), 0);
}

static void watchdog_stop(void)
{
	os_event_signal(watchdog_quit);
	pthread_join(watchdog_thread, NULL);
}

/* the following code must finish within ms */
static void step(const char *name, uint32_t ms)
{
	watchdog_step = name;
	os_atomic_set_long(&watchdog_deadline, watchdog_now() + (long)ms);
}

static void step_done(void)
{
	os_atomic_set_long(&watchdog_deadline, 0);
}

/* ------------------------------------------------------------------------- */
/* slow encoders: every encode call sleeps slow_ms, and while blocked is set, waits until it is cleared */

static volatile long slow_ms, audio_slow_ms;
static volatile bool blocked, audio_blocked;
static volatile long in_encode, in_audio_encode;
static volatile long video_encodes, texture_encodes, audio_encodes;

struct slow_encoder {
	obs_encoder_t *encoder;
	int64_t frames;
	uint8_t packet[16];
};

static void *slow_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	UNUSED_PARAMETER(settings);
	struct slow_encoder *se = bzalloc(sizeof(*se));
	se->encoder = encoder;
	return se;
}

static void slow_destroy(void *data)
{
	bfree(data);
}

static void be_slow(volatile bool *block, volatile long *ms, volatile long *inside)
{
	os_atomic_inc_long(inside);
	while (os_atomic_load_bool(block))
		os_sleep_ms(1);
	long wait = os_atomic_load_long(ms);
	if (wait)
		os_sleep_ms((uint32_t)wait);
	os_atomic_dec_long(inside);
}

static void make_video_packet(struct slow_encoder *se, int64_t pts, struct encoder_packet *packet)
{
	static const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0x88, 0x84, 0x10};
	static const uint8_t slice[] = {0, 0, 0, 1, 0x41, 0x9a, 0x02, 0x10};
	bool key = se->frames % 30 == 0;
	memcpy(se->packet, key ? idr : slice, sizeof(idr));
	packet->data = se->packet;
	packet->size = sizeof(idr);
	packet->pts = pts;
	packet->dts = pts;
	packet->keyframe = key;
	packet->type = OBS_ENCODER_VIDEO;
	se->frames++;
}

static bool slow_video_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet,
			      bool *received_packet)
{
	be_slow(&blocked, &slow_ms, &in_encode);
	make_video_packet(data, frame->pts, packet);
	*received_packet = true;
	os_atomic_inc_long(&video_encodes);
	return true;
}

static bool slow_texture_encode(void *data, struct encoder_texture *texture, int64_t pts, uint64_t lock_key,
				uint64_t *next_key, struct encoder_packet *packet, bool *received_packet)
{
	UNUSED_PARAMETER(texture);
	UNUSED_PARAMETER(lock_key);
	UNUSED_PARAMETER(next_key);
	be_slow(&blocked, &slow_ms, &in_encode);
	make_video_packet(data, pts, packet);
	*received_packet = true;
	os_atomic_inc_long(&texture_encodes);
	return true;
}

static bool slow_audio_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet,
			      bool *received_packet)
{
	struct slow_encoder *se = data;
	be_slow(&audio_blocked, &audio_slow_ms, &in_audio_encode);
	memset(se->packet, 0x11, 8);
	packet->data = se->packet;
	packet->size = 8;
	packet->pts = frame->pts;
	packet->dts = frame->pts;
	packet->type = OBS_ENCODER_AUDIO;
	*received_packet = true;
	os_atomic_inc_long(&audio_encodes);
	return true;
}

static size_t slow_audio_frame_size(void *data)
{
	UNUSED_PARAMETER(data);
	return 1024;
}

static bool slow_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	static uint8_t header[] = {0x01, 0x64, 0x00, 0x1f, 0xff};
	UNUSED_PARAMETER(data);
	*extra_data = header;
	*size = sizeof(header);
	return true;
}

static void slow_video_info(void *data, struct video_scale_info *info)
{
	UNUSED_PARAMETER(data);
	info->format = VIDEO_FORMAT_NV12;
}

static void slow_audio_info(void *data, struct audio_convert_info *info)
{
	UNUSED_PARAMETER(data);
	info->format = AUDIO_FORMAT_FLOAT_PLANAR;
}

/* ------------------------------------------------------------------------- */
/* an encoded output that counts packets */

struct slow_output {
	obs_output_t *output;
	volatile long video_packets, audio_packets;
};

static void *output_create(obs_data_t *settings, obs_output_t *output)
{
	UNUSED_PARAMETER(settings);
	struct slow_output *so = bzalloc(sizeof(*so));
	so->output = output;
	return so;
}

static void output_destroy(void *data)
{
	bfree(data);
}

static bool output_start(void *data)
{
	struct slow_output *so = data;
	if (!obs_output_can_begin_data_capture(so->output, 0))
		return false;
	if (!obs_output_initialize_encoders(so->output, 0))
		return false;
	return obs_output_begin_data_capture(so->output, 0);
}

static void output_stop(void *data, uint64_t ts)
{
	UNUSED_PARAMETER(ts);
	struct slow_output *so = data;
	obs_output_end_data_capture(so->output);
}

static void output_packet(void *data, struct encoder_packet *packet)
{
	struct slow_output *so = data;
	if (!packet)
		return;
	if (packet->type == OBS_ENCODER_VIDEO)
		os_atomic_inc_long(&so->video_packets);
	else
		os_atomic_inc_long(&so->audio_packets);
}

static uint64_t output_total_bytes(void *data)
{
	struct slow_output *so = data;
	return (uint64_t)(so->video_packets + so->audio_packets) * 8;
}

static void register_slow_types(void)
{
	struct obs_encoder_info venc = {
		.id = "slow_video_enc",
		.type = OBS_ENCODER_VIDEO,
		.codec = "h264",
		.get_name = fake_get_name,
		.create = slow_create,
		.destroy = slow_destroy,
		.encode = slow_video_encode,
		.get_extra_data = slow_extra_data,
		.get_video_info = slow_video_info,
	};
	obs_register_encoder(&venc);

	struct obs_encoder_info tenc = venc;
	tenc.id = "slow_texture_enc";
	tenc.encode_texture2 = slow_texture_encode;
	tenc.caps = OBS_ENCODER_CAP_PASS_TEXTURE;
	obs_register_encoder(&tenc);

	struct obs_encoder_info aenc = {
		.id = "slow_audio_enc",
		.type = OBS_ENCODER_AUDIO,
		.codec = "aac",
		.get_name = fake_get_name,
		.create = slow_create,
		.destroy = slow_destroy,
		.encode = slow_audio_encode,
		.get_frame_size = slow_audio_frame_size,
		.get_audio_info = slow_audio_info,
	};
	obs_register_encoder(&aenc);

	struct obs_output_info out = {
		.id = "slow_output",
		.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED,
		.get_name = fake_get_name,
		.create = output_create,
		.destroy = output_destroy,
		.start = output_start,
		.stop = output_stop,
		.encoded_packet = output_packet,
		.get_total_bytes = output_total_bytes,
		.encoded_video_codecs = "h264",
		.encoded_audio_codecs = "aac",
	};
	obs_register_output(&out);
}

/* ------------------------------------------------------------------------- */
/* a session with one output, its encoders and a source */

struct session {
	obs_source_t *source;
	obs_output_t *output;
	obs_encoder_t *venc, *aenc;
	struct slow_output *so;
};

static void session_start(struct session *s, const char *video_encoder)
{
	os_atomic_set_long(&slow_ms, 0);
	os_atomic_set_long(&audio_slow_ms, 0);
	os_atomic_set_bool(&blocked, false);
	os_atomic_set_bool(&audio_blocked, false);
	os_atomic_set_long(&video_encodes, 0);
	os_atomic_set_long(&texture_encodes, 0);
	os_atomic_set_long(&audio_encodes, 0);

	core_start();
	register_slow_types();

	s->source = obs_source_create("fake_input", "src", NULL, NULL);
	obs_set_output_source(0, s->source);
	s->output = obs_output_create("slow_output", "out", NULL, NULL);
	s->venc = obs_video_encoder_create(video_encoder, "venc", NULL, NULL);
	s->aenc = obs_audio_encoder_create("slow_audio_enc", "aenc", NULL, 0, NULL);
	obs_encoder_set_video(s->venc, obs_get_video());
	obs_encoder_set_audio(s->aenc, obs_get_audio());
	obs_output_set_video_encoder(s->output, s->venc);
	obs_output_set_audio_encoder(s->output, s->aenc, 0);
	s->so = obs_obj_get_data(s->output);
}

static void session_stop(struct session *s)
{
	step("release objects", 5000);
	obs_output_release(s->output);
	obs_encoder_release(s->venc);
	obs_encoder_release(s->aenc);
	obs_set_output_source(0, NULL);
	obs_source_release(s->source);
	step_done();

	step("obs_shutdown", 10000);
	core_stop();
	step_done();
}

static bool wait_until(volatile long *counter, long count, uint32_t ms)
{
	for (uint32_t i = 0; i < ms / 5 && os_atomic_load_long(counter) < count; i++)
		os_sleep_ms(5);
	return os_atomic_load_long(counter) >= count;
}

static bool wait_inside(volatile long *inside, uint32_t ms)
{
	for (uint32_t i = 0; i < ms / 5 && !os_atomic_load_long(inside); i++)
		os_sleep_ms(5);
	return os_atomic_load_long(inside) != 0;
}

static bool wait_inactive(obs_output_t *output, uint32_t ms)
{
	for (uint32_t i = 0; i < ms / 5 && obs_output_active(output); i++)
		os_sleep_ms(5);
	return !obs_output_active(output);
}

/* what a frontend polls every second for its stats display, each call must return right away */
static void poll_stats(struct session *s)
{
	step("stats calls", 1000);
	(void)obs_output_get_total_frames(s->output);
	(void)obs_output_get_frames_dropped(s->output);
	(void)obs_output_get_total_bytes(s->output);
	(void)obs_output_get_connect_time_ms(s->output);
	(void)obs_output_get_congestion(s->output);
	(void)obs_output_active(s->output);
	(void)obs_output_reconnecting(s->output);
	(void)obs_get_lagged_frames();
	(void)obs_get_total_frames();
	(void)obs_get_active_fps();
	(void)obs_get_average_frame_time_ns();
	(void)video_output_get_skipped_frames(obs_get_video());
	(void)video_output_get_total_frames(obs_get_video());
	(void)obs_encoder_active(s->venc);
	(void)obs_encoder_get_encoded_frames(s->venc);
	(void)obs_encoder_paused(s->venc);
	step_done();
}

/* rendering keeps going: at least frames more frames within ms while the main thread polls */
static void expect_rendering(struct session *s, uint32_t frames, uint32_t ms)
{
	uint64_t start_frames = obs_get_total_frames();
	uint64_t start = os_gettime_ns();
	while (os_gettime_ns() - start < (uint64_t)ms * 1000000ULL) {
		poll_stats(s);

		step("create and release a source", 1000);
		obs_source_t *tmp = obs_source_create("fake_input", "tmp", NULL, NULL);
		obs_source_release(tmp);
		step_done();

		os_sleep_ms(50);
	}
	assert_true(obs_get_total_frames() - start_frames >= frames);
}

/* clears *flag after ms on another thread; join the thread with pthread_join() */
struct unblock {
	volatile bool *flag;
	uint32_t ms;
	pthread_t thread;
};

static void *unblock_main(void *param)
{
	struct unblock *u = param;
	os_sleep_ms(u->ms);
	os_atomic_set_bool(u->flag, false);
	return NULL;
}

static void unblock_after(struct unblock *u, volatile bool *flag, uint32_t ms)
{
	u->flag = flag;
	u->ms = ms;
	assert_int_equal(pthread_create(&u->thread, NULL, unblock_main, u), 0);
}

/* ------------------------------------------------------------------------- */

/* an encoder three times slower than real time, stopped and started again while it stays that slow. Regression:
 * with a slow CPU encoder, the video thread never let go of its input lock long enough for the output's stop
 * thread to disconnect the encoder, so the output never stopped, and the next start, release or shutdown waited
 * for it forever. */
static void steady_slow(const char *encoder, volatile long *encodes, volatile long *ms)
{
	struct session s;
	session_start(&s, encoder);
	watchdog_start();

	step("obs_output_start", 5000);
	assert_true(obs_output_start(s.output));
	step_done();
	assert_true(wait_until(&s.so->video_packets, 10, 5000));

	/* about three times the frame time */
	os_atomic_set_long(ms, 50);
	long before = os_atomic_load_long(encodes);
	expect_rendering(&s, 60, 2000);
	long done = os_atomic_load_long(encodes) - before;
	/* the encoder only gets the frames it can keep up with */
	assert_true(done > 10);
	assert_true(done < 80);

	step("obs_output_stop", 5000);
	obs_output_stop(s.output);
	assert_true(wait_inactive(s.output, 5000));
	step_done();

	/* starts again while still slow */
	step("restart while slow", 5000);
	assert_true(obs_output_start(s.output));
	step_done();
	assert_true(wait_until(&s.so->video_packets, s.so->video_packets + 5, 5000));
	expect_rendering(&s, 30, 1000);
	step("obs_output_stop", 5000);
	obs_output_stop(s.output);
	assert_true(wait_inactive(s.output, 5000));
	step_done();

	session_stop(&s);
	watchdog_stop();
}

static void cpu_steady_slow_test(void **state)
{
	UNUSED_PARAMETER(state);
	steady_slow("slow_video_enc", &video_encodes, &slow_ms);
}

static void texture_steady_slow_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_set_env("NULL_GRAPHICS_YUV_TEXTURES", "1");
	steady_slow("slow_texture_enc", &texture_encodes, &slow_ms);
	core_set_env("NULL_GRAPHICS_YUV_TEXTURES", NULL);
}

static void audio_steady_slow_test(void **state)
{
	UNUSED_PARAMETER(state);
	steady_slow("slow_video_enc", &audio_encodes, &audio_slow_ms);
}

/* an encoder that gets slower and slower, up to a second per frame, then recovers */
static void ramp(const char *encoder, volatile long *encodes)
{
	struct session s;
	session_start(&s, encoder);
	watchdog_start();

	step("obs_output_start", 5000);
	assert_true(obs_output_start(s.output));
	step_done();
	assert_true(wait_until(&s.so->video_packets, 10, 5000));

	static const long ramp_ms[] = {10, 20, 40, 80, 160, 320, 640, 1000};
	for (size_t i = 0; i < sizeof(ramp_ms) / sizeof(ramp_ms[0]); i++) {
		os_atomic_set_long(&slow_ms, ramp_ms[i]);
		os_atomic_set_long(&audio_slow_ms, ramp_ms[i] / 2);
		expect_rendering(&s, 15, 500);
	}

	os_atomic_set_long(&slow_ms, 0);
	os_atomic_set_long(&audio_slow_ms, 0);
	long before = os_atomic_load_long(encodes);
	assert_true(wait_until(encodes, before + 30, 5000));

	step("obs_output_stop", 5000);
	obs_output_stop(s.output);
	assert_true(wait_inactive(s.output, 5000));
	step_done();

	session_stop(&s);
	watchdog_stop();
}

static void cpu_ramp_test(void **state)
{
	UNUSED_PARAMETER(state);
	ramp("slow_video_enc", &video_encodes);
}

static void texture_ramp_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_set_env("NULL_GRAPHICS_YUV_TEXTURES", "1");
	ramp("slow_texture_enc", &texture_encodes);
	core_set_env("NULL_GRAPHICS_YUV_TEXTURES", NULL);
}

/* an encoder that does not return for seconds: the session keeps running, and stopping finishes once it does */
static void stall(const char *encoder, volatile bool *block, volatile long *inside)
{
	struct session s;
	session_start(&s, encoder);
	watchdog_start();

	step("obs_output_start", 5000);
	assert_true(obs_output_start(s.output));
	step_done();
	assert_true(wait_until(&s.so->video_packets, 10, 5000));

	os_atomic_set_bool(block, true);
	assert_true(wait_inside(inside, 2000));
	expect_rendering(&s, 60, 2000);

	/* stopping while it hangs: the output may only finish stopping once the encoder returns */
	step("obs_output_stop while stalled", 2000);
	obs_output_stop(s.output);
	step_done();
	expect_rendering(&s, 30, 1000);

	os_atomic_set_bool(block, false);
	step("output stops after the encoder returns", 5000);
	assert_true(wait_inactive(s.output, 5000));
	step_done();

	/* and starts again */
	step("restart", 5000);
	assert_true(obs_output_start(s.output));
	step_done();
	assert_true(wait_until(&s.so->video_packets, s.so->video_packets + 5, 5000));

	/* stalls again, and the session shuts down once the encoder returns */
	os_atomic_set_bool(block, true);
	assert_true(wait_inside(inside, 2000));
	expect_rendering(&s, 30, 1000);
	step("obs_output_stop while stalled", 2000);
	obs_output_stop(s.output);
	step_done();
	struct unblock u;
	unblock_after(&u, block, 1000);
	step("output stops after the encoder returns", 5000);
	assert_true(wait_inactive(s.output, 5000));
	step_done();
	pthread_join(u.thread, NULL);

	session_stop(&s);
	watchdog_stop();
}

static void cpu_stall_test(void **state)
{
	UNUSED_PARAMETER(state);
	stall("slow_video_enc", &blocked, &in_encode);
}

static void texture_stall_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_set_env("NULL_GRAPHICS_YUV_TEXTURES", "1");
	stall("slow_texture_enc", &blocked, &in_encode);
	core_set_env("NULL_GRAPHICS_YUV_TEXTURES", NULL);
}

static void audio_stall_test(void **state)
{
	UNUSED_PARAMETER(state);
	stall("slow_video_enc", &audio_blocked, &in_audio_encode);
}

/* releasing everything while the encoder hangs, without stopping first */
static void release_while_stalled(const char *encoder)
{
	struct session s;
	session_start(&s, encoder);
	watchdog_start();

	step("obs_output_start", 5000);
	assert_true(obs_output_start(s.output));
	step_done();
	assert_true(wait_until(&s.so->video_packets, 10, 5000));

	os_atomic_set_bool(&blocked, true);
	assert_true(wait_inside(&in_encode, 2000));
	struct unblock u;
	unblock_after(&u, &blocked, 1500);

	step("release while stalled", 10000);
	obs_output_release(s.output);
	obs_encoder_release(s.venc);
	obs_encoder_release(s.aenc);
	step_done();
	s.output = NULL;
	s.venc = s.aenc = NULL;
	pthread_join(u.thread, NULL);

	session_stop(&s);
	watchdog_stop();
}

static void cpu_release_while_stalled_test(void **state)
{
	UNUSED_PARAMETER(state);
	release_while_stalled("slow_video_enc");
}

static void texture_release_while_stalled_test(void **state)
{
	UNUSED_PARAMETER(state);
	core_set_env("NULL_GRAPHICS_YUV_TEXTURES", "1");
	release_while_stalled("slow_texture_enc");
	core_set_env("NULL_GRAPHICS_YUV_TEXTURES", NULL);
}

static int setup(void **state)
{
	UNUSED_PARAMETER(state);
	return os_event_init(&watchdog_quit, OS_EVENT_TYPE_MANUAL);
}

static int teardown(void **state)
{
	UNUSED_PARAMETER(state);
	os_event_destroy(watchdog_quit);
	return 0;
}

/* an argument runs only the tests that match it */
int main(int argc, char **argv)
{
	if (argc > 1)
		cmocka_set_test_filter(argv[1]);

	const struct CMUnitTest tests[] = {
		cmocka_unit_test(cpu_steady_slow_test),
		cmocka_unit_test(texture_steady_slow_test),
		cmocka_unit_test(audio_steady_slow_test),
		cmocka_unit_test(cpu_ramp_test),
		cmocka_unit_test(texture_ramp_test),
		cmocka_unit_test(cpu_stall_test),
		cmocka_unit_test(texture_stall_test),
		cmocka_unit_test(audio_stall_test),
		cmocka_unit_test(cpu_release_while_stalled_test),
		cmocka_unit_test(texture_release_while_stalled_test),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
