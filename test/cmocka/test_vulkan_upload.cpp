#include <stdio.h>
#include <stdlib.h>

#include <math.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include <graphics/graphics.h>
#include <util/base.h>
#include <util/platform.h>

#include "vulkan-subsystem.hpp"

// cmocka.h redefines `inline`, so it must come after the C++ standard headers.
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
extern "C" {
#include <cmocka.h>
}

// Set OBS_VULKAN_REQUIRE_VALIDATION=1 to make a missing validation layer fail the test.
static void require_validation()
{
	const char *rq = getenv("OBS_VULKAN_REQUIRE_VALIDATION");
	if (rq && !strcmp(rq, "1")) {
		fail_msg("validation layer required (OBS_VULKAN_REQUIRE_VALIDATION=1) but missing");
	}
}

static void *module;
static int (*create)(gs_device_t **, uint32_t);
static void (*destroy)(gs_device_t *);
static int (*messages)(void);
static gs_texture_t *(*texture_create)(gs_device_t *, uint32_t, uint32_t, enum gs_color_format, uint32_t,
				       const uint8_t **, uint32_t);
static void (*texture_destroy)(gs_texture_t *);
static bool (*texture_map)(gs_texture_t *, uint8_t **, uint32_t *);
static void (*texture_unmap)(gs_texture_t *);
static gs_stagesurf_t *(*stagesurface_create)(gs_device_t *, uint32_t, uint32_t, enum gs_color_format);
static void (*stage_texture)(gs_device_t *, gs_stagesurf_t *, gs_texture_t *);
static bool (*stagesurface_map)(gs_stagesurf_t *, uint8_t **, uint32_t *);
static void (*stagesurface_unmap)(gs_stagesurf_t *);
static void (*stagesurface_destroy)(gs_stagesurf_t *);
static void (*flush)(gs_device_t *);

template<typename T> static bool load(T &fn, const char *name)
{
	fn = (T)os_dlsym(module, name);
	if (!fn) {
		print_message("missing export: %s\n", name);
	}
	return fn != nullptr;
}

static int setup(void **)
{
#ifdef _WIN32
	_putenv_s("OBS_VULKAN_VALIDATION", "1");
#else
	setenv("OBS_VULKAN_VALIDATION", "1", 1);
#endif
	module = os_dlopen(OBS_VULKAN_MODULE);
	if (!module) {
		return -1;
	}
	bool ok = load(create, "device_create") & load(destroy, "device_destroy") &
		  load(messages, "obs_vulkan_validation_messages") & load(texture_create, "device_texture_create") &
		  load(texture_destroy, "gs_texture_destroy") & load(texture_map, "gs_texture_map") &
		  load(texture_unmap, "gs_texture_unmap") & load(stagesurface_create, "device_stagesurface_create") &
		  load(stage_texture, "device_stage_texture") & load(stagesurface_map, "gs_stagesurface_map") &
		  load(stagesurface_unmap, "gs_stagesurface_unmap") &
		  load(stagesurface_destroy, "gs_stagesurface_destroy") & load(flush, "device_flush");
	return ok ? 0 : -1;
}

static int teardown(void **)
{
	os_dlclose(module);
	return 0;
}

static gs_device *create_or_skip()
{
	gs_device_t *device = nullptr;
	int ret = create(&device, 0);
	if (ret == GS_ERROR_NOT_SUPPORTED) {
		skip();
	}
	assert_int_equal(ret, GS_SUCCESS);
	return device;
}

// Destroys the device; with the validation layer active, no message may have been reported.
static void destroy_and_check(gs_device *device)
{
	destroy(device);
	int n = messages();
	if (n < 0) {
		require_validation();
		print_message("validation layer missing, not checked\n");
	} else {
		assert_int_equal(n, 0);
	}
}

