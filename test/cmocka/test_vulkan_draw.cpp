// Draw path: pipelines, buffers, samplers, state, descriptors, dynamic rendering. Each test renders into a small
// target and reads it back; the validation layer (with synchronization validation) must report nothing.
#include <stdio.h>
#include <stdlib.h>

#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "vulkan-subsystem.hpp"
#include "test_vulkan_shader_effect.h"

// cmocka.h redefines `inline`, so it must come after the C++ standard headers.
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
extern "C" {
#include <cmocka.h>
}

// CI sets OBS_VULKAN_REQUIRE_VALIDATION=1: a missing validation layer then fails instead of skipping the check.
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
static void (*set_render_target)(gs_device_t *, gs_texture_t *, gs_zstencil_t *);
static void (*clear)(gs_device_t *, uint32_t, const struct vec4 *, float, uint8_t);
static gs_stagesurf_t *(*stagesurface_create)(gs_device_t *, uint32_t, uint32_t, enum gs_color_format);
static void (*stage_texture)(gs_device_t *, gs_stagesurf_t *, gs_texture_t *);
static bool (*stagesurface_map)(gs_stagesurf_t *, uint8_t **, uint32_t *);
static void (*stagesurface_unmap)(gs_stagesurf_t *);
static void (*stagesurface_destroy)(gs_stagesurf_t *);
static void (*flush)(gs_device_t *);
static void (*set_vec4)(gs_sparam_t *, const struct vec4 *);
static void (*set_bool)(gs_sparam_t *, bool);
static void (*set_int)(gs_sparam_t *, int);
static void (*set_matrix4)(gs_sparam_t *, const struct matrix4 *);
static void (*set_texture)(gs_sparam_t *, gs_texture_t *);
static gs_vertbuffer_t *(*vertexbuffer_create)(gs_device_t *, struct gs_vb_data *, uint32_t);
static void (*vertexbuffer_destroy)(gs_vertbuffer_t *);
static void (*vertexbuffer_flush)(gs_vertbuffer_t *);
static gs_vb_data *(*vertexbuffer_get_data)(const gs_vertbuffer_t *);
static gs_indexbuffer_t *(*indexbuffer_create)(gs_device_t *, enum gs_index_type, void *, size_t, uint32_t);
static void (*indexbuffer_destroy)(gs_indexbuffer_t *);
static void (*load_vertexbuffer)(gs_device_t *, gs_vertbuffer_t *);
static void (*load_indexbuffer)(gs_device_t *, gs_indexbuffer_t *);
static void (*load_vertexshader)(gs_device_t *, gs_shader_t *);
static void (*load_pixelshader)(gs_device_t *, gs_shader_t *);
static void (*draw)(gs_device_t *, enum gs_draw_mode, uint32_t, uint32_t);
static void (*ortho)(gs_device_t *, float, float, float, float, float, float);
static void (*set_viewport)(gs_device_t *, int, int, int, int);
static void (*set_scissor_rect)(gs_device_t *, const struct gs_rect *);
static void (*set_cull_mode)(gs_device_t *, enum gs_cull_mode);
static void (*enable_blending)(gs_device_t *, bool);
static void (*blend_function)(gs_device_t *, enum gs_blend_type, enum gs_blend_type);
static void (*enable_framebuffer_srgb)(gs_device_t *, bool);

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
		  load(texture_destroy, "gs_texture_destroy") & load(set_render_target, "device_set_render_target") &
		  load(clear, "device_clear") & load(stagesurface_create, "device_stagesurface_create") &
		  load(stage_texture, "device_stage_texture") & load(stagesurface_map, "gs_stagesurface_map") &
		  load(stagesurface_unmap, "gs_stagesurface_unmap") &
		  load(stagesurface_destroy, "gs_stagesurface_destroy") & load(flush, "device_flush") &
		  load(vkt.vertexshader_create, "device_vertexshader_create") &
		  load(vkt.pixelshader_create, "device_pixelshader_create") &
		  load(vkt.get_param_by_name, "gs_shader_get_param_by_name") &
		  load(vkt.shader_destroy, "gs_shader_destroy") & load(set_vec4, "gs_shader_set_vec4") &
		  load(set_bool, "gs_shader_set_bool") & load(set_int, "gs_shader_set_int") &
		  load(set_matrix4, "gs_shader_set_matrix4") & load(set_texture, "gs_shader_set_texture") &
		  load(vertexbuffer_create, "device_vertexbuffer_create") &
		  load(vertexbuffer_destroy, "gs_vertexbuffer_destroy") &
		  load(vertexbuffer_flush, "gs_vertexbuffer_flush") &
		  load(vertexbuffer_get_data, "gs_vertexbuffer_get_data") &
		  load(indexbuffer_create, "device_indexbuffer_create") &
		  load(indexbuffer_destroy, "gs_indexbuffer_destroy") &
		  load(load_vertexbuffer, "device_load_vertexbuffer") &
		  load(load_indexbuffer, "device_load_indexbuffer") &
		  load(load_vertexshader, "device_load_vertexshader") &
		  load(load_pixelshader, "device_load_pixelshader") & load(draw, "device_draw") &
		  load(ortho, "device_ortho") & load(set_viewport, "device_set_viewport") &
		  load(set_scissor_rect, "device_set_scissor_rect") & load(set_cull_mode, "device_set_cull_mode") &
		  load(enable_blending, "device_enable_blending") & load(blend_function, "device_blend_function") &
		  load(enable_framebuffer_srgb, "device_enable_framebuffer_srgb");
	return ok ? 0 : -1;
}

static int teardown(void **)
{
	os_dlclose(module);
	return 0;
}

static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
	_putenv_s(name, value);
#else
	setenv(name, value, 1);
#endif
}

// Runs fn on this thread; aborts the process when it takes longer than 5 s (a hang would stall ctest).
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

/* ---- fixture ---- */

constexpr uint32_t N = 4; // target size

