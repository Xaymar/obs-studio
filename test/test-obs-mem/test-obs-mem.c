#undef NDEBUG
#include <assert.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <obs.h>
#include <obs-data.h>
#include <obs-mem.h>
#include <obs-missing-files.h>
#include <obs-properties.h>
#include <obs-audio-controls.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

struct test_obj {
	struct obs_mem_header hdr;
	volatile long destroyed;
	bool want_weak;
	obs_mem_weak_t *weak_in_dtor;
	void *ref_in_dtor;
	volatile long *destroy_count; /* race test only: survives the poisoning below */
	bool poison;
};

static void test_destroy(void *ptr)
{
	struct test_obj *obj = ptr;
	long n = os_atomic_inc_long(&obj->destroyed);
	assert(n == 1);

	obj->ref_in_dtor = obs_mem_get_ref(obj);
	if (obj->want_weak)
		obj->weak_in_dtor = obs_mem_get_weak(obj);

	/* The test owns the memory so it can inspect the object afterwards. */
	obs_mem_finalize(obj);

	if (obj->poison) {
		/* Stands in for freed memory: a positive refs lets a late upgrade "succeed" so it gets detected. */
		os_atomic_inc_long(obj->destroy_count);
		memset(obj, 0x11, sizeof(*obj));
	}
}

static struct test_obj *test_create(void)
{
	struct test_obj *obj = bzalloc(sizeof(*obj));
	obs_mem_init(obj, test_destroy);
	return obj;
}

static void test_header_layout(void)
{
	assert(sizeof(struct obs_mem_header) == 32);
	assert(offsetof(struct test_obj, hdr) == 0);
	/* bmalloc aligns to 32; brealloc may not, so the header itself does not require it */
	assert(((uintptr_t)test_create() & 31) == 0);
}

static void test_counting(void)
{
	struct test_obj *obj = test_create();

	assert(obj->hdr.refs == 1);
	assert(obs_mem_acquire(obj) == obj);
	assert(!obs_mem_release(obj));
	assert(obj->destroyed == 0);
	assert(obs_mem_release(obj));
	assert(obj->destroyed == 1);
	assert(!obs_mem_release(NULL));
	assert(obs_mem_acquire(NULL) == NULL);
	assert(obs_mem_get_ref(NULL) == NULL);

	bfree(obj);
}

static void test_get_ref_dying(void)
{
	struct test_obj *obj = test_create();

	assert(obs_mem_get_ref(obj) == obj);
	assert(!obs_mem_release(obj));
	assert(obs_mem_release(obj));
	assert(obj->destroyed == 1);
	assert(obj->ref_in_dtor == NULL);

	bfree(obj);
}

static void test_weak_basic(void)
{
	struct test_obj *obj = test_create();
	obs_mem_weak_t *a = obs_mem_get_weak(obj);
	obs_mem_weak_t *b = obs_mem_get_weak(obj);

	assert(a && a == b);
	assert(!obs_mem_weak_expired(a));
	assert(obs_mem_weak_references(a, obj));

	void *strong = obs_mem_weak_get_strong(a);
	assert(strong == obj);
	assert(obj->hdr.refs == 2);
	assert(!obs_mem_release(strong));

	assert(obs_mem_release(obj));
	assert(obs_mem_weak_expired(a));
	assert(obs_mem_weak_get_strong(a) == NULL);
	assert(obs_mem_weak_references(a, obj));
	assert(obs_mem_weak_expired(NULL));

	/* One reference per get_weak call; finalize already dropped the object's own. */
	obs_mem_weak_release(a);
	obs_mem_weak_release(b);
	bfree(obj);
}

static void test_get_weak_while_dying(void)
{
	struct test_obj *obj = test_create();
	obj->want_weak = true;

	assert(obs_mem_release(obj));
	assert(obj->weak_in_dtor);
	assert(obs_mem_weak_expired(obj->weak_in_dtor));
	assert(obs_mem_weak_get_strong(obj->weak_in_dtor) == NULL);
	obs_mem_weak_release(obj->weak_in_dtor);

	bfree(obj);
}

static void test_finalize_forced(void)
{
	struct test_obj *obj = test_create();
	obs_mem_weak_t *weak = obs_mem_get_weak(obj);

	assert(obj->hdr.refs == 1);
	obs_mem_finalize(obj);
	assert(obs_mem_weak_expired(weak));
	assert(obs_mem_weak_get_strong(weak) == NULL);

	obs_mem_weak_release(weak);
	bfree(obj);
}

#define RACE_THREADS 4
#define RACE_ITERATIONS 100000

static volatile long race_upgraded;
static volatile long race_missed;

