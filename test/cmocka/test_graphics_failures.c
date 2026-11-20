#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"
#include "gs-test.h"
#include "null-fail.h"

#include <graphics/image-file.h>
#include <graphics/vec3.h>

/* graphics/: what libobs does when the device cannot create or map an object */

static void create_device_test(void **state)
{
	UNUSED_PARAMETER(state);
	obs_test_setup(state);

	graphics_t *graphics = NULL;
	null_fail_next("device_create", 1);
	assert_int_equal(gs_create(&graphics, NULL_GRAPHICS_MODULE, 0), GS_ERROR_FAIL);
	assert_null(graphics);

	/* the immediate and sprite vertex buffers are created with the device. Regression: this left the
	 * context current with no reference, and gs_destroy() then counted the reference down from 0 to wrap
	 * around (seconds per failure); it must be quick now */
	for (long n = 1; n <= 3; n++) {
		uint64_t start = os_gettime_ns();
		null_fail_next("device_vertexbuffer_create", n);
		assert_int_equal(gs_create(&graphics, NULL_GRAPHICS_MODULE, 0), GS_ERROR_FAIL);
		assert_null(gs_get_context());
		assert_true(os_gettime_ns() - start < 1000000000ULL);
	}
	null_reset_failures();
	null_reset_failures();

	assert_int_equal(gs_create(&graphics, NULL_GRAPHICS_MODULE, 0), GS_SUCCESS);
	gs_destroy(graphics);
	assert_int_equal(obs_test_teardown(state), 0);
}

static void objects_test(void **state)
{
	UNUSED_PARAMETER(state);

	null_fail_next("device_texture_create", 1);
	assert_null(gs_texture_create(16, 16, GS_RGBA, 1, NULL, 0));
	null_fail_next("device_cubetexture_create", 1);
	assert_null(gs_cubetexture_create(16, GS_RGBA, 1, NULL, 0));
	null_fail_next("device_voltexture_create", 1);
	assert_null(gs_voltexture_create(4, 4, 4, GS_RGBA, 1, NULL, 0));
	null_fail_next("device_stagesurface_create", 1);
	assert_null(gs_stagesurface_create(16, 16, GS_RGBA));
	null_fail_next("device_zstencil_create", 1);
	assert_null(gs_zstencil_create(16, 16, GS_Z24_S8));
	struct gs_sampler_info si = {0};
	null_fail_next("device_samplerstate_create", 1);
	assert_null(gs_samplerstate_create(&si));
	null_fail_next("device_timer_create", 1);
	assert_null(gs_timer_create());
	null_fail_next("device_timer_range_create", 1);
	assert_null(gs_timer_range_create());

	/* mapping */
	gs_texture_t *tex = gs_texture_create(16, 16, GS_RGBA, 1, NULL, GS_DYNAMIC);
	uint8_t *ptr;
	uint32_t linesize;
	null_fail_next("gs_texture_map", 1);
	assert_false(gs_texture_map(tex, &ptr, &linesize));
	assert_true(gs_texture_map(tex, &ptr, &linesize));
	gs_texture_unmap(tex);
	/* set_image maps the texture */
	uint8_t pixels[16 * 16 * 4] = {0};
	null_fail_next("gs_texture_map", 1);
	gs_texture_set_image(tex, pixels, 16 * 4, false);
	gs_texture_set_image(tex, pixels, 16 * 4, true);
	gs_texture_destroy(tex);

	gs_stagesurf_t *surf = gs_stagesurface_create(16, 16, GS_RGBA);
	null_fail_next("gs_stagesurface_map", 1);
	assert_false(gs_stagesurface_map(surf, &ptr, &linesize));
	gs_stagesurface_destroy(surf);
}

static void texrender_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* the render target, or its depth buffer, cannot be created */
	gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	null_fail_next("device_texture_create", 1);
	assert_false(gs_texrender_begin(tr, 32, 32));
	assert_null(gs_texrender_get_texture(tr));
	/* Regression: the texrender stayed without a target until the size changed */
	assert_true(gs_texrender_begin(tr, 32, 32));
	gs_texrender_end(tr);
	gs_texrender_destroy(tr);

	tr = gs_texrender_create(GS_RGBA, GS_Z24_S8);
	null_fail_next("device_zstencil_create", 1);
	assert_false(gs_texrender_begin(tr, 32, 32));
	assert_true(gs_texrender_begin(tr, 32, 32));
	gs_texrender_end(tr);
	gs_texrender_destroy(tr);
}

static void geometry_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* saving immediate-mode geometry needs a new vertex buffer */
	gs_render_start(true);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(1.0f, 0.0f);
	gs_vertex2f(0.0f, 1.0f);
	null_fail_next("device_vertexbuffer_create", 1);
	assert_null(gs_render_save());

	struct gs_vb_data *vbd = gs_vbdata_create();
	vbd->num = 3;
	vbd->points = bzalloc(sizeof(struct vec3) * 3);
	null_fail_next("device_vertexbuffer_create", 1);
	/* the buffer owns the data, also when it fails */
	assert_null(gs_vertexbuffer_create(vbd, 0));

	uint16_t *indices = bzalloc(sizeof(uint16_t) * 3);
	null_fail_next("device_indexbuffer_create", 1);
	assert_null(gs_indexbuffer_create(GS_UNSIGNED_SHORT, indices, 3, 0));
}

static const char *simple_effect = "uniform float4x4 ViewProj;\n"
				   "uniform texture2d image;\n"
				   "sampler_state s { Filter = Linear; };\n"
				   "struct V { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
				   "V VS(V v) { V o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; "
				   "return o; }\n"
				   "float4 PS(V v) : TARGET { return image.Sample(s, v.uv); }\n"
				   "technique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";

static void effect_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* a shader that does not compile fails the effect, without leaking */
	char *errors = NULL;
	null_fail_next("device_pixelshader_create", 1);
	assert_null(gs_effect_create(simple_effect, "fail_ps.effect", &errors));
	bfree(errors);
	errors = NULL;
	null_fail_next("device_vertexshader_create", 1);
	assert_null(gs_effect_create(simple_effect, "fail_vs.effect", &errors));
	bfree(errors);
	/* the sampler state of the effect */
	null_fail_next("device_samplerstate_create", 1);
	gs_effect_t *effect = gs_effect_create(simple_effect, "fail_ss.effect", NULL);
	gs_effect_destroy(effect);

	/* shaders from files */
	null_fail_next("device_vertexshader_create", 1);
	assert_null(gs_vertexshader_create_from_file(OBS_DATA_DIR "/default.effect", &errors));
	bfree(errors);
}

static void image_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* the image decodes, but its texture cannot be created */
	gs_image_file_ex_t image;
	gs_image_file_ex_init(&image, TEST_DATA_DIR "/rgba.png", GS_IMAGE_ALPHA_STRAIGHT);
	assert_true(image.loaded);
	null_fail_next("device_texture_create", 1);
	gs_image_file_ex_init_texture(&image);
	assert_null(image.texture);
	gs_image_file_ex_free(&image);

	null_fail_next("device_texture_create", 1);
	assert_null(gs_texture_create_from_file(TEST_DATA_DIR "/rgba.png"));
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(create_device_test), gs_unit_test(objects_test),  gs_unit_test(texrender_test),
		gs_unit_test(geometry_test),          gs_unit_test(effect_test),   gs_unit_test(image_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
