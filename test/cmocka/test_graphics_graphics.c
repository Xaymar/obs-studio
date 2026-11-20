#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"
#include "gs-test.h"

#include <math.h>
#include <string.h>

#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <graphics/quat.h>
#include <graphics/axisang.h>

/* graphics.c through the gs_* API on libobs-null */

#define assert_near(a, b) assert_true(fabsf((float)(a) - (float)(b)) < 1e-4f)

static const float marker_color[4] = {0.5f, 0.5f, 0.5f, 1.0f};

static void info_test(void **state)
{
	UNUSED_PARAMETER(state);

	assert_string_equal(gs_get_device_name(), "Null");
	assert_string_equal(gs_get_driver_version(), "0.0.0");
	assert_string_equal(gs_get_renderer(), "Null Renderer");
	assert_int_equal(gs_get_gpu_dmem(), 0);
	assert_int_equal(gs_get_gpu_smem(), 0);
	assert_int_equal(gs_get_device_type(), GS_DEVICE_DIRECT3D_11);
	assert_int_equal(gs_get_adapter_count(), 1);
	assert_ptr_equal(gs_get_context(), gs_test_graphics);
	assert_non_null(gs_get_device_obj());
	assert_false(gs_nv12_available());
	assert_false(gs_p010_available());
	assert_false(gs_is_monitor_hdr(NULL));
#ifdef _WIN32
	assert_false(gs_shared_texture_available());
	assert_false(gs_gdi_texture_available());
	assert_false(gs_can_adapter_fast_clear());
#endif
	/* without device support, NV12 and P010 fall back to two separate plane textures */
	gs_texture_t *y = NULL, *uv = NULL;
	assert_true(gs_texture_create_nv12(&y, &uv, 16, 16, 0));
	assert_int_equal(gs_texture_get_width(uv), 8);
	gs_texture_destroy(y);
	gs_texture_destroy(uv);
	if (gs_texture_create_p010(&y, &uv, 16, 16, 0)) {
		gs_texture_destroy(y);
		gs_texture_destroy(uv);
	}
#ifdef _WIN32
	assert_null(gs_stagesurface_create_nv12(16, 16));
	assert_null(gs_stagesurface_create_p010(16, 16));
	assert_null(gs_duplicator_create(0));
	assert_null(gs_texture_open_shared(1));
	assert_null(gs_texture_open_nt_shared(1));
	assert_null(gs_texture_wrap_obj(NULL));
	assert_null(gs_texture_create_gdi(16, 16));
	struct gs_monitor_info info;
	assert_false(gs_get_duplicator_monitor_info(0, &info));
	assert_int_equal(gs_duplicator_get_monitor_index(NULL), -1);
	struct gs_device_loss loss = {0};
	gs_register_loss_callbacks(&loss);
	gs_unregister_loss_callbacks(NULL);
#endif
	gs_debug_marker_begin(marker_color, "marker");
	gs_debug_marker_begin_format(marker_color, "marker %d", 1);
	gs_debug_marker_end();
	assert_true(gs_test_calls("device_debug_marker_begin") == 2);
}

static bool count_adapter(void *param, const char *name, uint32_t id)
{
	int *count = param;
	UNUSED_PARAMETER(name);
	UNUSED_PARAMETER(id);
	(*count)++;
	return true;
}

static void adapters_test(void **state)
{
	UNUSED_PARAMETER(state);
	int count = 0;
	gs_enum_adapters(count_adapter, &count);
	assert_int_equal(count, 1);
}

static void matrix_stack_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct matrix4 m;
	gs_matrix_identity();
	gs_matrix_get(&m);
	assert_near(m.x.x, 1.0f);

	gs_matrix_push();
	gs_matrix_translate3f(1.0f, 2.0f, 3.0f);
	gs_matrix_get(&m);
	assert_near(m.t.x, 1.0f);
	assert_near(m.t.z, 3.0f);
	struct vec3 v;
	vec3_set(&v, 1.0f, 1.0f, 1.0f);
	gs_matrix_translate(&v);
	gs_matrix_get(&m);
	assert_near(m.t.x, 2.0f);
	gs_matrix_scale3f(2.0f, 2.0f, 2.0f);
	gs_matrix_scale(&v);
	gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, 0.5f);
	struct axisang aa;
	axisang_set(&aa, 0.0f, 1.0f, 0.0f, 0.25f);
	gs_matrix_rotaa(&aa);
	struct quat q;
	quat_identity(&q);
	gs_matrix_rotquat(&q);
	gs_matrix_transpose();
	gs_matrix_pop();

	gs_matrix_get(&m);
	assert_near(m.t.x, 0.0f);

	struct matrix4 set;
	matrix4_identity(&set);
	set.t.y = 7.0f;
	gs_matrix_set(&set);
	gs_matrix_mul(&set);
	gs_matrix_get(&m);
	assert_near(m.t.y, 14.0f);

	/* popping the last matrix is refused */
	gs_matrix_identity();
	gs_matrix_pop();
	gs_matrix_get(&m);
	assert_near(m.x.x, 1.0f);
}