// Runs fn on this thread; aborts the process when it takes longer than 5 s (a deadlock would hang ctest).
template<typename F> static void within_5s(F fn)
{
	std::promise<void> done;
	std::thread watchdog([f = done.get_future()]() mutable {
		if (f.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
			fprintf(stderr, "watchdog: timed out after 5 s\n");
			fflush(stderr);
			_Exit(2);
		}
	});
	fn();
	done.set_value();
	watchdog.join();
}

// Test hook OBS_VULKAN_TEST_FAIL_SUBMIT, read once at device creation; toggled here on the live device.
static void set_fail_submit(gs_device *device, bool on)
{
	device->frames.test_fail_submit = on;
}

// Stages `tex` into a new stage surface and compares it with `want` (tightly packed).
static void assert_readback(gs_device *device, gs_texture_t *tex, enum gs_color_format fmt, uint32_t w, uint32_t h,
			    const uint8_t *want)
{
	const uint32_t row = w * gs_get_format_bpp(fmt) / 8;
	gs_stagesurf_t *stage = stagesurface_create(device, w, h, fmt);
	assert_non_null(stage);
	stage_texture(device, stage, tex);
	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	bool mapped = false;
	within_5s([&] { mapped = stagesurface_map(stage, &data, &linesize); });
	assert_true(mapped);
	for (uint32_t y = 0; y < h; ++y) {
		assert_memory_equal(data + y * linesize, want + y * row, row);
	}
	stagesurface_unmap(stage);
	stagesurface_destroy(stage);
}

static void test_texture_with_initial_data_reads_back_exactly(void **)
{
	gs_device *device = create_or_skip();
	uint8_t pixels[4 * 4 * 4];
	for (uint32_t i = 0; i < sizeof(pixels); ++i) {
		pixels[i] = (uint8_t)(i * 4 + 1); // distinct per pixel and channel
	}
	const uint8_t *data[] = {pixels};
	gs_texture_t *tex = texture_create(device, 4, 4, GS_RGBA, 1, data, 0);
	assert_non_null(tex);

	assert_readback(device, tex, GS_RGBA, 4, 4, pixels);

	texture_destroy(tex);
	destroy_and_check(device);
}

static int warnings;
static void count_warnings(int lvl, const char *msg, va_list args, void *)
{
	if (lvl <= LOG_WARNING) {
		++warnings;
		vfprintf(stderr, msg, args);
		fputc('\n', stderr);
	}
}

static void test_texture_create_without_data_still_works(void **)
{
	gs_device *device = create_or_skip();
	log_handler_t old_handler;
	void *old_param;
	base_get_log_handler(&old_handler, &old_param);
	warnings = 0;
	base_set_log_handler(count_warnings, nullptr);

	gs_texture_t *tex = texture_create(device, 16, 16, GS_RGBA, 1, nullptr, 0);
	gs_texture_t *mips = texture_create(device, 16, 16, GS_BGRA, 0, nullptr, GS_BUILD_MIPMAPS);
	flush(device);

	base_set_log_handler(old_handler, old_param);
	assert_non_null(tex);
	assert_non_null(mips);
	assert_int_equal(warnings, 0);

	texture_destroy(mips);
	texture_destroy(tex);
	destroy_and_check(device);
}