struct race_ctx {
	volatile long destroy_count;
	/* A creates the object, B tries to upgrade; the flags hand it between the two. */
	void *volatile shared_weak;
	volatile long ready;
	volatile long done;
	volatile long stop;
	volatile long bad;
};

static void *race_thread_b(void *param)
{
	struct race_ctx *ctx = param;

	while (!os_atomic_load_long(&ctx->stop)) {
		if (!os_atomic_load_long(&ctx->ready))
			continue;

		obs_mem_weak_t *weak = ctx->shared_weak;
		struct test_obj *obj = obs_mem_weak_get_strong(weak);
		if (obj) {
			/* Holding a ref means the destroy callback can't have run; if it did, the pin failed. */
			if (os_atomic_load_long(&ctx->destroy_count))
				os_atomic_inc_long(&ctx->bad);
			os_atomic_inc_long(&race_upgraded);
			obs_mem_release(obj);
		} else {
			os_atomic_inc_long(&race_missed);
		}

		os_atomic_store_long(&ctx->ready, 0);
		os_atomic_store_long(&ctx->done, 1);
	}
	return NULL;
}

static void *race_thread_a(void *param)
{
	(void)param;
	struct race_ctx ctx = {0};
	pthread_t b;
	pthread_create(&b, NULL, race_thread_b, &ctx);

	for (int i = 0; i < RACE_ITERATIONS; i++) {
		struct test_obj *obj = test_create();
		obj->poison = true;
		obj->destroy_count = &ctx.destroy_count;
		os_atomic_store_long(&ctx.destroy_count, 0);
		obs_mem_weak_t *weak = obs_mem_get_weak(obj);

		ctx.shared_weak = weak;
		os_atomic_store_long(&ctx.done, 0);
		os_atomic_store_long(&ctx.ready, 1);

		/* Vary the delay so the release lands at different points of B's upgrade. */
		for (volatile int k = 0; k < (i * 7) % 200; k++)
			;
		obs_mem_release(obj);

		while (!os_atomic_load_long(&ctx.done))
			;

		assert(ctx.destroy_count == 1);
		assert(obs_mem_weak_expired(weak));
		obs_mem_weak_release(weak);
		bfree(obj);
	}

	os_atomic_store_long(&ctx.stop, 1);
	pthread_join(b, NULL);

	assert(ctx.bad == 0);
	return NULL;
}

static void test_upgrade_race(void)
{
	pthread_t threads[RACE_THREADS / 2];

	for (size_t i = 0; i < RACE_THREADS / 2; i++)
		pthread_create(&threads[i], NULL, race_thread_a, NULL);
	for (size_t i = 0; i < RACE_THREADS / 2; i++)
		pthread_join(threads[i], NULL);

	printf("upgrade race: %ld upgraded, %ld expired\n", race_upgraded, race_missed);
	assert(race_upgraded > 0 && race_missed > 0);
}

static void test_data_item_release_quirk(void)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_int(d, "a", 1);

	obs_data_item_t *item = obs_data_item_byname(d, "a");
	assert(item);
	obs_data_item_release(&item); /* data still holds a ref: not destroyed, pointer kept */
	assert(item != NULL);

	/* remove() detaches the item and drops both the caller's and the data's reference (it used to keep
	 * the data's, which leaked): the item is destroyed and the pointer cleared. `item` no longer owns a
	 * reference, so it is not released again. */
	obs_data_item_t *second = obs_data_item_byname(d, "a");
	assert(second == item);
	obs_data_item_remove(&second);
	assert(second == NULL);
	assert(obs_data_item_byname(d, "a") == NULL);

	obs_data_release(d);
}

static void test_data_item_next(void)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_int(d, "a", 1);
	obs_data_set_int(d, "b", 2);
	obs_data_set_int(d, "c", 3);

	int n = 0;
	obs_data_item_t *item = obs_data_first(d);
	while (item) {
		n++;
		if (!obs_data_item_next(&item))
			break;
	}
	assert(n == 3);
	assert(item == NULL);

	obs_data_release(d);
}

static void test_data_array_refs(void)
{
	obs_data_array_t *a = obs_data_array_create();
	obs_data_t *d = obs_data_create();
	obs_data_set_int(d, "v", 42);
	obs_data_array_push_back(a, d); /* the array takes its own ref */

	obs_data_array_addref(a);
	obs_data_array_release(a);
	obs_data_array_release(a);

	/* Array destroyed: the element survives on our ref alone. */
	assert(obs_data_get_int(d, "v") == 42);
	obs_data_release(d);
}