static void viewport_projection_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct gs_rect rect;
	gs_set_viewport(1, 2, 30, 40);
	gs_viewport_push();
	gs_set_viewport(0, 0, 10, 10);
	gs_get_viewport(&rect);
	assert_int_equal(rect.cx, 10);
	gs_viewport_pop();
	gs_get_viewport(&rect);
	assert_int_equal(rect.x, 1);
	assert_int_equal(rect.cy, 40);
	gs_viewport_pop(); /* nothing pushed */

	gs_reset_viewport();
	gs_set_2d_mode();
	gs_set_3d_mode(60.0f, 0.1f, 100.0f);
	gs_perspective(90.0f, 16.0f / 9.0f, 0.1f, 100.0f);
	gs_ortho(0.0f, 10.0f, 0.0f, 10.0f, -1.0f, 1.0f);
	gs_frustum(-1.0f, 1.0f, -1.0f, 1.0f, 0.1f, 10.0f);
	gs_projection_push();
	gs_projection_pop();
	assert_true(gs_test_calls("device_ortho") >= 2);
	assert_true(gs_test_calls("device_frustum") >= 2);

	gs_set_scissor_rect(&rect);
	gs_set_scissor_rect(NULL);
	gs_set_cull_mode(GS_FRONT);
	assert_int_equal(gs_get_cull_mode(), GS_FRONT);
}

static void blend_state_test(void **state)
{
	UNUSED_PARAMETER(state);

	gs_blend_state_push();
	gs_enable_blending(false);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_blend_function_separate(GS_BLEND_ONE, GS_BLEND_ZERO, GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_blend_op(GS_BLEND_OP_ADD);
	gs_blend_state_pop();
	gs_blend_state_pop(); /* nothing pushed */
	gs_reset_blend_state();
	assert_true(gs_test_calls("device_blend_function") + gs_test_calls("device_blend_function_separate") >= 3);

	gs_enable_depth_test(true);
	gs_enable_stencil_test(true);
	gs_enable_stencil_write(true);
	gs_enable_color(true, true, true, false);
	gs_depth_function(GS_LESS);
	gs_stencil_function(GS_STENCIL_BOTH, GS_ALWAYS);
	gs_stencil_op(GS_STENCIL_BOTH, GS_KEEP, GS_KEEP, GS_REPLACE);

	gs_enable_framebuffer_srgb(true);
	assert_true(gs_framebuffer_srgb_enabled());
	gs_set_linear_srgb(true);
	assert_true(gs_get_linear_srgb());
	gs_set_linear_srgb(false);
}

static void immediate_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* immediate mode: reuses the shared vertex buffer */
	gs_render_start(false);
	struct vec2 v2;
	struct vec3 v3;
	struct vec4 c;
	vec2_set(&v2, 0.0f, 0.0f);
	vec3_set(&v3, 1.0f, 0.0f, 0.0f);
	vec4_set(&c, 1.0f, 0.0f, 0.0f, 1.0f);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex3f(1.0f, 0.0f, 0.0f);
	gs_vertex2v(&v2);
	gs_vertex3v(&v3);
	for (int i = 0; i < 4; i++) {
		gs_normal3f(0.0f, 0.0f, 1.0f);
		gs_color(0xffffffff);
		gs_texcoord(0.0f, 0.0f, 0);
	}
	gs_render_stop(GS_TRISTRIP);
	assert_int_equal(gs_test_calls("device_draw"), 1);

	/* a new buffer, saved for later */
	gs_render_start(true);
	gs_vertex3v(&v3);
	gs_vertex3v(&v3);
	gs_vertex3v(&v3);
	gs_normal3v(&v3);
	gs_normal3v(&v3);
	gs_normal3v(&v3);
	gs_color4v(&c);
	gs_color4v(&c);
	gs_color4v(&c);
	gs_texcoord2v(&v2, 0);
	gs_texcoord2v(&v2, 0);
	gs_texcoord2v(&v2, 0);
	gs_vertbuffer_t *vb = gs_render_save();
	assert_non_null(vb);
	struct gs_vb_data *data = gs_vertexbuffer_get_data(vb);
	assert_int_equal(data->num, 3);
	/* Regression: gs_color4v() was an empty stub and dropped the colors */
	assert_non_null(data->colors);
	for (size_t i = 0; i < 3; i++)
		assert_int_equal(data->colors[i], 0xff0000ff);
	gs_vertexbuffer_destroy(vb);

	/* a new buffer drawn right away, with mismatching counts */
	gs_render_start(true);
	gs_vertex3v(&v3);
	gs_vertex3v(&v3);
	gs_normal3v(&v3);
	gs_color4v(&c);
	gs_texcoord2v(&v2, 0);
	gs_render_stop(GS_LINES);

	/* empty */
	gs_render_start(true);
	gs_render_stop(GS_POINTS);
	gs_render_start(true);
	assert_null(gs_render_save());
	gs_render_start(false);
	gs_render_stop(GS_POINTS);
}