static void test_mip_chain_generated_by_blit(void **)
{
	void (*stage_mip)(gs_device_t *, gs_stagesurf_t *, gs_texture_t *, uint32_t);
	if (!load(stage_mip, "obs_vulkan_test_stage_mip")) {
		fail();
	}

	gs_device *device = create_or_skip();
	uint8_t pixels[8 * 8 * 4];
	for (uint32_t y = 0; y < 8; ++y) {
		for (uint32_t x = 0; x < 8; ++x) {
			uint8_t v = (x + y) % 2 ? 255 : 0;
			uint8_t *p = pixels + (y * 8 + x) * 4;
			p[0] = p[1] = p[2] = v;
			p[3] = 255;
		}
	}
	const uint8_t *data[] = {pixels};
	gs_texture_t *tex = texture_create(device, 8, 8, GS_BGRA, 1, data, GS_BUILD_MIPMAPS);
	assert_non_null(tex);
	assert_int_equal(tex->levels, 4);

	gs_stagesurf_t *stage = stagesurface_create(device, 1, 1, GS_BGRA);
	assert_non_null(stage);
	stage_mip(device, stage, tex, 3);
	uint8_t *out = nullptr;
	uint32_t linesize = 0;
	bool mapped = false;
	within_5s([&] { mapped = stagesurface_map(stage, &out, &linesize); });
	assert_true(mapped);
	for (int c = 0; c < 3; ++c) {
		if (out[c] < 126 || out[c] > 129) {
			fail_msg("channel %d is %u, expected mid-grey", c, out[c]);
		}
	}
	assert_int_equal(out[3], 255);
	stagesurface_unmap(stage);

	stagesurface_destroy(stage);
	texture_destroy(tex);
	destroy_and_check(device);
}

// GS_BUILD_MIPMAPS on non-power-of-two sizes at device level (libobs's gs_texture_create strips NPOT mips, the
// backend must still handle device_texture_create callers). Each level must be max(1, size >> level) and equal
// the linear blit of the level above: a bilinear sample at the destination texel centre mapped into the source,
// edge-clamped, computed on the CPU from the read-back previous level (within 2/255).
static void test_npot_mip_chain_is_bilinear_per_level(void **)
{
	void (*stage_mip)(gs_device_t *, gs_stagesurf_t *, gs_texture_t *, uint32_t);
	if (!load(stage_mip, "obs_vulkan_test_stage_mip")) {
		fail();
	}
	gs_device *device = create_or_skip();
	const uint32_t sizes[][2] = {{6, 3}, {5, 5}, {7, 2}};
	for (const auto &sz : sizes) {
		const uint32_t w0 = sz[0], h0 = sz[1];
		std::vector<uint8_t> prev(w0 * h0 * 4);
		for (uint32_t i = 0; i < w0 * h0; ++i) { // non-uniform: every texel and channel differs
			prev[i * 4 + 0] = (uint8_t)(i * 37);
			prev[i * 4 + 1] = (uint8_t)(255 - i * 23);
			prev[i * 4 + 2] = (uint8_t)(i * 11 + 40);
			prev[i * 4 + 3] = 255;
		}
		const uint8_t *data[] = {prev.data()};
		gs_texture_t *tex = texture_create(device, w0, h0, GS_RGBA, 1, data, GS_BUILD_MIPMAPS);
		assert_non_null(tex);
		assert_int_equal(tex->levels, gs_get_total_levels(w0, h0, 1));
		uint32_t pw = w0, ph = h0;
		for (uint32_t level = 1; level < tex->levels; ++level) {
			const uint32_t w = std::max(w0 >> level, 1u), h = std::max(h0 >> level, 1u);
			gs_stagesurf_t *stage = stagesurface_create(device, w, h, GS_RGBA);
			assert_non_null(stage);
			stage_mip(device, stage, tex, level); // rejects (logs, no copy) a size mismatch
			uint8_t *out = nullptr;
			uint32_t linesize = 0;
			bool mapped = false;
			within_5s([&] { mapped = stagesurface_map(stage, &out, &linesize); });
			assert_true(mapped);
			std::vector<uint8_t> cur(w * h * 4);
			for (uint32_t y = 0; y < h; ++y) {
				for (uint32_t x = 0; x < w; ++x) {
					const float sx = (x + 0.5f) * pw / w - 0.5f, sy = (y + 0.5f) * ph / h - 0.5f;
					const int x0 = (int)floorf(sx), y0 = (int)floorf(sy);
					const float fx = sx - x0, fy = sy - y0;
					auto at = [&](int xx, int yy, int c) {
						xx = std::clamp(xx, 0, (int)pw - 1),
						yy = std::clamp(yy, 0, (int)ph - 1);
						return (float)prev[(yy * pw + xx) * 4 + c];
					};
					for (int c = 0; c < 4; ++c) {
						const float v =
							(at(x0, y0, c) * (1 - fx) + at(x0 + 1, y0, c) * fx) * (1 - fy) +
							(at(x0, y0 + 1, c) * (1 - fx) + at(x0 + 1, y0 + 1, c) * fx) *
								fy;
						const int got = out[y * linesize + x * 4 + c];
						if (abs(got - (int)lroundf(v)) > 2) {
							fail_msg("%ux%u level %u (%u,%u) channel %d: %d, want %.1f", w0,
								 h0, level, x, y, c, got, v);
						}
						cur[(y * w + x) * 4 + c] = (uint8_t)got;
					}
				}
			}
			stagesurface_unmap(stage);
			stagesurface_destroy(stage);
			prev.swap(cur);
			pw = w, ph = h;
		}
		texture_destroy(tex);
	}
	destroy_and_check(device);
}