static void test_missing_file_refs(void)
{
	obs_missing_files_t *a = obs_missing_files_create();
	obs_missing_files_t *b = obs_missing_files_create();
	obs_missing_file_t *file = obs_missing_file_create("some/path", NULL, OBS_MISSING_FILE_SCRIPT, NULL, NULL);

	obs_missing_files_add_file(a, file); /* a owns the creation ref */
	obs_missing_files_append(b, a);      /* b takes its own ref */
	obs_mem_acquire(file);               /* ours, to observe the file after both lists are gone */

	obs_missing_files_destroy(a);
	obs_missing_files_destroy(b);

	assert(strcmp(obs_missing_file_get_path(file), "some/path") == 0);
	obs_missing_file_release(file);
}

static volatile long destroy_signal_ran;

static void on_source_destroy(void *param, calldata_t *cd)
{
	obs_source_t *src = calldata_ptr(cd, "source");
	obs_weak_source_t *w = obs_source_get_weak_source(src);
	assert(obs_weak_source_expired(w));
	assert(obs_weak_source_get_source(w) == NULL);
	obs_weak_source_release(w);
	os_atomic_inc_long(&destroy_signal_ran);
	(void)param;
}

/* obs_wait_for_destroy_queue is a no-op without video, so poll for the deferred destroy instead. */
static void wait_for(volatile long *flag)
{
	for (int i = 0; i < 5000 && !os_atomic_load_long(flag); i++)
		os_sleep_ms(1);
}

static void test_source_weak_in_destroy_signal(void)
{
	obs_scene_t *s = obs_scene_create_private("t");
	assert(s);
	signal_handler_connect(obs_source_get_signal_handler(obs_scene_get_source(s)), "destroy", on_source_destroy,
			       NULL);

	obs_scene_release(s);
	obs_wait_for_destroy_queue(); /* no-op without video, hence wait_for */
	wait_for(&destroy_signal_ran);
	assert(os_atomic_load_long(&destroy_signal_ran) == 1);
}

static void test_source_weak_basic(void)
{
	obs_scene_t *s = obs_scene_create_private("w");
	obs_source_t *src = obs_scene_get_source(s);
	obs_weak_source_t *w = obs_source_get_weak_source(src);
	assert(!obs_weak_source_expired(w));
	assert(obs_weak_source_references_source(w, src));

	obs_source_t *strong = obs_weak_source_get_source(w);
	assert(strong == src);
	obs_source_release(strong);

	obs_scene_release(s);
	obs_wait_for_destroy_queue(); /* no-op without video; expiry happens on the last release */
	assert(obs_weak_source_expired(w));
	assert(obs_weak_source_get_source(w) == NULL);
	assert(obs_weak_source_references_source(w, src));
	obs_weak_source_release(w);
}

static void test_canvas_weak_basic(void)
{
	obs_canvas_t *canvas = obs_canvas_create_private("weak", NULL, 0);
	obs_canvas_t *other = obs_canvas_create_private("other", NULL, 0);
	assert(canvas && other);

	obs_weak_canvas_t *w = obs_canvas_get_weak_canvas(canvas);
	assert(obs_weak_canvas_references_canvas(w, canvas));
	assert(!obs_weak_canvas_references_canvas(w, other));
	assert(!obs_weak_canvas_references_canvas(w, NULL));
	assert(!obs_weak_canvas_references_canvas(NULL, canvas));

	obs_canvas_t *strong = obs_weak_canvas_get_canvas(w);
	assert(strong == canvas);
	obs_canvas_release(strong);

	obs_canvas_release(other);
	obs_canvas_release(canvas);
	obs_wait_for_destroy_queue(); /* no-op without video; expiry happens on the last release */
	assert(obs_weak_canvas_get_canvas(w) == NULL);
	obs_weak_canvas_release(w);
}

static void test_sceneitem_survives_remove(void)
{
	obs_scene_t *a = obs_scene_create_private("a");
	obs_scene_t *b = obs_scene_create_private("b");
	obs_source_t *b_src = obs_scene_get_source(b);

	obs_sceneitem_t *item = obs_scene_add(a, b_src);
	assert(item);
	obs_sceneitem_addref(item);
	obs_sceneitem_remove(item);

	assert(obs_sceneitem_get_source(item) == b_src);
	assert(obs_sceneitem_get_id(item) > 0);
	obs_sceneitem_release(item);

	obs_scene_release(b);
	obs_scene_release(a);
	obs_wait_for_destroy_queue(); /* no-op without video; nothing here needs the deferred free */
}

static obs_fader_t *cb_fader;
static volatile long fader_cb_ran;