static void texture_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* 2x2 BGRA, uploaded flipped */
	gs_texture_t *tex = gs_texture_create(2, 2, GS_BGRA, 1, NULL, GS_DYNAMIC);
	assert_non_null(tex);
	assert_int_equal(gs_get_texture_type(tex), GS_TEXTURE_2D);
	assert_int_equal(gs_texture_get_width(tex), 2);
	assert_int_equal(gs_texture_get_height(tex), 2);
	assert_int_equal(gs_texture_get_color_format(tex), GS_BGRA);
	assert_false(gs_texture_is_rect(tex));
	assert_non_null(gs_texture_get_obj(tex));

	const uint32_t px[4] = {1, 2, 3, 4};
	gs_texture_set_image(tex, (const uint8_t *)px, 8, true);
	uint8_t *ptr;
	uint32_t linesize;
	assert_true(gs_texture_map(tex, &ptr, &linesize));
	assert_int_equal(linesize, 8);
	assert_int_equal(((uint32_t *)ptr)[0], 3);
	assert_int_equal(((uint32_t *)ptr)[3], 2);
	gs_texture_unmap(tex);
	gs_texture_set_image(tex, (const uint8_t *)px, 8, false);
	assert_true(gs_texture_map(tex, &ptr, &linesize));
	assert_int_equal(((uint32_t *)ptr)[0], 1);
	gs_texture_unmap(tex);

	gs_load_texture(tex, 0);
	gs_load_texture(NULL, 0);
	gs_texture_destroy(tex);

	/* cube and volume textures */
	gs_texture_t *cube = gs_cubetexture_create(4, GS_RGBA, 1, NULL, 0);
	assert_int_equal(gs_cubetexture_get_size(cube), 4);
	assert_int_equal(gs_cubetexture_get_color_format(cube), GS_RGBA);
	uint32_t face[16] = {0};
	gs_cubetexture_set_image(cube, 0, face, 16, false);
	gs_cubetexture_set_image(cube, 1, face, 16, true);
	gs_cubetexture_destroy(cube);

	gs_texture_t *vol = gs_voltexture_create(2, 2, 2, GS_R8, 1, NULL, 0);
	assert_int_equal(gs_voltexture_get_width(vol), 2);
	assert_int_equal(gs_voltexture_get_height(vol), 2);
	assert_int_equal(gs_voltexture_get_depth(vol), 2);
	assert_int_equal(gs_voltexture_get_color_format(vol), GS_R8);
	gs_voltexture_destroy(vol);
}