// `row` bytes per row, `h` rows; `want` receives the same bytes tightly packed.
static void write_pattern(uint8_t *ptr, uint32_t linesize, uint32_t row, uint32_t h, uint8_t seed, uint8_t *want)
{
	for (uint32_t y = 0; y < h; ++y) {
		for (uint32_t x = 0; x < row; ++x) {
			uint8_t v = (uint8_t)(seed + y * 37 + x * 3);
			ptr[y * linesize + x] = v;
			want[y * row + x] = v;
		}
	}
}

static void test_dynamic_texture_map_write_unmap_roundtrip(void **)
{
	gs_device *device = create_or_skip();
	gs_texture_t *tex = texture_create(device, 8, 4, GS_BGRA, 1, nullptr, GS_DYNAMIC);
	assert_non_null(tex);
	uint8_t want[8 * 4 * 4];
	uint8_t *ptr = nullptr;
	uint32_t linesize = 0;
	bool mapped = false;

	within_5s([&] { mapped = texture_map(tex, &ptr, &linesize); });
	assert_true(mapped);
	assert_int_equal(linesize, 8 * 4);
	write_pattern(ptr, linesize, 8 * 4, 4, 1, want);
	texture_unmap(tex);
	assert_readback(device, tex, GS_BGRA, 8, 4, want);

	// Two map/unmap pairs in one frame, the first copy not yet submitted: map neither submits nor waits, and the
	// copies apply in order.
	within_5s([&] { mapped = texture_map(tex, &ptr, &linesize); });
	assert_true(mapped);
	write_pattern(ptr, linesize, 8 * 4, 4, 100, want);
	texture_unmap(tex);
	const uint64_t value = device->graphics.next_value;
	within_5s([&] { mapped = texture_map(tex, &ptr, &linesize); });
	assert_true(mapped);
	assert_int_equal(device->graphics.next_value, value);
	write_pattern(ptr, linesize, 8 * 4, 4, 200, want);
	texture_unmap(tex);
	assert_readback(device, tex, GS_BGRA, 8, 4, want);

	texture_destroy(tex);
	destroy_and_check(device);
}

// Vulkan entry points straight from the loader (the test links only the headers).
template<typename T> static T vkfn(const char *name)
{
#ifdef _WIN32
	static void *lib = os_dlopen("vulkan-1");
#else
	static void *lib = os_dlopen("libvulkan.so.1");
#endif
	T fn = lib ? (T)os_dlsym(lib, name) : nullptr;
	if (!fn) {
		fail_msg("vulkan loader: %s missing", name);
	}
	return fn;
}