struct fixture {
	gs_device *d = nullptr;
	gs_texture_t *rt = nullptr;
	std::vector<gs_shader_t *> shaders;
	std::vector<gs_vertbuffer_t *> vbs;
};

// A 4x4 RGBA render target, pixel-space ortho projection, full viewport, no culling (sprite winding varies).
static fixture *open_fixture(enum gs_color_format format = GS_RGBA)
{
	gs_device_t *device = nullptr;
	int ret = create(&device, 0);
	if (ret == GS_ERROR_NOT_SUPPORTED) {
		skip();
	}
	assert_int_equal(ret, GS_SUCCESS);
	auto f = new fixture;
	f->d = device;
	vkt.device = device;
	f->rt = texture_create(device, N, N, format, 1, nullptr, GS_RENDER_TARGET);
	assert_non_null(f->rt);
	set_render_target(device, f->rt, nullptr);
	ortho(device, 0.0f, (float)N, 0.0f, (float)N, -100.0f, 100.0f);
	set_viewport(device, 0, 0, N, N);
	set_cull_mode(device, GS_NEITHER);
	return f;
}

static void close_fixture(fixture *f)
{
	for (gs_vertbuffer_t *vb : f->vbs) {
		vertexbuffer_destroy(vb);
	}
	for (gs_shader_t *s : f->shaders) {
		vkt.shader_destroy(s);
	}
	texture_destroy(f->rt);
	destroy(f->d);
	vkt.device = nullptr;
	delete f;
	int n = messages();
	if (n < 0) {
		require_validation();
		print_message("validation layer missing, not checked\n");
	} else {
		assert_int_equal(n, 0);
	}
}

static gs_shader_t *shader(fixture *f, enum gs_shader_type type, const char *text)
{
	char *errors = nullptr;
	gs_shader_t *s = type == GS_SHADER_VERTEX ? vkt.vertexshader_create(f->d, text, "test", &errors)
						  : vkt.pixelshader_create(f->d, text, "test", &errors);
	if (!s) {
		print_message("%s\n", errors ? errors : "(no message)");
	}
	bfree(errors);
	assert_non_null(s);
	f->shaders.push_back(s);
	return s;
}

static void clear_to(fixture *f, float r, float g, float b, float a)
{
	struct vec4 c;
	vec4_set(&c, r, g, b, a);
	clear(f->d, GS_CLEAR_COLOR, &c, 1.0f, 0);
}

// Quad from (x0, y0) to (x1, y1) in pixels as a triangle strip, with uv 0..1 (as libobs's sprite buffer).
static gs_vertbuffer_t *quad(fixture *f, float x0, float y0, float x1, float y1, uint32_t flags = 0)
{
	gs_vb_data *v = gs_vbdata_create();
	v->num = 4;
	v->points = (vec3 *)bzalloc(sizeof(vec3) * 4);
	v->num_tex = 1;
	v->tvarray = (gs_tvertarray *)bzalloc(sizeof(gs_tvertarray));
	v->tvarray[0].width = 2;
	vec2 *uv = (vec2 *)bzalloc(sizeof(vec2) * 4);
	v->tvarray[0].array = uv;
	const float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
	for (int i = 0; i < 4; ++i) {
		vec3_set(&v->points[i], xs[i], ys[i], 0.0f);
		vec2_set(&uv[i], i & 1 ? 1.0f : 0.0f, i & 2 ? 1.0f : 0.0f);
	}
	gs_vertbuffer_t *vb = vertexbuffer_create(f->d, v, flags);
	assert_non_null(vb);
	f->vbs.push_back(vb);
	return vb;
}

static std::vector<uint8_t> readback(fixture *f, gs_texture_t *tex = nullptr, enum gs_color_format format = GS_RGBA)
{
	tex = tex ? tex : f->rt;
	gs_stagesurf_t *stage = stagesurface_create(f->d, N, N, format);
	assert_non_null(stage);
	stage_texture(f->d, stage, tex);
	std::vector<uint8_t> out;
	within_5s([&] {
		uint8_t *data;
		uint32_t linesize;
		if (stagesurface_map(stage, &data, &linesize)) {
			for (uint32_t y = 0; y < N; ++y) {
				out.insert(out.end(), data + y * linesize, data + y * linesize + N * 4);
			}
			stagesurface_unmap(stage);
		}
	});
	stagesurface_destroy(stage);
	assert_int_equal(out.size(), N * N * 4);
	return out;
}

static void expect_pixel(const std::vector<uint8_t> &img, uint32_t x, uint32_t y, int r, int g, int b, int a)
{
	const uint8_t *p = &img[(y * N + x) * 4];
	const int want[4] = {r, g, b, a};
	for (int i = 0; i < 4; ++i) {
		if (abs(p[i] - want[i]) > 1) {
			fail_msg("pixel (%u, %u): got %u %u %u %u, expected %d %d %d %d", x, y, p[0], p[1], p[2], p[3],
				 r, g, b, a);
		}
	}
}

static const char *vs_solid = "uniform float4x4 ViewProj;\n"
			      "struct V { float4 pos : POSITION; };\n"
			      "V main(V v) { V o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); return o; }\n";
static const char *ps_color = "uniform float4 color;\n"
			      "float4 main(float4 pos : POSITION) : TARGET { return color; }\n";

// Loads the solid pass with `color`; returns the pixel shader.
static gs_shader_t *use_solid(fixture *f, float r, float g, float b, float a)
{
	gs_shader_t *vs = shader(f, GS_SHADER_VERTEX, vs_solid);
	gs_shader_t *ps = shader(f, GS_SHADER_PIXEL, ps_color);
	struct vec4 c;
	vec4_set(&c, r, g, b, a);
	set_vec4(vkt.get_param_by_name(ps, "color"), &c);
	load_vertexshader(f->d, vs);
	load_pixelshader(f->d, ps);
	return ps;
}

/* ---- pipeline cache ---- */