/* Clear a render target, stage it and read the pixels back */
static void readback_test(void **state)
{
	UNUSED_PARAMETER(state);

	gs_texture_t *rt = gs_texture_create(4, 4, GS_BGRA, 1, NULL, GS_RENDER_TARGET);
	gs_zstencil_t *zs = gs_zstencil_create(4, 4, GS_Z24_S8);
	gs_stagesurf_t *stage = gs_stagesurface_create(4, 4, GS_BGRA);
	assert_int_equal(gs_stagesurface_get_width(stage), 4);
	assert_int_equal(gs_stagesurface_get_height(stage), 4);
	assert_int_equal(gs_stagesurface_get_color_format(stage), GS_BGRA);

	gs_set_render_target(rt, zs);
	assert_ptr_equal(gs_get_render_target(), rt);
	assert_ptr_equal(gs_get_zstencil_target(), zs);
	struct vec4 color;
	vec4_set(&color, 1.0f, 0.0f, 0.0f, 1.0f); /* red */
	gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH | GS_CLEAR_STENCIL, &color, 1.0f, 0);
	gs_stage_texture(stage, rt);

	uint8_t *data;
	uint32_t linesize;
	assert_true(gs_stagesurface_map(stage, &data, &linesize));
	assert_int_equal(linesize, 16);
	assert_int_equal(data[0], 0x00);  /* B */
	assert_int_equal(data[2], 0xff);  /* R */
	assert_int_equal(data[63], 0xff); /* A of the last pixel */
	gs_stagesurface_unmap(stage);

	/* copies between textures */
	gs_texture_t *copy = gs_texture_create(4, 4, GS_BGRA, 1, NULL, GS_DYNAMIC);
	gs_copy_texture(copy, rt);
	gs_copy_texture_region(copy, 0, 0, rt, 0, 0, 2, 2);
	uint8_t *ptr;
	assert_true(gs_texture_map(copy, &ptr, &linesize));
	assert_int_equal(ptr[2], 0xff);
	gs_texture_unmap(copy);

	gs_set_render_target_with_color_space(rt, NULL, GS_CS_709_EXTENDED);
	assert_int_equal(gs_get_color_space(), GS_CS_709_EXTENDED);
	gs_set_render_target(NULL, NULL);

	gs_texture_t *cube = gs_cubetexture_create(4, GS_BGRA, 1, NULL, GS_RENDER_TARGET);
	gs_set_cube_render_target(cube, 2, NULL);
	gs_set_render_target(NULL, NULL);

	gs_cubetexture_destroy(cube);
	gs_texture_destroy(copy);
	gs_stagesurface_destroy(stage);
	gs_zstencil_destroy(zs);
	gs_texture_destroy(rt);
}

static void buffers_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct gs_vb_data *vbd = gs_vbdata_create();
	vbd->num = 4;
	vbd->points = bzalloc(sizeof(struct vec3) * 4);
	gs_vertbuffer_t *vb = gs_vertexbuffer_create(vbd, GS_DYNAMIC);
	assert_ptr_equal(gs_vertexbuffer_get_data(vb), vbd);
	gs_vertexbuffer_flush(vb);
	gs_vertexbuffer_flush_direct(vb, vbd);
	gs_load_vertexbuffer(vb);

	uint16_t idx[6] = {0, 1, 2, 2, 1, 3};
	gs_indexbuffer_t *ib = gs_indexbuffer_create(GS_UNSIGNED_SHORT, idx, 6, GS_DUP_BUFFER | GS_DYNAMIC);
	assert_int_equal(gs_indexbuffer_get_num_indices(ib), 6);
	assert_int_equal(gs_indexbuffer_get_type(ib), GS_UNSIGNED_SHORT);
	assert_int_equal(((uint16_t *)gs_indexbuffer_get_data(ib))[5], 3);
	gs_indexbuffer_flush(ib);
	gs_indexbuffer_flush_direct(ib, idx);
	gs_load_indexbuffer(ib);
	gs_draw(GS_TRIS, 0, 6);

	/* GS_DUP_BUFFER copies the vertex data */
	struct gs_vb_data *src = gs_vbdata_create();
	src->num = 3;
	src->points = bzalloc(sizeof(struct vec3) * 3);
	src->normals = bzalloc(sizeof(struct vec3) * 3);
	src->tangents = bzalloc(sizeof(struct vec3) * 3);
	src->colors = bzalloc(sizeof(uint32_t) * 3);
	src->num_tex = 1;
	src->tvarray = bzalloc(sizeof(struct gs_tvertarray));
	src->tvarray[0].width = 2;
	src->tvarray[0].array = bzalloc(sizeof(float) * 2 * 3);
	gs_vertbuffer_t *dup = gs_vertexbuffer_create(src, GS_DUP_BUFFER);
	assert_ptr_not_equal(gs_vertexbuffer_get_data(dup), src);
	gs_vbdata_destroy(src);
	gs_vertexbuffer_destroy(dup);

	gs_load_vertexbuffer(NULL);
	gs_load_indexbuffer(NULL);
	gs_indexbuffer_destroy(ib);
	gs_vertexbuffer_destroy(vb);

	gs_timer_t *timer = gs_timer_create();
	gs_timer_begin(timer);
	gs_timer_end(timer);
	uint64_t ticks = 1;
	assert_true(gs_timer_get_data(timer, &ticks));
	gs_timer_destroy(timer);
	gs_timer_range_t *range = gs_timer_range_create();
	gs_timer_range_begin(range);
	gs_timer_range_end(range);
	bool disjoint;
	uint64_t freq;
	assert_true(gs_timer_range_get_data(range, &disjoint, &freq));
	gs_timer_range_destroy(range);

	struct gs_sampler_info si = {.filter = GS_FILTER_LINEAR, .address_u = GS_ADDRESS_CLAMP};
	gs_samplerstate_t *ss = gs_samplerstate_create(&si);
	gs_load_samplerstate(ss, 0);
	gs_load_default_samplerstate(false, 0);
	gs_samplerstate_destroy(ss);
}