// map hands out space in the recording frame's upload arena, so it never waits for the GPU: here the submission
// holding the previous unmap's copy cannot finish until a helper thread signals `hold`, 500 ms later. A map that waited
// for that submission returns only after the signal.
static void test_dynamic_map_does_not_wait_for_previous_frame(void **)
{
	gs_device *device = create_or_skip();
	auto create_sem = vkfn<PFN_vkCreateSemaphore>("vkCreateSemaphore");
	auto destroy_sem = vkfn<PFN_vkDestroySemaphore>("vkDestroySemaphore");
	auto submit2 = vkfn<PFN_vkQueueSubmit2>("vkQueueSubmit2");
	auto signal = vkfn<PFN_vkSignalSemaphore>("vkSignalSemaphore");
	auto wait_idle = vkfn<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle");

	gs_texture_t *tex = texture_create(device, 8, 4, GS_BGRA, 1, nullptr, GS_DYNAMIC);
	assert_non_null(tex);
	uint8_t want[8 * 4 * 4];
	uint8_t *ptr = nullptr;
	uint32_t linesize = 0;
	assert_true(texture_map(tex, &ptr, &linesize));
	write_pattern(ptr, linesize, 8 * 4, 4, 1, want);
	texture_unmap(tex);

	// A batch waiting for `hold` (timeline, wait-before-signal) ahead of the frame on the same queue. Queues run batches
	// in order on the drivers tested, so the frame cannot finish before `hold` is signalled. (A binary semaphore chained
	// from that batch into the frame's acquire wait would guarantee it, but the validation layer rejects that.) If a
	// driver let the frame overtake the batch, this test could pass with a waiting map; it can never fail a
	// non-waiting one.
	VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	VkSemaphoreCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
	VkSemaphore hold = VK_NULL_HANDLE;
	assert_int_equal(create_sem(device->dev, &tci, nullptr, &hold), VK_SUCCESS);
	VkSemaphoreSubmitInfo w = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
	w.semaphore = hold;
	w.value = 1;
	w.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
	si.waitSemaphoreInfoCount = 1;
	si.pWaitSemaphoreInfos = &w;
	assert_int_equal(submit2(device->graphics.q, 1, &si, VK_NULL_HANDLE), VK_SUCCESS);
	flush(device);

	std::atomic<bool> released{false};
	std::thread release([&] {
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
		released = true;
		VkSemaphoreSignalInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
		sig.semaphore = hold;
		sig.value = 1;
		signal(device->dev, &sig);
	});
	const bool mapped = texture_map(tex, &ptr, &linesize);
	const bool waited = released;
	if (mapped) {
		write_pattern(ptr, linesize, 8 * 4, 4, 50, want);
		texture_unmap(tex);
	}
	release.join();
	assert_true(mapped);
	assert_false(waited);
	assert_readback(device, tex, GS_BGRA, 8, 4, want);

	wait_idle(device->dev);
	destroy_sem(device->dev, hold, nullptr);
	texture_destroy(tex);
	destroy_and_check(device);
}

// Records and submits one frame: a map/unmap of `other`.
static void submit_one_frame(gs_device *device, gs_texture_t *other, uint8_t seed, uint8_t *want)
{
	uint8_t *ptr = nullptr;
	uint32_t linesize = 0;
	assert_true(texture_map(other, &ptr, &linesize));
	write_pattern(ptr, linesize, 4 * 4, 4, seed, want);
	texture_unmap(other);
	const uint64_t value = device->graphics.next_value;
	flush(device);
	assert_int_equal(device->graphics.next_value, value + 1);
}