static void test_same_state_returns_same_pipeline(void **)
{
	fixture *f = open_fixture();
	use_solid(f, 1, 0, 0, 1);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	draw(f->d, GS_TRISTRIP, 0, 0);
	assert_int_equal(f->d->pipelines.pipelines.size(), 1);
	VkPipeline first = f->d->pipelines.pipelines.begin()->second.pipeline;
	assert_non_null(first);
	draw(f->d, GS_TRISTRIP, 0, 0);
	assert_int_equal(f->d->pipelines.pipelines.size(), 1);
	assert_true(f->d->pipelines.pipelines.begin()->second.pipeline == first);
	// Viewport and scissor are dynamic: no new pipeline either.
	set_viewport(f->d, 0, 0, 2, 2);
	struct gs_rect r = {0, 0, 1, 1};
	set_scissor_rect(f->d, &r);
	draw(f->d, GS_TRISTRIP, 0, 0);
	assert_int_equal(f->d->pipelines.pipelines.size(), 1);
	set_scissor_rect(f->d, nullptr);
	readback(f);
	close_fixture(f);
}

static void test_different_blend_returns_different_pipeline(void **)
{
	fixture *f = open_fixture();
	use_solid(f, 1, 0, 0, 1);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	draw(f->d, GS_TRISTRIP, 0, 0);
	blend_function(f->d, GS_BLEND_ONE, GS_BLEND_ZERO);
	draw(f->d, GS_TRISTRIP, 0, 0);
	auto &p = f->d->pipelines.pipelines;
	assert_int_equal(p.size(), 2);
	assert_true(p.begin()->second.pipeline != std::next(p.begin())->second.pipeline);
	// Back to the first state: found again, not recompiled.
	blend_function(f->d, GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	f->d->state.src_a = GS_BLEND_ONE;
	draw(f->d, GS_TRISTRIP, 0, 0);
	assert_int_equal(p.size(), 2);
	readback(f);
	close_fixture(f);
}

/* ---- the default effect, blending, scissor ---- */

static void test_textured_quad_readback_matches_source(void **)
{
	fixture *f = open_fixture();
	uint8_t src[N * N * 4];
	for (uint32_t y = 0; y < N; ++y) {
		for (uint32_t x = 0; x < N; ++x) {
			uint8_t *p = &src[(y * N + x) * 4];
			p[0] = (uint8_t)(x * 64);
			p[1] = (uint8_t)(y * 64);
			p[2] = (uint8_t)((x + y) * 32);
			p[3] = 255;
		}
	}
	const uint8_t *levels[] = {src};
	gs_texture_t *tex = texture_create(f->d, N, N, GS_RGBA, 1, levels, 0);
	assert_non_null(tex);

	char *errors = nullptr;
	std::string path = std::string(OBS_SOURCE_DIR) + "/libobs/data/default.effect";
	gs_effect_t *effect = vkt_effect_create(path.c_str(), &errors);
	if (!effect) {
		print_message("%s\n", errors ? errors : "(no message)");
	}
	bfree(errors);
	assert_non_null(effect);
	gs_shader_t *vs = vkt_effect_shader(effect, "Draw", GS_SHADER_VERTEX);
	gs_shader_t *ps = vkt_effect_shader(effect, "Draw", GS_SHADER_PIXEL);
	set_texture(vkt.get_param_by_name(ps, "image"), tex);
	load_vertexshader(f->d, vs);
	load_pixelshader(f->d, ps);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	clear_to(f, 0, 0, 0, 0);
	draw(f->d, GS_TRISTRIP, 0, 0);
	std::vector<uint8_t> img = readback(f);
	for (uint32_t y = 0; y < N; ++y) {
		for (uint32_t x = 0; x < N; ++x) {
			const uint8_t *p = &src[(y * N + x) * 4];
			expect_pixel(img, x, y, p[0], p[1], p[2], p[3]);
		}
	}
	vkt_effect_destroy(effect); // destroys its shaders
	texture_destroy(tex);
	close_fixture(f);
}

// Default blend state (as D3D11): colour SRCALPHA/INVSRCALPHA, alpha ONE/INVSRCALPHA. Also orders the transfer clear
// before the colour attachment write (sync validation would report a missing barrier).
static void test_blend_alpha_matches_reference(void **)
{
	fixture *f = open_fixture();
	use_solid(f, 1, 0, 0, 0.5f);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	clear_to(f, 0, 0, 1, 1);
	draw(f->d, GS_TRISTRIP, 0, 0);
	std::vector<uint8_t> img = readback(f);
	expect_pixel(img, 0, 0, 128, 0, 128, 255);
	expect_pixel(img, 3, 3, 128, 0, 128, 255);
	close_fixture(f);
}

// The scissor limits drawing; as D3D11's ClearRenderTargetView, a clear ignores it.
static void test_scissor_limits_clear_area(void **)
{
	fixture *f = open_fixture();
	use_solid(f, 0, 1, 0, 1);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	clear_to(f, 1, 0, 0, 1);
	struct gs_rect r = {1, 1, 2, 2};
	set_scissor_rect(f->d, &r);
	draw(f->d, GS_TRISTRIP, 0, 0);
	std::vector<uint8_t> img = readback(f);
	for (uint32_t y = 0; y < N; ++y) {
		for (uint32_t x = 0; x < N; ++x) {
			const bool in = x >= 1 && x < 3 && y >= 1 && y < 3;
			expect_pixel(img, x, y, in ? 0 : 255, in ? 255 : 0, 0, 255);
		}
	}
	clear_to(f, 0, 0, 1, 1);
	img = readback(f);
	expect_pixel(img, 0, 0, 0, 0, 255, 255);
	expect_pixel(img, 1, 1, 0, 0, 255, 255);
	set_scissor_rect(f->d, nullptr);
	close_fixture(f);
}

// Painted pixels of a 4x4 image as a row-major mask of '#' (painted) and '.' (clear colour).
static void expect_mask(const std::vector<uint8_t> &img, const char *mask, int r, int g, int b)
{
	for (uint32_t y = 0; y < N; ++y) {
		for (uint32_t x = 0; x < N; ++x) {
			const bool on = mask[y * N + x] == '#';
			expect_pixel(img, x, y, on ? r : 0, on ? g : 0, on ? b : 0, 255);
		}
	}
}

// D3D11 semantics: scissor rects are in pixels, origin top-left, y down. {0, 0, 2, 1} is the left two pixels of the
// top row only; a flipped origin would paint the bottom row.
static void test_scissor_origin_is_top_left(void **)
{
	fixture *f = open_fixture();
	use_solid(f, 0, 1, 0, 1);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	clear_to(f, 0, 0, 0, 1);
	struct gs_rect r = {0, 0, 2, 1};
	set_scissor_rect(f->d, &r);
	draw(f->d, GS_TRISTRIP, 0, 0);
	set_scissor_rect(f->d, nullptr);
	expect_mask(readback(f),
		    "##.."
		    "...."
		    "...."
		    "....",
		    0, 255, 0);
	close_fixture(f);
}

// D3D11 semantics: the viewport's TopLeftY is pixels down from the top. Viewport (0, 1, 4, 2) receives the whole
// 4x4 ortho space; the quad covers its top-left quarter (0..2 by 0..2), i.e. x 0..2 and y 1..2 in pixels: two
// pixels of row 1. A flipped viewport would put them in row 2.
static void test_viewport_offset_and_non_square(void **)
{
	fixture *f = open_fixture();
	use_solid(f, 0, 1, 0, 1);
	load_vertexbuffer(f->d, quad(f, 0, 0, N / 2, N / 2));
	clear_to(f, 0, 0, 0, 1);
	set_viewport(f->d, 0, 1, N, 2);
	draw(f->d, GS_TRISTRIP, 0, 0);
	expect_mask(readback(f),
		    "...."
		    "##.."
		    "...."
		    "....",
		    0, 255, 0);

	// Viewport (0, 1, 4, 3) is not centred, so a bottom-left origin (GL style: y = 4 - 1 - 3) would give rows 0..2
	// instead of D3D11's rows 1..3. The quad spans the full ortho height and x 0..2.
	load_vertexbuffer(f->d, quad(f, 0, 0, N / 2, N));
	clear_to(f, 0, 0, 0, 1);
	set_viewport(f->d, 0, 1, N, 3);
	draw(f->d, GS_TRISTRIP, 0, 0);
	expect_mask(readback(f),
		    "...."
		    "##.."
		    "##.."
		    "##..",
		    0, 255, 0);
	close_fixture(f);
}

// Viewport (1, 1, 3, 3) maps the full quad to pixels x 1..4, y 1..4; scissor {0, 0, 3, 2} keeps x < 3, y < 2.
static void test_viewport_offset_with_scissor(void **)
{
	fixture *f = open_fixture();
	use_solid(f, 0, 1, 0, 1);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	clear_to(f, 0, 0, 0, 1);
	set_viewport(f->d, 1, 1, 3, 3);
	struct gs_rect r = {0, 0, 3, 2};
	set_scissor_rect(f->d, &r);
	draw(f->d, GS_TRISTRIP, 0, 0);
	set_scissor_rect(f->d, nullptr);
	expect_mask(readback(f),
		    "...."
		    ".##."
		    "...."
		    "....",
		    0, 255, 0);
	close_fixture(f);
}

// Sampling the current render target is a feedback loop: the draw is skipped (logged once), with no validation
// message, and the next valid draw works. An unset texture is skipped the same way.
static void test_feedback_loop_and_unset_texture_skip_draw(void **)
{
	fixture *f = open_fixture();
	gs_shader_t *vs = shader(f, GS_SHADER_VERTEX, vs_solid);
	gs_shader_t *ps =
		shader(f, GS_SHADER_PIXEL,
		       "uniform texture2d image;\n"
		       "float4 main(float4 pos : POSITION) : TARGET { return image.Load(int3(pos.xy, 0)); }\n");
	load_vertexshader(f->d, vs);
	load_pixelshader(f->d, ps);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	clear_to(f, 1, 0, 0, 1);
	draw(f->d, GS_TRISTRIP, 0, 0); // image unset
	draw(f->d, GS_TRISTRIP, 0, 0);
	set_texture(vkt.get_param_by_name(ps, "image"), f->rt);
	draw(f->d, GS_TRISTRIP, 0, 0); // feedback
	draw(f->d, GS_TRISTRIP, 0, 0);
	std::vector<uint8_t> img = readback(f);
	expect_pixel(img, 1, 1, 255, 0, 0, 255);
	assert_true(vkt.get_param_by_name(ps, "image")->warned);
	assert_true(vkt.get_param_by_name(ps, "image")->feedback_warned);

	use_solid(f, 0, 0, 1, 1);
	draw(f->d, GS_TRISTRIP, 0, 0);
	expect_pixel(readback(f), 1, 1, 0, 0, 255, 255);
	close_fixture(f);
}

/* ---- shader interface ---- */

// The vertex stage's ViewProj (set 0) and the pixel stage's color (set 1) both arrive; the quad covers the left half.
static void test_vertex_and_pixel_params_both_apply(void **)
{
	fixture *f = open_fixture();
	use_solid(f, 0, 1, 0, 1);
	load_vertexbuffer(f->d, quad(f, 0, 0, N / 2, N));
	clear_to(f, 0, 0, 0, 1);
	draw(f->d, GS_TRISTRIP, 0, 0);
	std::vector<uint8_t> img = readback(f);
	expect_pixel(img, 0, 0, 0, 255, 0, 255);
	expect_pixel(img, 1, 3, 0, 255, 0, 255);
	expect_pixel(img, 2, 0, 0, 0, 0, 255);
	expect_pixel(img, 3, 3, 0, 0, 0, 255);
	close_fixture(f);
}

// A user matrix goes in untransposed (as D3D11/GL): mul(v, World)[i] = dot(World row i, v). This World halves x and
// adds 2 in row x, column w; transposed it would instead move w and draw elsewhere.
static void test_world_matrix_moves_vertex_to_expected_pixel(void **)
{
	fixture *f = open_fixture();
	gs_shader_t *vs = shader(f, GS_SHADER_VERTEX,
				 "uniform float4x4 ViewProj;\nuniform float4x4 World;\n"
				 "struct V { float4 pos : POSITION; };\n"
				 "V main(V v) { V o; o.pos = mul(mul(float4(v.pos.xyz, 1.0), World), ViewProj); "
				 "return o; }\n");
	gs_shader_t *ps = shader(f, GS_SHADER_PIXEL, ps_color);
	struct matrix4 w;
	matrix4_identity(&w);
	w.x.x = 0.5f;
	w.x.w = 2.0f;
	set_matrix4(vkt.get_param_by_name(vs, "World"), &w);
	struct vec4 c;
	vec4_set(&c, 1, 1, 1, 1);
	set_vec4(vkt.get_param_by_name(ps, "color"), &c);
	load_vertexshader(f->d, vs);
	load_pixelshader(f->d, ps);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	clear_to(f, 0, 0, 0, 1);
	draw(f->d, GS_TRISTRIP, 0, 0);
	std::vector<uint8_t> img = readback(f);
	expect_pixel(img, 1, 1, 0, 0, 0, 255);
	expect_pixel(img, 2, 1, 255, 255, 255, 255);
	expect_pixel(img, 3, 3, 255, 255, 255, 255);
	close_fixture(f);
}

// Vertex inputs declared in an order unlike the buffer's, and VS output / PS input structs whose members differ in
// order (and include a COLOR varying at location 16): values must arrive by semantic.
static void test_vertex_semantics_and_varyings_match_by_semantic(void **)
{
	fixture *f = open_fixture();
	gs_shader_t *vs =
		shader(f, GS_SHADER_VERTEX,
		       "uniform float4x4 ViewProj;\n"
		       "struct VI { float2 uv : TEXCOORD0; float3 tangent : TANGENT; float4 color : COLOR;\n"
		       "            float4 pos : POSITION; float3 normal : NORMAL; };\n"
		       "struct VO { float4 pos : POSITION; float4 a : TEXCOORD0; float4 c : COLOR;\n"
		       "            float4 b : TEXCOORD1; };\n"
		       "VO main(VI v) { VO o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj);\n"
		       "  o.a = float4(v.color.r, v.normal.y, 0.0, 0.0); o.b = float4(v.tangent.z, v.uv.x, 0, 0);\n"
		       "  o.c = float4(0.0, 0.0, 0.0, v.color.g); return o; }\n");
	gs_shader_t *ps = shader(f, GS_SHADER_PIXEL,
				 "struct PI { float4 b : TEXCOORD1; float4 c : COLOR; float4 pos : POSITION;\n"
				 "            float4 a : TEXCOORD0; };\n"
				 "float4 main(PI p) : TARGET { return float4(p.a.x, p.a.y + p.c.w, p.b.x, p.b.y); }\n");
	load_vertexshader(f->d, vs);
	load_pixelshader(f->d, ps);

	gs_vb_data *v = gs_vbdata_create();
	v->num = 4;
	v->points = (vec3 *)bzalloc(sizeof(vec3) * 4);
	v->normals = (vec3 *)bzalloc(sizeof(vec3) * 4);
	v->tangents = (vec3 *)bzalloc(sizeof(vec3) * 4);
	v->colors = (uint32_t *)bzalloc(sizeof(uint32_t) * 4);
	v->num_tex = 1;
	v->tvarray = (gs_tvertarray *)bzalloc(sizeof(gs_tvertarray));
	v->tvarray[0].width = 2;
	vec2 *uv = (vec2 *)bzalloc(sizeof(vec2) * 4);
	v->tvarray[0].array = uv;
	for (int i = 0; i < 4; ++i) {
		vec3_set(&v->points[i], i & 1 ? (float)N : 0.0f, i & 2 ? (float)N : 0.0f, 0.0f);
		vec3_set(&v->normals[i], 0.0f, 0.5f, 0.0f);
		vec3_set(&v->tangents[i], 0.0f, 0.0f, 0.25f);
		v->colors[i] = 0xFF0000FF; // 0xAABBGGRR: red, green 0
		vec2_set(&uv[i], 0.75f, 0.0f);
	}
	gs_vertbuffer_t *vb = vertexbuffer_create(f->d, v, 0);
	assert_non_null(vb);
	f->vbs.push_back(vb);
	load_vertexbuffer(f->d, vb);
	enable_blending(f->d, false);
	draw(f->d, GS_TRISTRIP, 0, 0);
	std::vector<uint8_t> img = readback(f);
	expect_pixel(img, 0, 0, 255, 128, 64, 191);
	expect_pixel(img, 3, 3, 255, 128, 64, 191);
	close_fixture(f);
}

static void test_bool_and_int_params(void **)
{
	fixture *f = open_fixture();
	gs_shader_t *vs = shader(f, GS_SHADER_VERTEX, vs_solid);
	gs_shader_t *ps = shader(f, GS_SHADER_PIXEL,
				 "uniform bool flag;\nuniform int count;\n"
				 "float4 main(float4 pos : POSITION) : TARGET {\n"
				 "  return float4(flag ? 1.0 : 0.0, float(count) / 4.0, 0.0, 1.0); }\n");
	load_vertexshader(f->d, vs);
	load_pixelshader(f->d, ps);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	set_bool(vkt.get_param_by_name(ps, "flag"), true);
	set_int(vkt.get_param_by_name(ps, "count"), 3);
	draw(f->d, GS_TRISTRIP, 0, 0);
	std::vector<uint8_t> img = readback(f);
	expect_pixel(img, 1, 1, 255, 191, 0, 255);
	set_bool(vkt.get_param_by_name(ps, "flag"), false);
	set_int(vkt.get_param_by_name(ps, "count"), 1);
	draw(f->d, GS_TRISTRIP, 0, 0);
	img = readback(f);
	expect_pixel(img, 1, 1, 0, 64, 0, 255);
	close_fixture(f);
}

// discard keeps the clear colour; SV_Position.xy is the pixel centre, top-left origin as D3D.
static void test_discard_leaves_clear_color(void **)
{
	fixture *f = open_fixture();
	gs_shader_t *vs = shader(f, GS_SHADER_VERTEX, vs_solid);
	gs_shader_t *ps = shader(f, GS_SHADER_PIXEL,
				 "float4 main(float4 pos : POSITION) : TARGET {\n"
				 "  if (pos.x < 2.0 || pos.y > 3.0) discard; return float4(0.0, 1.0, 0.0, 1.0); }\n");
	load_vertexshader(f->d, vs);
	load_pixelshader(f->d, ps);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	clear_to(f, 1, 0, 0, 1);
	draw(f->d, GS_TRISTRIP, 0, 0);
	std::vector<uint8_t> img = readback(f);
	expect_pixel(img, 1, 0, 255, 0, 0, 255);
	expect_pixel(img, 2, 0, 0, 255, 0, 255);
	expect_pixel(img, 3, 2, 0, 255, 0, 255);
	expect_pixel(img, 3, 3, 255, 0, 0, 255);
	close_fixture(f);
}

// Load (format_conversion, area, bicubic, deinterlace use it) at SV_Position.xy copies the texture 1:1.
static void test_load_copies_texels(void **)
{
	fixture *f = open_fixture();
	uint8_t src[N * N * 4];
	for (uint32_t i = 0; i < N * N * 4; ++i) {
		src[i] = (uint8_t)(i * 7 + 3);
	}
	const uint8_t *levels[] = {src};
	gs_texture_t *tex = texture_create(f->d, N, N, GS_RGBA, 1, levels, 0);
	gs_shader_t *vs = shader(f, GS_SHADER_VERTEX, vs_solid);
	gs_shader_t *ps =
		shader(f, GS_SHADER_PIXEL,
		       "uniform texture2d image;\n"
		       "float4 main(float4 pos : POSITION) : TARGET { return image.Load(int3(pos.xy, 0)); }\n");
	set_texture(vkt.get_param_by_name(ps, "image"), tex);
	load_vertexshader(f->d, vs);
	load_pixelshader(f->d, ps);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	enable_blending(f->d, false);
	draw(f->d, GS_TRISTRIP, 0, 0);
	std::vector<uint8_t> img = readback(f);
	assert_memory_equal(img.data(), src, sizeof(src));
	texture_destroy(tex);
	close_fixture(f);
}

// D3D's SV_Position.w is the clip w (gl_FragCoord.w is 1/w): with w = 2, the pixel shader must see 2.
static void test_sv_position_w_is_clip_w(void **)
{
	fixture *f = open_fixture();
	gs_shader_t *vs =
		shader(f, GS_SHADER_VERTEX,
		       "uniform float4x4 ViewProj;\n"
		       "struct V { float4 pos : POSITION; };\n"
		       "V main(V v) { V o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj) * 2.0; return o; }\n");
	gs_shader_t *ps =
		shader(f, GS_SHADER_PIXEL,
		       "float4 main(float4 pos : POSITION) : TARGET { return float4(pos.w / 4.0, 0, 0, 1); }\n");
	load_vertexshader(f->d, vs);
	load_pixelshader(f->d, ps);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	draw(f->d, GS_TRISTRIP, 0, 0);
	std::vector<uint8_t> img = readback(f);
	expect_pixel(img, 1, 1, 128, 0, 0, 255);
	close_fixture(f);
}

/* ---- rasterizer, targets ---- */

// As D3D11 (FrontCounterClockwise): counter-clockwise on screen is front-facing.
static void test_cull_modes_match_d3d11_winding(void **)
{
	fixture *f = open_fixture();
	use_solid(f, 1, 1, 1, 1);
	auto tri = [&](bool ccw) {
		gs_vb_data *v = gs_vbdata_create();
		v->num = 3;
		v->points = (vec3 *)bzalloc(sizeof(vec3) * 3);
		vec3_set(&v->points[0], 0, 0, 0);
		vec3_set(&v->points[1], ccw ? 0.0f : (float)N, ccw ? (float)N : 0.0f, 0); // TL, BL, TR is CCW on screen
		vec3_set(&v->points[2], ccw ? (float)N : 0.0f, ccw ? 0.0f : (float)N, 0);
		gs_vertbuffer_t *vb = vertexbuffer_create(f->d, v, 0);
		f->vbs.push_back(vb);
		return vb;
	};
	gs_vertbuffer_t *ccw = tri(true), *cw = tri(false);
	struct {
		gs_vertbuffer_t *vb;
		enum gs_cull_mode mode;
		bool visible;
	} cases[] = {{ccw, GS_BACK, true}, {ccw, GS_FRONT, false}, {ccw, GS_NEITHER, true},
		     {cw, GS_BACK, false}, {cw, GS_FRONT, true},   {cw, GS_NEITHER, true}};
	for (const auto &c : cases) {
		clear_to(f, 0, 0, 0, 1);
		set_cull_mode(f->d, c.mode);
		load_vertexbuffer(f->d, c.vb);
		draw(f->d, GS_TRIS, 0, 0);
		std::vector<uint8_t> img = readback(f);
		const int v = c.visible ? 255 : 0;
		expect_pixel(img, 0, 0, v, v, v, 255);
	}
	close_fixture(f);
}

// A texture rendered to and then sampled (colour attachment write, then shader read) in the same frame.
static void test_render_target_then_sample(void **)
{
	fixture *f = open_fixture();
	gs_texture_t *a = texture_create(f->d, N, N, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	set_render_target(f->d, a, nullptr);
	use_solid(f, 0.25f, 0.5f, 0.75f, 1);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	clear_to(f, 0, 0, 0, 1);
	draw(f->d, GS_TRISTRIP, 0, 0);

	set_render_target(f->d, f->rt, nullptr);
	gs_shader_t *ps =
		shader(f, GS_SHADER_PIXEL,
		       "uniform texture2d image;\n"
		       "float4 main(float4 pos : POSITION) : TARGET { return image.Load(int3(pos.xy, 0)); }\n");
	set_texture(vkt.get_param_by_name(ps, "image"), a);
	load_pixelshader(f->d, ps);
	draw(f->d, GS_TRISTRIP, 0, 0);
	std::vector<uint8_t> img = readback(f);
	expect_pixel(img, 2, 2, 64, 128, 191, 255);
	texture_destroy(a);
	close_fixture(f);
}

// BGRX's sampled views force alpha to one; as a render target it needs (and gets) an identity-swizzle view.
static void test_bgrx_render_target(void **)
{
	fixture *f = open_fixture(GS_BGRX);
	use_solid(f, 1, 0.5f, 0, 1);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	draw(f->d, GS_TRISTRIP, 0, 0);
	std::vector<uint8_t> img = readback(f, nullptr, GS_BGRX);
	expect_pixel(img, 1, 1, 0, 128, 255, 255); // B G R X bytes
	close_fixture(f);
}

// framebuffer sRGB: linear 0.5 is stored sRGB-encoded (188), as D3D11's sRGB render target view.
static void test_framebuffer_srgb_encodes(void **)
{
	fixture *f = open_fixture();
	use_solid(f, 0.5f, 0.5f, 0.5f, 1);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	enable_framebuffer_srgb(f->d, true);
	draw(f->d, GS_TRISTRIP, 0, 0);
	enable_framebuffer_srgb(f->d, false);
	std::vector<uint8_t> img = readback(f);
	expect_pixel(img, 0, 0, 188, 188, 188, 255);
	close_fixture(f);
}

/* ---- buffers ---- */

static void test_indexed_draw(void **)
{
	fixture *f = open_fixture();
	use_solid(f, 0, 0, 1, 1);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	uint16_t *idx = (uint16_t *)bmalloc(sizeof(uint16_t) * 3);
	idx[0] = 0;
	idx[1] = 1;
	idx[2] = 2; // the top-left triangle only
	gs_indexbuffer_t *ib = indexbuffer_create(f->d, GS_UNSIGNED_SHORT, idx, 3, 0);
	assert_non_null(ib);
	load_indexbuffer(f->d, ib);
	clear_to(f, 0, 0, 0, 1);
	draw(f->d, GS_TRIS, 0, 0);
	std::vector<uint8_t> img = readback(f);
	expect_pixel(img, 0, 0, 0, 0, 255, 255);
	expect_pixel(img, 3, 3, 0, 0, 0, 255);
	load_indexbuffer(f->d, nullptr);
	indexbuffer_destroy(ib);
	close_fixture(f);
}

// A dynamic buffer flushed between two draws of one frame: each draw keeps the data it was recorded with.
static void test_dynamic_vertex_buffer_flush_between_draws(void **)
{
	fixture *f = open_fixture();
	use_solid(f, 1, 1, 0, 1);
	gs_vertbuffer_t *vb = quad(f, 0, 0, N / 2, N / 2, GS_DYNAMIC);
	load_vertexbuffer(f->d, vb);
	clear_to(f, 0, 0, 0, 1);
	draw(f->d, GS_TRISTRIP, 0, 0);
	gs_vb_data *v = vertexbuffer_get_data(vb);
	for (int i = 0; i < 4; ++i) {
		v->points[i].x += N / 2;
		v->points[i].y += N / 2;
	}
	vertexbuffer_flush(vb);
	draw(f->d, GS_TRISTRIP, 0, 0);
	std::vector<uint8_t> img = readback(f);
	expect_pixel(img, 0, 0, 255, 255, 0, 255);
	expect_pixel(img, 3, 3, 255, 255, 0, 255);
	expect_pixel(img, 3, 0, 0, 0, 0, 255);
	// A later frame re-uploads the unchanged data into its own arena.
	clear_to(f, 0, 0, 0, 1);
	draw(f->d, GS_TRISTRIP, 0, 0);
	img = readback(f);
	expect_pixel(img, 3, 3, 255, 255, 0, 255);
	expect_pixel(img, 0, 0, 0, 0, 0, 255);
	close_fixture(f);
}

/* ---- frame resources and failure paths ---- */

// Consecutive draws into one target share a rendering instance; a clear (recorded outside it) ends it.
static void test_draws_batch_until_target_changes(void **)
{
	fixture *f = open_fixture();
	use_solid(f, 1, 0, 0, 1);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	draw(f->d, GS_TRISTRIP, 0, 0);
	VkImageView open = f->d->rendering_view;
	assert_true(open != VK_NULL_HANDLE);
	draw(f->d, GS_TRISTRIP, 0, 0);
	assert_true(f->d->rendering_view == open);
	clear_to(f, 0, 0, 0, 1);
	assert_true(f->d->rendering_view == VK_NULL_HANDLE);
	draw(f->d, GS_TRISTRIP, 0, 0);
	flush(f->d);
	assert_true(f->d->rendering_view == VK_NULL_HANDLE);
	close_fixture(f);
}

// More uniform data than one arena chunk in a frame: the arena grows instead of waiting, and every draw keeps its
// own values (the last one wins on screen).
static void test_upload_arena_grows_without_waiting(void **)
{
	fixture *f = open_fixture();
	gs_shader_t *ps = use_solid(f, 0, 0, 0, 1);
	gs_sparam_t *color = vkt.get_param_by_name(ps, "color");
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	enable_blending(f->d, false);
	const uint64_t before = f->d->graphics.next_value;
	// Each 16-byte block takes one alignment step; enough draws for more than one 1 MiB chunk.
	const VkDeviceSize step = std::max<VkDeviceSize>(f->d->limits.minUniformBufferOffsetAlignment, 16);
	const int count = (int)((1u << 20) / step) + 64;
	for (int i = 0; i < count; ++i) {
		struct vec4 c;
		vec4_set(&c, (float)(i % 256) / 255.0f, 0, 0, 1);
		set_vec4(color, &c);
		draw(f->d, GS_TRISTRIP, 0, 0);
	}
	assert_true(f->d->frames.frames[f->d->frames.index].arena.size() >= 2);
	assert_int_equal(f->d->graphics.next_value, before); // nothing was submitted (no wait on a slot)
	std::vector<uint8_t> img = readback(f);
	expect_pixel(img, 0, 0, (count - 1) % 256, 0, 0, 255);
	close_fixture(f);
}

// More COLOR/NORMAL varyings than the device allows: pipeline creation fails once, draws are logged no-ops.
static void test_varying_limit_fails_pipeline_not_draw(void **)
{
	fixture *f = open_fixture();
	gs_shader_t *vs = shader(f, GS_SHADER_VERTEX,
				 "uniform float4x4 ViewProj;\n"
				 "struct V { float4 pos : POSITION; float4 n : NORMAL; };\n"
				 "V main(V v) { V o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.n = o.pos; "
				 "return o; }\n");
	gs_shader_t *ps = shader(f, GS_SHADER_PIXEL,
				 "struct V { float4 pos : POSITION; float4 n : NORMAL; };\n"
				 "float4 main(V v) : TARGET { return v.n; }\n");
	assert_int_equal(vs->max_varying, 18);
	f->d->limits.maxVertexOutputComponents = 64; // the guaranteed minimum: locations 0-15
	load_vertexshader(f->d, vs);
	load_pixelshader(f->d, ps);
	gs_vb_data *v = gs_vbdata_create();
	v->num = 4;
	v->points = (vec3 *)bzalloc(sizeof(vec3) * 4);
	v->normals = (vec3 *)bzalloc(sizeof(vec3) * 4);
	gs_vertbuffer_t *vb = vertexbuffer_create(f->d, v, 0);
	f->vbs.push_back(vb);
	load_vertexbuffer(f->d, vb);
	clear_to(f, 0, 0, 1, 1);
	draw(f->d, GS_TRISTRIP, 0, 0);
	draw(f->d, GS_TRISTRIP, 0, 0);
	assert_int_equal(f->d->pipelines.pipelines.size(), 1);
	assert_null(f->d->pipelines.pipelines.begin()->second.pipeline);
	std::vector<uint8_t> img = readback(f);
	expect_pixel(img, 0, 0, 0, 0, 255, 255);
	close_fixture(f);
}

// A failed submit while a rendering instance is open: it is ended before the submit, later draws neither hang nor
// leak and render correctly again (contents before the failure are lost: the tracker restarts from UNDEFINED).
static void test_draw_after_failed_submit(void **)
{
	fixture *f = open_fixture();
	use_solid(f, 0, 1, 0, 1);
	load_vertexbuffer(f->d, quad(f, 0, 0, N, N));
	within_5s([&] {
		set_env("OBS_VULKAN_TEST_FAIL_SUBMIT", "1");
		clear_to(f, 1, 0, 0, 1);
		draw(f->d, GS_TRISTRIP, 0, 0);
		flush(f->d);
		draw(f->d, GS_TRISTRIP, 0, 0);
		flush(f->d);
		set_env("OBS_VULKAN_TEST_FAIL_SUBMIT", "0");
	});
	assert_int_equal(f->d->graphics.failed_values.size(), 2);
	assert_true(f->d->rendering_view == VK_NULL_HANDLE);
	enable_blending(f->d, false);
	draw(f->d, GS_TRISTRIP, 0, 0);
	std::vector<uint8_t> img = readback(f);
	expect_pixel(img, 1, 1, 0, 255, 0, 255);
	close_fixture(f);
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_same_state_returns_same_pipeline),
		cmocka_unit_test(test_different_blend_returns_different_pipeline),
		cmocka_unit_test(test_textured_quad_readback_matches_source),
		cmocka_unit_test(test_blend_alpha_matches_reference),
		cmocka_unit_test(test_scissor_limits_clear_area),
		cmocka_unit_test(test_scissor_origin_is_top_left),
		cmocka_unit_test(test_viewport_offset_and_non_square),
		cmocka_unit_test(test_viewport_offset_with_scissor),
		cmocka_unit_test(test_feedback_loop_and_unset_texture_skip_draw),
		cmocka_unit_test(test_vertex_and_pixel_params_both_apply),
		cmocka_unit_test(test_world_matrix_moves_vertex_to_expected_pixel),
		cmocka_unit_test(test_vertex_semantics_and_varyings_match_by_semantic),
		cmocka_unit_test(test_bool_and_int_params),
		cmocka_unit_test(test_discard_leaves_clear_color),
		cmocka_unit_test(test_load_copies_texels),
		cmocka_unit_test(test_sv_position_w_is_clip_w),
		cmocka_unit_test(test_cull_modes_match_d3d11_winding),
		cmocka_unit_test(test_render_target_then_sample),
		cmocka_unit_test(test_bgrx_render_target),
		cmocka_unit_test(test_framebuffer_srgb_encodes),
		cmocka_unit_test(test_indexed_draw),
		cmocka_unit_test(test_dynamic_vertex_buffer_flush_between_draws),
		cmocka_unit_test(test_draws_batch_until_target_changes),
		cmocka_unit_test(test_upload_arena_grows_without_waiting),
		cmocka_unit_test(test_varying_limit_fails_pipeline_not_draw),
		cmocka_unit_test(test_draw_after_failed_submit),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