static void frame_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct gs_init_data init = {.cx = 320, .cy = 240, .format = GS_BGRA, .num_backbuffers = 1};
	gs_swapchain_t *swap = gs_swapchain_create(&init);
	assert_non_null(swap);
	gs_load_swapchain(swap);
	uint32_t cx, cy;
	gs_get_size(&cx, &cy);
	assert_int_equal(cx, 320);
	assert_int_equal(gs_get_width(), 320);
	assert_int_equal(gs_get_height(), 240);
	gs_resize(640, 480);
	assert_int_equal(gs_get_width(), 640);
	gs_update_color_space();

	gs_begin_frame();
	gs_begin_scene();
	gs_draw_cube_backdrop(NULL, NULL, -1.0f, 1.0f, 1.0f, -1.0f, 0.1f);
	gs_end_scene();
	assert_true(gs_is_present_ready());
	gs_present();
	gs_flush();

	gs_load_swapchain(NULL);
	gs_swapchain_destroy(swap);
}

static void sprite_test(void **state)
{
	UNUSED_PARAMETER(state);

	gs_texture_t *tex = gs_texture_create(8, 4, GS_RGBA, 1, NULL, 0);
	gs_draw_sprite(tex, 0, 0, 0);
	gs_draw_sprite(tex, GS_FLIP_U | GS_FLIP_V, 16, 8);
	gs_draw_sprite(NULL, 0, 16, 8);
	gs_draw_sprite(NULL, 0, 0, 0); /* no texture and no size: refused */
	gs_draw_quadf(tex, 0, 0.0f, 0.0f);
	gs_draw_quadf(tex, GS_FLIP_U, 4.5f, 2.5f);
	gs_draw_quadf(NULL, GS_FLIP_V, 4.5f, 2.5f);
	gs_draw_sprite_subregion(tex, 0, 1, 1, 4, 2);
	gs_draw_sprite_subregion(tex, GS_FLIP_U | GS_FLIP_V, 0, 0, 8, 4);
	gs_draw_sprite_subregion(NULL, 0, 0, 0, 1, 1);
	assert_true(gs_test_calls("device_draw") >= 6);
	gs_texture_destroy(tex);
}

/* every wrapper refuses to run without a graphics context */
static void no_context_test(void **state)
{
	UNUSED_PARAMETER(state);

	gs_leave_context();
	assert_null(gs_get_context());
	assert_null(gs_get_device_name());
	assert_int_equal(gs_get_device_type(), -1);
	assert_null(gs_texture_create(2, 2, GS_RGBA, 1, NULL, 0));
	assert_null(gs_stagesurface_create(2, 2, GS_RGBA));
	assert_null(gs_vertexbuffer_create(NULL, 0));
	gs_matrix_push();
	gs_matrix_identity();
	gs_render_start(true);
	gs_vertex2f(0, 0);
	gs_render_stop(GS_TRIS);
	/* Regression: the sprite helpers dereferenced the missing context */
	gs_draw_sprite(NULL, 0, 1, 1);
	gs_draw_sprite_subregion(NULL, 0, 0, 0, 1, 1);
	gs_clear(GS_CLEAR_COLOR, NULL, 0.0f, 0);
	gs_blend_state_push();
	gs_set_2d_mode();
	gs_viewport_push();
	assert_null(gs_get_render_target());
	gs_enter_context(gs_test_graphics);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		gs_unit_test(info_test),         gs_unit_test(adapters_test),
		gs_unit_test(matrix_stack_test), gs_unit_test(viewport_projection_test),
		gs_unit_test(blend_state_test),  gs_unit_test(immediate_test),
		gs_unit_test(texture_test),      gs_unit_test(readback_test),
		gs_unit_test(buffers_test),      gs_unit_test(frame_test),
		gs_unit_test(sprite_test),       gs_unit_test(no_context_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