// A mapping held across submissions: the pointer stays valid and the update applies, both while the slot it came from
// is not begun again (copied via the current frame's arena) and after a full turn of the ring (that slot keeps its
// arena while mapped, so nothing else is overwritten by writes through the pointer).
static void test_dynamic_map_across_flush_stays_valid(void **)
{
	gs_device *device = create_or_skip();
	gs_texture_t *tex = texture_create(device, 4, 4, GS_RGBA, 1, nullptr, GS_DYNAMIC);
	gs_texture_t *other = texture_create(device, 4, 4, GS_RGBA, 1, nullptr, GS_DYNAMIC);
	assert_non_null(tex);
	assert_non_null(other);
	uint8_t want[4 * 4 * 4], want_other[4 * 4 * 4];
	uint8_t *ptr = nullptr;
	uint32_t linesize = 0;

	// Held across vk_frame_ring::count - 1 submissions (the map's own frame and one more).
	assert_true(texture_map(tex, &ptr, &linesize));
	flush(device);
	submit_one_frame(device, other, 9, want_other);
	write_pattern(ptr, linesize, 4 * 4, 4, 1, want);
	texture_unmap(tex);
	assert_readback(device, tex, GS_RGBA, 4, 4, want);
	assert_readback(device, other, GS_RGBA, 4, 4, want_other);

	// Held across more than a full turn of the ring (its slot is begun again twice), written only afterwards.
	assert_true(texture_map(tex, &ptr, &linesize));
	flush(device);
	for (uint32_t i = 0; i < 2 * vk_frame_ring::count; ++i) {
		submit_one_frame(device, other, (uint8_t)(20 + i), want_other);
	}
	write_pattern(ptr, linesize, 4 * 4, 4, 77, want);
	texture_unmap(tex);
	assert_readback(device, tex, GS_RGBA, 4, 4, want);
	assert_readback(device, other, GS_RGBA, 4, 4, want_other);
	for (const vk_frame &f : device->frames.frames) {
		assert_int_equal(f.maps, 0);
	}

	texture_destroy(other);
	texture_destroy(tex);
	destroy_and_check(device);
}

// As D3D11, a texture that is already mapped cannot be mapped again.
static void test_dynamic_map_twice_without_unmap_returns_false(void **)
{
	gs_device *device = create_or_skip();
	gs_texture_t *tex = texture_create(device, 4, 4, GS_RGBA, 1, nullptr, GS_DYNAMIC);
	assert_non_null(tex);
	uint8_t want[4 * 4 * 4];
	uint8_t *ptr = nullptr, *again = nullptr;
	uint32_t linesize = 0, linesize2 = 0;
	assert_true(texture_map(tex, &ptr, &linesize));
	write_pattern(ptr, linesize, 4 * 4, 4, 3, want);
	assert_false(texture_map(tex, &again, &linesize2));
	texture_unmap(tex);
	assert_readback(device, tex, GS_RGBA, 4, 4, want);
	// Mappable again after the unmap.
	assert_true(texture_map(tex, &ptr, &linesize));
	texture_unmap(tex);
	texture_destroy(tex);
	destroy_and_check(device);
}

// An unmap without a map records nothing (it neither begins a frame nor copies stale data).
static void test_dynamic_unmap_without_map_is_noop(void **)
{
	gs_device *device = create_or_skip();
	gs_texture_t *tex = texture_create(device, 4, 4, GS_RGBA, 1, nullptr, GS_DYNAMIC);
	assert_non_null(tex);
	flush(device);
	assert_false(device->frames.recording);
	texture_unmap(tex);
	assert_false(device->frames.recording);

	uint8_t want[4 * 4 * 4];
	uint8_t *ptr = nullptr;
	uint32_t linesize = 0;
	assert_true(texture_map(tex, &ptr, &linesize));
	write_pattern(ptr, linesize, 4 * 4, 4, 5, want);
	texture_unmap(tex);
	flush(device);
	texture_unmap(tex); // second unmap
	assert_false(device->frames.recording);
	assert_readback(device, tex, GS_RGBA, 4, 4, want);
	texture_destroy(tex);
	destroy_and_check(device);
}

