#undef NDEBUG
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

#include <obs-mem.h>
#include <util/bmem.h>
#include <util/threading.h>

struct test_obj {
	struct obs_mem_header hdr;
	volatile long destroyed;
	bool want_weak;
	obs_mem_weak_t *weak_in_dtor;
	void *ref_in_dtor;
	volatile long *signal;
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

struct race_ctx {
	/* A creates the object, B tries to upgrade; the flags hand it between the two. */
	void *volatile shared_weak;
	volatile long ready;
	volatile long done;
	volatile long stop;
	volatile long bad;
	volatile long upgraded;
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
			/* Holding a ref means the destroy callback can't have run. */
			if (os_atomic_load_long(&obj->destroyed))
				os_atomic_inc_long(&ctx->bad);
			os_atomic_inc_long(&ctx->upgraded);
			obs_mem_release(obj);
		}

		os_atomic_store_long(&ctx->ready, 0);
		os_atomic_store_long(&ctx->done, 1);
	}
	return NULL;
}

static void *race_thread_a(void *param)
{
	struct race_ctx ctx = {0};
	pthread_t b;
	pthread_create(&b, NULL, race_thread_b, &ctx);

	for (int i = 0; i < RACE_ITERATIONS; i++) {
		struct test_obj *obj = test_create();
		obs_mem_weak_t *weak = obs_mem_get_weak(obj);

		ctx.shared_weak = weak;
		os_atomic_store_long(&ctx.done, 0);
		os_atomic_store_long(&ctx.ready, 1);

		obs_mem_release(obj);

		while (!os_atomic_load_long(&ctx.done))
			;

		assert(obj->destroyed == 1);
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

	printf("test-obs-mem: all tests passed\n");
	return 0;
}
