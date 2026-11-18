#include <stdint.h>

#include "vulkan-tracker.hpp"

// cmocka.h redefines `inline`, so it must come after the C++ standard headers.
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
extern "C" {
#include <cmocka.h>
}

#define IMG(n) ((VkImage)(uintptr_t)(n))
#define BUF(n) ((VkBuffer)(uintptr_t)(n))

static const vk_access sample = {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
				 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
static const vk_access render = {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
				 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

static void first_use_emits_undefined_to_needed_layout(void **)
{
	vk_tracker t;
	vk_barriers b;
	t.use_image(IMG(1), 0, sample, b);
	assert_int_equal(b.images.size(), 1);
	assert_int_equal(b.images[0].oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);
	assert_int_equal(b.images[0].newLayout, sample.layout);
	assert_int_equal(b.images[0].srcAccessMask, 0);
	assert_int_equal(b.images[0].srcStageMask, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
	assert_int_equal(b.images[0].dstStageMask, sample.stage);
	assert_true(b.images[0].image == IMG(1));
}

static void same_access_twice_emits_nothing(void **)
{
	vk_tracker t;
	vk_barriers b;
	t.use_image(IMG(1), 0, sample, b);
	b = {};
	t.use_image(IMG(1), 0, sample, b);
	assert_true(b.images.empty() && b.buffers.empty());
}

static void write_then_read_emits_one_barrier_with_write_in_src(void **)
{
	vk_tracker t;
	vk_barriers b;
	t.use_image(IMG(1), 0, render, b);
	b = {};
	t.use_image(IMG(1), 0, sample, b);
	assert_int_equal(b.images.size(), 1);
	assert_int_equal(b.images[0].srcStageMask, render.stage);
	assert_int_equal(b.images[0].srcAccessMask, render.access);
	assert_int_equal(b.images[0].dstAccessMask, sample.access);
}

static void read_then_read_emits_nothing_when_layout_matches(void **)
{
	vk_tracker t;
	vk_barriers b;
	t.use_image(IMG(1), 0, sample, b);
	b = {};
	vk_access other = {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, sample.layout};
	t.use_image(IMG(1), 0, other, b);
	assert_true(b.images.empty());
}

static void layout_change_emits_barrier_with_old_and_new_layout(void **)
{
	vk_tracker t;
	vk_barriers b;
	t.use_image(IMG(1), 0, sample, b);
	b = {};
	vk_access src = {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
			 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
	t.use_image(IMG(1), 0, src, b);
	assert_int_equal(b.images.size(), 1);
	assert_int_equal(b.images[0].oldLayout, sample.layout);
	assert_int_equal(b.images[0].newLayout, src.layout);
	assert_int_equal(b.images[0].srcAccessMask, 0);
}

static void forget_resets_to_undefined(void **)
{
	vk_tracker t;
	vk_barriers b;
	t.use_image(IMG(1), 0, sample, b);
	t.forget(IMG(1));
	b = {};
	t.use_image(IMG(1), 0, sample, b);
	assert_int_equal(b.images.size(), 1);
	assert_int_equal(b.images[0].oldLayout, VK_IMAGE_LAYOUT_UNDEFINED);
}

static void read_then_write_emits_execution_barrier(void **)
{
	vk_tracker t;
	vk_barriers b;
	// Two readers in different stages: the write must wait for both.
	vk_access vs = {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			VK_IMAGE_LAYOUT_GENERAL};
	vk_access fs = {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			VK_IMAGE_LAYOUT_GENERAL};
	vk_access wr = {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			VK_IMAGE_LAYOUT_GENERAL};
	t.use_image(IMG(1), 0, vs, b);
	t.use_image(IMG(1), 0, fs, b);
	b = {};
	t.use_image(IMG(1), 0, wr, b);
	assert_int_equal(b.images.size(), 1);
	assert_int_equal(b.images[0].oldLayout, VK_IMAGE_LAYOUT_GENERAL);
	assert_int_equal(b.images[0].srcStageMask, vs.stage | fs.stage);
	assert_int_equal(b.images[0].srcAccessMask, 0);
	assert_int_equal(b.images[0].dstAccessMask, wr.access);
}

static void buffer_first_use_is_free_then_write_then_read_barriers(void **)
{
	vk_tracker t;
	vk_barriers b;
	vk_access w = {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
	vk_access r = {VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT,
		       VK_IMAGE_LAYOUT_UNDEFINED};
	t.use_buffer(BUF(1), 0, w, b);
	assert_true(b.buffers.empty());
	t.use_buffer(BUF(1), 0, r, b);
	assert_int_equal(b.buffers.size(), 1);
	assert_int_equal(b.buffers[0].srcStageMask, w.stage);
	assert_int_equal(b.buffers[0].srcAccessMask, w.access);
	assert_int_equal(b.buffers[0].dstAccessMask, r.access);
}

static void write_then_write_emits_barrier(void **)
{
	vk_tracker t;
	vk_barriers b;
	t.use_image(IMG(1), 0, render, b);
	b = {};
	t.use_image(IMG(1), 0, render, b);
	assert_int_equal(b.images.size(), 1);
	assert_int_equal(b.images[0].srcAccessMask, render.access);
	assert_int_equal(b.images[0].dstAccessMask, render.access);
}

static void write_then_read_other_stage_emits_second_barrier(void **)
{
	vk_tracker t;
	vk_barriers b;
	vk_access w = {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL};
	vk_access fs = {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			VK_IMAGE_LAYOUT_GENERAL};
	vk_access vs = {VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
			VK_IMAGE_LAYOUT_GENERAL};
	t.use_image(IMG(1), 0, w, b);
	t.use_image(IMG(1), 0, fs, b);
	b = {};
	t.use_image(IMG(1), 0, vs, b);
	assert_int_equal(b.images.size(), 1);
	assert_true((b.images[0].srcStageMask & w.stage) != 0);
	assert_int_equal(b.images[0].srcAccessMask, w.access);
	assert_int_equal(b.images[0].dstStageMask, vs.stage);
	// The write after both readers waits for both.
	vk_access w2 = {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			VK_IMAGE_LAYOUT_GENERAL};
	b = {};
	t.use_image(IMG(1), 0, w2, b);
	assert_int_equal(b.images.size(), 1);
	assert_true((b.images[0].srcStageMask & (fs.stage | vs.stage)) == (fs.stage | vs.stage));
}

static void buffer_write_then_read_other_stage_emits_second_barrier(void **)
{
	vk_tracker t;
	vk_barriers b;
	vk_access w = {VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED};
	vk_access fs = {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED};
	vk_access va = {VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT,
			VK_IMAGE_LAYOUT_UNDEFINED};
	t.use_buffer(BUF(1), 0, w, b);
	t.use_buffer(BUF(1), 0, fs, b);
	b = {};
	t.use_buffer(BUF(1), 0, va, b);
	assert_int_equal(b.buffers.size(), 1);
	assert_int_equal(b.buffers[0].srcAccessMask, w.access);
	assert_int_equal(b.buffers[0].dstStageMask, va.stage);
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(first_use_emits_undefined_to_needed_layout),
		cmocka_unit_test(same_access_twice_emits_nothing),
		cmocka_unit_test(write_then_read_emits_one_barrier_with_write_in_src),
		cmocka_unit_test(read_then_read_emits_nothing_when_layout_matches),
		cmocka_unit_test(layout_change_emits_barrier_with_old_and_new_layout),
		cmocka_unit_test(forget_resets_to_undefined),
		cmocka_unit_test(read_then_write_emits_execution_barrier),
		cmocka_unit_test(buffer_first_use_is_free_then_write_then_read_barriers),
		cmocka_unit_test(write_then_write_emits_barrier),
		cmocka_unit_test(write_then_read_other_stage_emits_second_barrier),
		cmocka_unit_test(buffer_write_then_read_other_stage_emits_second_barrier),
	};
	return cmocka_run_group_tests(tests, NULL, NULL);
}