// Destroying a mapped texture (no unmap) is safe: the mapping lives in the frame's arena, nothing leaks or is copied.
static void test_dynamic_destroy_between_map_and_unmap_is_safe(void **)
{
	gs_device *device = create_or_skip();
	gs_texture_t *tex = texture_create(device, 4, 4, GS_RGBA, 1, nullptr, GS_DYNAMIC);
	gs_texture_t *other = texture_create(device, 4, 4, GS_RGBA, 1, nullptr, GS_DYNAMIC);
	assert_non_null(tex);
	assert_non_null(other);
	uint8_t want[4 * 4 * 4], unused[4 * 4 * 4];
	uint8_t *ptr = nullptr;
	uint32_t linesize = 0;
	assert_true(texture_map(tex, &ptr, &linesize));
	write_pattern(ptr, linesize, 4 * 4, 4, 1, unused);
	texture_destroy(tex);
	for (const vk_frame &f : device->frames.frames) {
		assert_int_equal(f.maps, 0); // its slot may reuse the arena again
	}
	for (uint32_t i = 0; i < vk_frame_ring::count + 1; ++i) {
		submit_one_frame(device, other, (uint8_t)(40 + i), want);
	}
	assert_readback(device, other, GS_RGBA, 4, 4, want);
	texture_destroy(other);
	destroy_and_check(device);
}

// Mapped pointers stay valid while later maps grow the arena by a new chunk (old chunks are never moved or freed),
// and each copy's buffer offset suits its format (R8 9 bytes, then RGBA32F needing 16-byte texel alignment).
static void test_dynamic_map_survives_arena_growth(void **)
{
	gs_device *device = create_or_skip();
	gs_texture_t *r8 = texture_create(device, 3, 3, GS_R8, 1, nullptr, GS_DYNAMIC);
	gs_texture_t *f32 = texture_create(device, 3, 1, GS_RGBA32F, 1, nullptr, GS_DYNAMIC);
	gs_texture_t *big = texture_create(device, 1024, 512, GS_RGBA, 1, nullptr, GS_DYNAMIC); // 2 MiB: a new chunk
	assert_non_null(r8);
	assert_non_null(f32);
	assert_non_null(big);
	uint8_t want_r8[3 * 3], want_f32[3 * 16];
	std::vector<uint8_t> want_big(1024 * 512 * 4);
	uint8_t *p_r8, *p_f32, *p_big;
	uint32_t l_r8, l_f32, l_big;
	assert_true(texture_map(r8, &p_r8, &l_r8));
	assert_true(texture_map(f32, &p_f32, &l_f32));
	const size_t chunks = device->frames.frames[device->frames.index].arena.size();
	assert_true(texture_map(big, &p_big, &l_big));
	assert_true(device->frames.frames[device->frames.index].arena.size() > chunks);
	assert_int_equal(l_r8, 3);
	assert_int_equal(l_f32, 3 * 16);
	assert_int_equal(l_big, 1024 * 4);
	write_pattern(p_r8, l_r8, 3, 3, 11, want_r8);
	write_pattern(p_f32, l_f32, 3 * 16, 1, 12, want_f32);
	write_pattern(p_big, l_big, 1024 * 4, 512, 13, want_big.data());
	texture_unmap(r8);
	texture_unmap(f32);
	texture_unmap(big);
	assert_readback(device, r8, GS_R8, 3, 3, want_r8);
	assert_readback(device, f32, GS_RGBA32F, 3, 1, want_f32);
	assert_readback(device, big, GS_RGBA, 1024, 512, want_big.data());
	texture_destroy(big);
	texture_destroy(f32);
	texture_destroy(r8);
	destroy_and_check(device);
}

static void test_map_non_dynamic_texture_returns_false(void **)
{
	gs_device *device = create_or_skip();
	gs_texture_t *tex = texture_create(device, 8, 8, GS_RGBA, 1, nullptr, 0);
	assert_non_null(tex);
	uint8_t *ptr = nullptr;
	uint32_t linesize = 0;
	assert_false(texture_map(tex, &ptr, &linesize));
	texture_destroy(tex);
	destroy_and_check(device);
}