static void fader_release_cb(void *param, float db)
{
	if (os_atomic_inc_long(&fader_cb_ran) == 1) {
		obs_fader_destroy(cb_fader); /* the creator's ref, dropped mid-dispatch */
		obs_mem_release(cb_fader);   /* the other holder's: only the dispatch's temporary ref is left */
	}
	(void)param;
	(void)db;
}

static void test_fader_release_in_callback(void)
{
	/* obs_fader_set_db ignores its own volume signal, so trigger through the attached source. */
	obs_scene_t *s = obs_scene_create_private("fader");
	obs_source_t *src = obs_scene_get_source(s);

	cb_fader = obs_fader_create(OBS_FADER_LOG);
	assert(obs_fader_attach_source(cb_fader, src));
	obs_mem_acquire(cb_fader);
	obs_fader_add_callback(cb_fader, fader_release_cb, NULL);

	obs_source_set_volume(src, 0.5f);
	assert(os_atomic_load_long(&fader_cb_ran) == 1);
	obs_source_set_volume(src, 0.25f); /* freed and disconnected */
	assert(os_atomic_load_long(&fader_cb_ran) == 1);

	obs_scene_release(s);
}

static volatile long fader_count_ran;

static void fader_count_cb(void *param, float db)
{
	os_atomic_inc_long(&fader_count_ran);
	(void)param;
	(void)db;
}

static void test_fader_destroy_detaches(void)
{
	obs_scene_t *s = obs_scene_create_private("fader2");
	obs_source_t *src = obs_scene_get_source(s);

	obs_fader_t *f = obs_fader_create(OBS_FADER_LOG);
	assert(obs_fader_attach_source(f, src));
	obs_mem_acquire(f); /* another holder */
	obs_fader_add_callback(f, fader_count_cb, NULL);

	obs_source_set_volume(src, 0.5f);
	assert(os_atomic_load_long(&fader_count_ran) == 1);

	obs_fader_destroy(f); /* the creator's destroy stops callbacks even though f lives on */
	obs_source_set_volume(src, 0.25f);
	assert(os_atomic_load_long(&fader_count_ran) == 1);

	obs_mem_release(f);
	obs_scene_release(s);
}

static void test_properties_group_outlives_parent(void)
{
	obs_properties_t *p = obs_properties_create();
	obs_properties_t *g = obs_properties_create();
	obs_properties_add_bool(g, "b", "B");
	assert(obs_properties_add_group(p, "g", "G", OBS_GROUP_NORMAL, g));

	obs_mem_acquire(g);
	obs_properties_destroy(p);

	assert(obs_properties_get_parent(g) == NULL);
	obs_property_t *first = obs_properties_first(g);
	assert(first && strcmp(obs_property_name(first), "b") == 0);
	obs_properties_destroy(g);
}

static const char *audio_src_name(void *type_data)
{
	(void)type_data;
	return "test audio";
}

static void *audio_src_create(obs_data_t *settings, obs_source_t *source)
{
	(void)settings;
	return source;
}

static void audio_src_destroy(void *data)
{
	(void)data;
}

static obs_volmeter_t *cb_volmeter;
static volatile long volmeter_cb_ran;

static void volmeter_release_cb(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
				const float peak[MAX_AUDIO_CHANNELS], const float input_peak[MAX_AUDIO_CHANNELS])
{
	/* the destroy detaches from the source while its audio callback lock is held */
	if (os_atomic_inc_long(&volmeter_cb_ran) == 1) {
		obs_volmeter_destroy(cb_volmeter);
		obs_mem_release(cb_volmeter); /* only the dispatch's temporary ref is left */
	}
	(void)param;
	(void)magnitude;
	(void)peak;
	(void)input_peak;
}

static volatile long volmeter_count_ran;

static void volmeter_count_cb(void *param, const float magnitude[MAX_AUDIO_CHANNELS],
			      const float peak[MAX_AUDIO_CHANNELS], const float input_peak[MAX_AUDIO_CHANNELS])
{
	os_atomic_inc_long(&volmeter_count_ran);
	(void)param;
	(void)magnitude;
	(void)peak;
	(void)input_peak;
}

static long audio_a_ran, audio_b_ran, audio_c_ran;

static void audio_a_cb(void *param, obs_source_t *source, const struct audio_data *data, bool muted)
{
	audio_a_ran++;
	(void)param;
	(void)source;
	(void)data;
	(void)muted;
}

static void audio_b_cb(void *param, obs_source_t *source, const struct audio_data *data, bool muted)
{
	if (audio_b_ran++ == 0)
		obs_source_remove_audio_capture_callback(source, audio_a_cb, NULL); /* a lower entry */
	(void)param;
	(void)data;
	(void)muted;
}