static void test_srgb_capable_format_creates_with_both_views_without_validation_errors(void **)
{
	gs_device *device = create_or_skip();
	const gs_color_format srgb[] = {GS_RGBA, GS_BGRA, GS_BGRX};
	for (gs_color_format f : srgb) {
		gs_texture_t *tex = texture_create(device, 16, 16, f, 1, nullptr, GS_RENDER_TARGET);
		assert_non_null(tex);
		assert_non_null(tex->view);
		assert_non_null(tex->srgb_view);
		texture_destroy(tex);
	}
	gs_texture_t *unorm = texture_create(device, 16, 16, GS_BGRX_UNORM, 1, nullptr, 0);
	assert_non_null(unorm);
	assert_non_null(unorm->view);
	assert_null(unorm->srgb_view);
	texture_destroy(unorm);
	destroy_and_check(device);
}

static void test_failed_submit_after_upload_does_not_hang(void **)
{
	gs_device *device = create_or_skip();
	uint8_t pixels[8 * 8 * 4] = {};
	const uint8_t *data[] = {pixels};
	gs_texture_t *tex = texture_create(device, 8, 8, GS_RGBA, 1, data, GS_BUILD_MIPMAPS);
	gs_texture_t *dyn = texture_create(device, 8, 8, GS_RGBA, 1, nullptr, GS_DYNAMIC);
	assert_non_null(tex);
	assert_non_null(dyn);
	uint8_t *ptr = nullptr;
	uint32_t linesize = 0;
	bool mapped = false;
	within_5s([&] { mapped = texture_map(dyn, &ptr, &linesize); });
	assert_true(mapped);
	texture_unmap(dyn);

	set_fail_submit(device, true);
	flush(device);
	set_fail_submit(device, false);

	// The lost copy must not make the next map wait for a value nobody signals.
	within_5s([&] { mapped = texture_map(dyn, &ptr, &linesize); });
	assert_true(mapped);
	texture_unmap(dyn);
	// Mapped, then the frame holding the mapping fails to submit: the slot is begun again at once but keeps the mapped
	// arena space, so the unmap still applies the update.
	within_5s([&] { mapped = texture_map(dyn, &ptr, &linesize); });
	assert_true(mapped);
	set_fail_submit(device, true);
	within_5s([&] { flush(device); });
	set_fail_submit(device, false);
	uint8_t want[8 * 8 * 4];
	write_pattern(ptr, linesize, 8 * 4, 8, 1, want);
	within_5s([&] { texture_unmap(dyn); });
	assert_readback(device, dyn, GS_RGBA, 8, 8, want);
	// Later updates apply too.
	within_5s([&] { mapped = texture_map(dyn, &ptr, &linesize); });
	assert_true(mapped);
	write_pattern(ptr, linesize, 8 * 4, 8, 2, want);
	texture_unmap(dyn);
	assert_readback(device, dyn, GS_RGBA, 8, 8, want);

	texture_destroy(dyn);
	texture_destroy(tex);
	within_5s([&] { destroy_and_check(device); });
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_texture_with_initial_data_reads_back_exactly),
		cmocka_unit_test(test_texture_create_without_data_still_works),
		cmocka_unit_test(test_mip_chain_generated_by_blit),
		cmocka_unit_test(test_npot_mip_chain_is_bilinear_per_level),
		cmocka_unit_test(test_dynamic_texture_map_write_unmap_roundtrip),
		cmocka_unit_test(test_dynamic_map_does_not_wait_for_previous_frame),
		cmocka_unit_test(test_dynamic_map_across_flush_stays_valid),
		cmocka_unit_test(test_dynamic_map_twice_without_unmap_returns_false),
		cmocka_unit_test(test_dynamic_unmap_without_map_is_noop),
		cmocka_unit_test(test_dynamic_destroy_between_map_and_unmap_is_safe),
		cmocka_unit_test(test_dynamic_map_survives_arena_growth),
		cmocka_unit_test(test_map_non_dynamic_texture_returns_false),
		cmocka_unit_test(test_srgb_capable_format_creates_with_both_views_without_validation_errors),
		cmocka_unit_test(test_failed_submit_after_upload_does_not_hang),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