static void audio_c_cb(void *param, obs_source_t *source, const struct audio_data *data, bool muted)
{
	audio_c_ran++;
	(void)param;
	(void)source;
	(void)data;
	(void)muted;
}

static void output_test_audio(obs_source_t *src)
{
	static float samples[2][480];
	struct obs_source_audio audio = {
		.data = {(uint8_t *)samples[0], (uint8_t *)samples[1]},
		.frames = 480,
		.speakers = SPEAKERS_STEREO,
		.format = AUDIO_FORMAT_FLOAT_PLANAR,
		.samples_per_sec = 48000,
		.timestamp = os_gettime_ns(),
	};
	obs_source_output_audio(src, &audio);
}

static void test_volmeter_release_in_callback(void)
{
	struct obs_audio_info ai = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
	assert(obs_reset_audio(&ai));

	struct obs_source_info info = {
		.id = "test_obs_mem_audio",
		.type = OBS_SOURCE_TYPE_INPUT,
		.output_flags = OBS_SOURCE_AUDIO,
		.get_name = audio_src_name,
		.create = audio_src_create,
		.destroy = audio_src_destroy,
	};
	obs_register_source(&info);

	obs_source_t *src = obs_source_create_private("test_obs_mem_audio", "audio", NULL);
	assert(src);

	cb_volmeter = obs_volmeter_create(OBS_FADER_LOG);
	assert(obs_volmeter_attach_source(cb_volmeter, src));
	obs_mem_acquire(cb_volmeter);
	obs_volmeter_add_callback(cb_volmeter, volmeter_release_cb, NULL);

	output_test_audio(src);
	assert(os_atomic_load_long(&volmeter_cb_ran) == 1);
	output_test_audio(src); /* freed and detached */
	assert(os_atomic_load_long(&volmeter_cb_ran) == 1);

	/* The creator's destroy stops callbacks even though another holder keeps the volmeter alive. */
	obs_volmeter_t *v = obs_volmeter_create(OBS_FADER_LOG);
	assert(obs_volmeter_attach_source(v, src));
	obs_mem_acquire(v);
	obs_volmeter_add_callback(v, volmeter_count_cb, NULL);
	output_test_audio(src);
	assert(os_atomic_load_long(&volmeter_count_ran) == 1);
	obs_volmeter_destroy(v);
	output_test_audio(src);
	assert(os_atomic_load_long(&volmeter_count_ran) == 1);
	obs_mem_release(v);

	/* Dispatch runs top-down; b removing the lower a must not make b run twice. */
	obs_source_add_audio_capture_callback(src, audio_a_cb, NULL);
	obs_source_add_audio_capture_callback(src, audio_b_cb, NULL);
	obs_source_add_audio_capture_callback(src, audio_c_cb, NULL);
	output_test_audio(src);
	assert(audio_c_ran == 1 && audio_b_ran == 1 && audio_a_ran == 0);
	output_test_audio(src);
	assert(audio_c_ran == 2 && audio_b_ran == 2 && audio_a_ran == 0);
	obs_source_remove_audio_capture_callback(src, audio_b_cb, NULL);
	obs_source_remove_audio_capture_callback(src, audio_c_cb, NULL);

	obs_source_release(src);
}

/* Without video, libobs logs graphics noise at warning level; show everything else from warnings up. */
static void warnings_log(int lvl, const char *msg, va_list args, void *param)
{
	if (lvl <= LOG_WARNING && strncmp(msg, "gs_", 3) != 0) {
		vfprintf(stderr, msg, args);
		fputc('\n', stderr);
	}
	(void)param;
}

static void test_context_objects(void)
{
	base_set_log_handler(warnings_log, NULL);
	assert(obs_startup("en-US", NULL, NULL));
	test_source_weak_in_destroy_signal();
	test_source_weak_basic();
	test_canvas_weak_basic();
	test_sceneitem_survives_remove();
	test_fader_release_in_callback();
	test_fader_destroy_detaches();
	test_properties_group_outlives_parent();
	test_volmeter_release_in_callback();
	obs_wait_for_destroy_queue(); /* no-op without video; the tests poll where they must */
	obs_shutdown();
	printf("test-obs-mem: %ld allocations left after shutdown\n", bnum_allocs());
}

int main(void)
{
	test_header_layout();
	test_counting();
	test_get_ref_dying();
	test_weak_basic();
	test_get_weak_while_dying();
	test_finalize_forced();
	test_upgrade_race();
	test_data_item_release_quirk();
	test_data_item_next();
	test_data_array_refs();
	test_missing_file_refs();
	test_context_objects();

	printf("test-obs-mem: all tests passed\n");
	return 0;
}
