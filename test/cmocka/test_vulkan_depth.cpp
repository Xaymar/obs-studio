// Depth/stencil buffers and depth-tested drawing. Expected pixels follow D3D11 semantics: depth range [0, 1], LESS
// passes the smaller depth, stencil reference 0 (D3D11's OMSetDepthStencilState(state, 0)), masks 0xFF. Each test
// renders into a small target and reads it back; the validation layer (with synchronization validation) must report
// nothing.
#include <stdio.h>
#include <stdlib.h>

#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include <graphics/graphics.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>
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
static gs_zstencil_t *(*zstencil_create)(gs_device_t *, uint32_t, uint32_t, enum gs_zstencil_format);
static void (*zstencil_destroy)(gs_zstencil_t *);
static gs_zstencil_t *(*get_zstencil_target)(const gs_device_t *);
static void (*set_render_target)(gs_device_t *, gs_texture_t *, gs_zstencil_t *);
static void (*clear)(gs_device_t *, uint32_t, const struct vec4 *, float, uint8_t);
static gs_stagesurf_t *(*stagesurface_create)(gs_device_t *, uint32_t, uint32_t, enum gs_color_format);
static void (*stage_texture)(gs_device_t *, gs_stagesurf_t *, gs_texture_t *);
static bool (*stagesurface_map)(gs_stagesurf_t *, uint8_t **, uint32_t *);
static void (*stagesurface_unmap)(gs_stagesurf_t *);
static void (*stagesurface_destroy)(gs_stagesurf_t *);
static void (*flush)(gs_device_t *);
static gs_shader_t *(*vertexshader_create)(gs_device_t *, const char *, const char *, char **);
static gs_shader_t *(*pixelshader_create)(gs_device_t *, const char *, const char *, char **);
static gs_sparam_t *(*get_param_by_name)(gs_shader_t *, const char *);
static void (*shader_destroy)(gs_shader_t *);
static void (*set_vec4)(gs_sparam_t *, const struct vec4 *);
static gs_vertbuffer_t *(*vertexbuffer_create)(gs_device_t *, struct gs_vb_data *, uint32_t);
static void (*vertexbuffer_destroy)(gs_vertbuffer_t *);
static void (*load_vertexbuffer)(gs_device_t *, gs_vertbuffer_t *);
static void (*load_vertexshader)(gs_device_t *, gs_shader_t *);
static void (*load_pixelshader)(gs_device_t *, gs_shader_t *);
static void (*draw)(gs_device_t *, enum gs_draw_mode, uint32_t, uint32_t);
static void (*ortho)(gs_device_t *, float, float, float, float, float, float);
static void (*set_viewport)(gs_device_t *, int, int, int, int);
static void (*set_cull_mode)(gs_device_t *, enum gs_cull_mode);
static void (*enable_blending)(gs_device_t *, bool);
static void (*enable_color)(gs_device_t *, bool, bool, bool, bool);
static void (*enable_depth_test)(gs_device_t *, bool);
static void (*depth_function)(gs_device_t *, enum gs_depth_test);
static void (*enable_stencil_test)(gs_device_t *, bool);
static void (*enable_stencil_write)(gs_device_t *, bool);
static void (*stencil_function)(gs_device_t *, enum gs_stencil_side, enum gs_depth_test);
static void (*stencil_op)(gs_device_t *, enum gs_stencil_side, enum gs_stencil_op_type, enum gs_stencil_op_type,
			  enum gs_stencil_op_type);

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
		  load(texture_destroy, "gs_texture_destroy") & load(zstencil_create, "device_zstencil_create") &
		  load(zstencil_destroy, "gs_zstencil_destroy") &
		  load(get_zstencil_target, "device_get_zstencil_target") &
		  load(set_render_target, "device_set_render_target") & load(clear, "device_clear") &
		  load(stagesurface_create, "device_stagesurface_create") &
		  load(stage_texture, "device_stage_texture") & load(stagesurface_map, "gs_stagesurface_map") &
		  load(stagesurface_unmap, "gs_stagesurface_unmap") &
		  load(stagesurface_destroy, "gs_stagesurface_destroy") & load(flush, "device_flush") &
		  load(vertexshader_create, "device_vertexshader_create") &
		  load(pixelshader_create, "device_pixelshader_create") &
		  load(get_param_by_name, "gs_shader_get_param_by_name") & load(shader_destroy, "gs_shader_destroy") &
		  load(set_vec4, "gs_shader_set_vec4") & load(vertexbuffer_create, "device_vertexbuffer_create") &
		  load(vertexbuffer_destroy, "gs_vertexbuffer_destroy") &
		  load(load_vertexbuffer, "device_load_vertexbuffer") &
		  load(load_vertexshader, "device_load_vertexshader") &
		  load(load_pixelshader, "device_load_pixelshader") & load(draw, "device_draw") &
		  load(ortho, "device_ortho") & load(set_viewport, "device_set_viewport") &
		  load(set_cull_mode, "device_set_cull_mode") & load(enable_blending, "device_enable_blending") &
		  load(enable_color, "device_enable_color") & load(enable_depth_test, "device_enable_depth_test") &
		  load(depth_function, "device_depth_function") &
		  load(enable_stencil_test, "device_enable_stencil_test") &
		  load(enable_stencil_write, "device_enable_stencil_write") &
		  load(stencil_function, "device_stencil_function") & load(stencil_op, "device_stencil_op");
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

// Depth of a vertex at libobs z: the view matrix negates z (as D3D11's UpdateViewProjMatrix), then ortho(-100, 100)
// maps -z to (-z + 100) / 200. So NEAR_Z gives depth 0.25 and FAR_Z gives 0.75.
constexpr float NEAR_Z = 50.0f, FAR_Z = -50.0f;

struct fixture {
	gs_device *d = nullptr;
	gs_texture_t *rt = nullptr;
	gs_zstencil_t *zs = nullptr;
	gs_shader_t *vs = nullptr, *ps = nullptr;
	std::vector<gs_vertbuffer_t *> vbs;
};

static const char *vs_solid = "uniform float4x4 ViewProj;\n"
			      "struct V { float4 pos : POSITION; };\n"
			      "V main(V v) { V o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); return o; }\n";
static const char *ps_color = "uniform float4 color;\n"
			      "float4 main(float4 pos : POSITION) : TARGET { return color; }\n";

static gs_shader_t *shader(fixture *f, enum gs_shader_type type, const char *text)
{
	char *errors = nullptr;
	gs_shader_t *s = type == GS_SHADER_VERTEX ? vertexshader_create(f->d, text, "test", &errors)
						  : pixelshader_create(f->d, text, "test", &errors);
	if (!s) {
		print_message("%s\n", errors ? errors : "(no message)");
	}
	bfree(errors);
	assert_non_null(s);
	return s;
}

// A 4x4 RGBA render target with a 4x4 `format` depth buffer (none for GS_ZS_NONE), pixel-space ortho, no culling, no
// blending, the solid-colour pass loaded. Depth test is on (LESS) by default, as D3D11.
static fixture *open_fixture(enum gs_zstencil_format format = GS_Z24_S8)
{
	gs_device_t *device = nullptr;
	int ret = create(&device, 0);
	if (ret == GS_ERROR_NOT_SUPPORTED) {
		skip();
	}
	assert_int_equal(ret, GS_SUCCESS);
	auto f = new fixture;
	f->d = device;
	f->rt = texture_create(device, N, N, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	assert_non_null(f->rt);
	if (format != GS_ZS_NONE) {
		f->zs = zstencil_create(device, N, N, format);
		assert_non_null(f->zs);
	}
	set_render_target(device, f->rt, f->zs);
	ortho(device, 0.0f, (float)N, 0.0f, (float)N, -100.0f, 100.0f);
	set_viewport(device, 0, 0, N, N);
	set_cull_mode(device, GS_NEITHER);
	enable_blending(device, false);
	f->vs = shader(f, GS_SHADER_VERTEX, vs_solid);
	f->ps = shader(f, GS_SHADER_PIXEL, ps_color);
	load_vertexshader(device, f->vs);
	load_pixelshader(device, f->ps);
	return f;
}

static void close_fixture(fixture *f)
{
	for (gs_vertbuffer_t *vb : f->vbs) {
		vertexbuffer_destroy(vb);
	}
	shader_destroy(f->vs);
	shader_destroy(f->ps);
	zstencil_destroy(f->zs);
	texture_destroy(f->rt);
	destroy(f->d);
	delete f;
	int n = messages();
	if (n < 0) {
		require_validation();
		print_message("validation layer missing, not checked\n");
	} else {
		assert_int_equal(n, 0);
	}
}

// Clears colour to black, depth to `depth`, stencil to `stencil`.
static void clear_all(fixture *f, float depth = 1.0f, uint8_t stencil = 0)
{
	struct vec4 c;
	vec4_set(&c, 0, 0, 0, 1);
	clear(f->d, GS_CLEAR_COLOR | GS_CLEAR_DEPTH | GS_CLEAR_STENCIL, &c, depth, stencil);
}

// Draws a quad from (x0, y0) to (x1, y1) in pixels at libobs depth z, in colour (r, g, b).
static void quad(fixture *f, float x0, float y0, float x1, float y1, float z, float r, float g, float b)
{
	gs_vb_data *v = gs_vbdata_create();
	v->num = 4;
	v->points = (vec3 *)bzalloc(sizeof(vec3) * 4);
	const float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
	for (int i = 0; i < 4; ++i) {
		vec3_set(&v->points[i], xs[i], ys[i], z);
	}
	gs_vertbuffer_t *vb = vertexbuffer_create(f->d, v, 0);
	assert_non_null(vb);
	f->vbs.push_back(vb);
	struct vec4 c;
	vec4_set(&c, r, g, b, 1);
	set_vec4(get_param_by_name(f->ps, "color"), &c);
	load_vertexbuffer(f->d, vb);
	draw(f->d, GS_TRISTRIP, 0, 0);
}

static std::vector<uint8_t> readback(fixture *f, gs_texture_t *tex = nullptr)
{
	gs_stagesurf_t *stage = stagesurface_create(f->d, N, N, GS_RGBA);
	assert_non_null(stage);
	stage_texture(f->d, stage, tex ? tex : f->rt);
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

// Row-major 4x4 mask: 'r', 'g', 'b' for red, green, blue; '.' for black.
static void expect_mask(const std::vector<uint8_t> &img, const char *mask)
{
	for (uint32_t y = 0; y < N; ++y) {
		for (uint32_t x = 0; x < N; ++x) {
			const char m = mask[y * N + x];
			const int want[4] = {m == 'r' ? 255 : 0, m == 'g' ? 255 : 0, m == 'b' ? 255 : 0, 255};
			const uint8_t *p = &img[(y * N + x) * 4];
			for (int i = 0; i < 4; ++i) {
				if (abs(p[i] - want[i]) > 1) {
					fail_msg("pixel (%u, %u): got %u %u %u %u, expected '%c'", x, y, p[0], p[1],
						 p[2], p[3], m);
				}
			}
		}
	}
}

static const char *all_red = "rrrrrrrrrrrrrrrr";
static const char *all_green = "gggggggggggggggg";
static const char *all_black = "................";
// Near red over x 0..3, far green over x 1..4: where they overlap (x 1..3) the near one wins.
static const char *near_red_wins = "rrrgrrrgrrrgrrrg";

/* ---- creation ---- */

static void zstencil_create_all_formats_or_fallback(void **)
{
	fixture *f = open_fixture(GS_ZS_NONE);
	assert_null(zstencil_create(f->d, N, N, GS_ZS_NONE));
	const struct {
		enum gs_zstencil_format format;
		VkFormat first, fallback;
	} cases[] = {{GS_Z16, VK_FORMAT_D16_UNORM, VK_FORMAT_D16_UNORM},
		     {GS_Z24_S8, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT},
		     {GS_Z32F, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT},
		     {GS_Z32F_S8X24, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}};
	int created = 0;
	for (const auto &c : cases) {
		gs_zstencil_t *zs = zstencil_create(f->d, N, N, c.format);
		if (!zs) {
			continue; // unsupported: a clean nullptr
		}
		++created;
		assert_true(zs->format == c.first || zs->format == c.fallback);
		const bool stencil = c.format == GS_Z24_S8 || c.format == GS_Z32F_S8X24;
		assert_int_equal(zs->aspect, VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0));
		// Usable: clear and depth-tested draw.
		set_render_target(f->d, f->rt, zs);
		assert_true(get_zstencil_target(f->d) == zs);
		clear_all(f);
		quad(f, 0, 0, N, N, NEAR_Z, 1, 0, 0);
		expect_mask(readback(f), all_red);
		set_render_target(f->d, f->rt, nullptr);
		zstencil_destroy(zs);
	}
	// D16 and D32 are mandatory depth attachment formats, and one of the two stencil formats is.
	assert_true(created >= 3);
	close_fixture(f);
}

/* ---- depth ---- */

static void depth_test_less_hides_farther_quad(void **)
{
	fixture *f = open_fixture();
	clear_all(f);
	quad(f, 0, 0, 3, N, NEAR_Z, 1, 0, 0);
	quad(f, 1, 0, N, N, FAR_Z, 0, 1, 0);
	expect_mask(readback(f), near_red_wins);
	// Reversed order: the near quad overwrites the far one, the result is the same.
	clear_all(f);
	quad(f, 1, 0, N, N, FAR_Z, 0, 1, 0);
	quad(f, 0, 0, 3, N, NEAR_Z, 1, 0, 0);
	expect_mask(readback(f), near_red_wins);
	close_fixture(f);
}

static void depth_test_off_overwrites_by_draw_order(void **)
{
	fixture *f = open_fixture();
	enable_depth_test(f->d, false);
	clear_all(f);
	quad(f, 0, 0, 3, N, NEAR_Z, 1, 0, 0);
	quad(f, 1, 0, N, N, FAR_Z, 0, 1, 0);
	expect_mask(readback(f), "rgggrgggrgggrggg");
	close_fixture(f);
}

// libobs has no depth write setter; D3D11's depthWriteEnabled is the same internal flag.
static void depth_write_off_does_not_occlude(void **)
{
	fixture *f = open_fixture();
	f->d->state.depth_write = 0;
	clear_all(f);
	quad(f, 0, 0, 3, N, NEAR_Z, 1, 0, 0);
	quad(f, 1, 0, N, N, FAR_Z, 0, 1, 0); // depth is still 1.0: passes LESS
	expect_mask(readback(f), "rgggrgggrgggrggg");
	close_fixture(f);
}

static void depth_func_greater_equal_and_always(void **)
{
	fixture *f = open_fixture();
	depth_function(f->d, GS_GREATER);
	clear_all(f, 0.5f);
	quad(f, 0, 0, N, N, NEAR_Z, 1, 0, 0); // 0.25 > 0.5 fails
	expect_mask(readback(f), all_black);
	quad(f, 0, 0, N, N, FAR_Z, 0, 1, 0); // 0.75 > 0.5 passes, depth becomes 0.75
	expect_mask(readback(f), all_green);
	quad(f, 0, 0, N, N, FAR_Z, 0, 0, 1); // 0.75 > 0.75 fails
	expect_mask(readback(f), all_green);
	depth_function(f->d, GS_GEQUAL);
	quad(f, 0, 0, N, N, FAR_Z, 0, 0, 1); // 0.75 >= 0.75 passes
	expect_mask(readback(f), "bbbbbbbbbbbbbbbb");
	depth_function(f->d, GS_ALWAYS);
	quad(f, 0, 0, N, N, NEAR_Z, 1, 0, 0);
	expect_mask(readback(f), all_red);
	close_fixture(f);
}

static void clear_depth_resets_occlusion(void **)
{
	fixture *f = open_fixture();
	clear_all(f);
	quad(f, 0, 0, N, N, NEAR_Z, 1, 0, 0);
	quad(f, 0, 0, N, N, FAR_Z, 0, 1, 0);
	expect_mask(readback(f), all_red);
	clear(f->d, GS_CLEAR_DEPTH, nullptr, 1.0f, 0); // colour kept
	quad(f, 0, 0, N, N, FAR_Z, 0, 1, 0);
	expect_mask(readback(f), all_green);
	close_fixture(f);
}

/* ---- stencil ---- */

// Writes stencil 0 (the reference, by REPLACE) over the left half with colour writes off, then paints where the
// stencil EQUALs the reference: the left half only.
static void stencil_test_masks_draw(void **)
{
	fixture *f = open_fixture();
	enable_depth_test(f->d, false);
	clear_all(f, 1.0f, 1);
	enable_stencil_test(f->d, true);
	enable_stencil_write(f->d, true);
	stencil_function(f->d, GS_STENCIL_BOTH, GS_ALWAYS);
	stencil_op(f->d, GS_STENCIL_BOTH, GS_KEEP, GS_KEEP, GS_REPLACE);
	enable_color(f->d, false, false, false, false);
	quad(f, 0, 0, 2, N, 0, 1, 0, 0);
	expect_mask(readback(f), all_black);
	enable_color(f->d, true, true, true, true);
	stencil_function(f->d, GS_STENCIL_BOTH, GS_EQUAL);
	stencil_op(f->d, GS_STENCIL_BOTH, GS_KEEP, GS_KEEP, GS_KEEP);
	quad(f, 0, 0, N, N, 0, 0, 1, 0);
	expect_mask(readback(f), "gg..gg..gg..gg..");
	close_fixture(f);
}

static void stencil_clear_value(void **)
{
	fixture *f = open_fixture();
	enable_depth_test(f->d, false);
	enable_stencil_test(f->d, true);
	stencil_function(f->d, GS_STENCIL_BOTH, GS_EQUAL);
	clear_all(f, 1.0f, 5);
	quad(f, 0, 0, N, N, 0, 1, 0, 0); // 0 == 5 fails
	expect_mask(readback(f), all_black);
	clear(f->d, GS_CLEAR_STENCIL, nullptr, 1.0f, 0);
	quad(f, 0, 0, N, N, 0, 1, 0, 0); // 0 == 0 passes
	expect_mask(readback(f), all_red);
	// 255 increments (wrapping, as D3D11's INCR) to 0 = the reference over the top half only.
	clear_all(f, 1.0f, 255);
	stencil_function(f->d, GS_STENCIL_BOTH, GS_ALWAYS);
	stencil_op(f->d, GS_STENCIL_BOTH, GS_KEEP, GS_KEEP, GS_INCR);
	enable_color(f->d, false, false, false, false);
	quad(f, 0, 0, N, 2, 0, 1, 0, 0);
	enable_color(f->d, true, true, true, true);
	stencil_function(f->d, GS_STENCIL_BOTH, GS_EQUAL);
	stencil_op(f->d, GS_STENCIL_BOTH, GS_KEEP, GS_KEEP, GS_KEEP);
	quad(f, 0, 0, N, N, 0, 0, 0, 1);
	expect_mask(readback(f), "bbbbbbbb........");
	close_fixture(f);
}

/* ---- targets, failures, pipelines ---- */

static void depth_texture_survives_render_target_switch(void **)
{
	fixture *f = open_fixture();
	gs_texture_t *b = texture_create(f->d, N, N, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	gs_zstencil_t *z2 = zstencil_create(f->d, N, N, GS_Z24_S8);
	assert_non_null(b);
	assert_non_null(z2);
	clear_all(f);
	quad(f, 0, 0, 2, N, NEAR_Z, 1, 0, 0);

	set_render_target(f->d, b, nullptr); // another target without depth
	assert_null(get_zstencil_target(f->d));
	struct vec4 black;
	vec4_set(&black, 0, 0, 0, 1);
	clear(f->d, GS_CLEAR_COLOR, &black, 1.0f, 0);
	quad(f, 0, 0, N, N, NEAR_Z, 0, 1, 0);

	set_render_target(f->d, f->rt, f->zs); // back: the near quad's depth is still there
	quad(f, 0, 0, N, N, FAR_Z, 0, 1, 0);
	expect_mask(readback(f), "rrggrrggrrggrrgg");
	expect_mask(readback(f, b), all_green);

	// Same colour target, another depth buffer: the rendering instance must change with it.
	quad(f, 0, 0, N, N, NEAR_Z, 0, 0, 1); // into f->zs: depth 0.25 everywhere now
	set_render_target(f->d, f->rt, z2);
	clear(f->d, GS_CLEAR_DEPTH, nullptr, 1.0f, 0);
	quad(f, 0, 0, N, N, FAR_Z, 1, 0, 0); // z2 is clear: passes
	expect_mask(readback(f), all_red);
	set_render_target(f->d, f->rt, f->zs);
	quad(f, 0, 0, N, N, FAR_Z, 0, 1, 0); // f->zs holds 0.25: fails
	expect_mask(readback(f), all_red);

	// Destroying the current depth buffer detaches it; drawing goes on without depth.
	set_render_target(f->d, f->rt, z2);
	zstencil_destroy(z2);
	assert_null(get_zstencil_target(f->d));
	quad(f, 0, 0, N, N, FAR_Z, 0, 0, 1);
	expect_mask(readback(f), "bbbbbbbbbbbbbbbb");
	texture_destroy(b);
	close_fixture(f);
}

static void failed_submit_with_depth_does_not_hang(void **)
{
	fixture *f = open_fixture();
	within_5s([&] {
		set_env("OBS_VULKAN_TEST_FAIL_SUBMIT", "1");
		clear_all(f);
		quad(f, 0, 0, N, N, NEAR_Z, 1, 0, 0);
		flush(f->d);
		quad(f, 0, 0, N, N, NEAR_Z, 1, 0, 0);
		flush(f->d);
		set_env("OBS_VULKAN_TEST_FAIL_SUBMIT", "0");
	});
	assert_int_equal(f->d->graphics.failed_values.size(), 2);
	assert_true(f->d->rendering_view == VK_NULL_HANDLE);
	// Depth contents are undefined after the failure: clear, then depth testing works again.
	clear_all(f);
	quad(f, 0, 0, 3, N, NEAR_Z, 1, 0, 0);
	quad(f, 1, 0, N, N, FAR_Z, 0, 1, 0);
	expect_mask(readback(f), near_red_wins);
	close_fixture(f);
}

static void pipeline_key_distinguishes_depth_state(void **)
{
	fixture *f = open_fixture();
	auto &p = f->d->pipelines.pipelines;
	clear_all(f);
	quad(f, 0, 0, N, N, NEAR_Z, 1, 0, 0);
	assert_int_equal(p.size(), 1);
	depth_function(f->d, GS_LEQUAL);
	quad(f, 0, 0, N, N, NEAR_Z, 0, 1, 0); // 0.25 <= 0.25 passes
	assert_int_equal(p.size(), 2);
	assert_true(p.begin()->second.pipeline != std::next(p.begin())->second.pipeline);
	expect_mask(readback(f), all_green);

	// No depth buffer, depth test on with a function nothing would pass: no effect (as D3D11 without a DSV), and the
	// depth state is not part of the key then.
	set_render_target(f->d, f->rt, nullptr);
	depth_function(f->d, GS_NEVER);
	quad(f, 0, 0, N, N, NEAR_Z, 0, 0, 1);
	const size_t n = p.size();
	expect_mask(readback(f), "bbbbbbbbbbbbbbbb");
	depth_function(f->d, GS_GREATER);
	enable_stencil_test(f->d, true);
	stencil_function(f->d, GS_STENCIL_BOTH, GS_NEVER);
	quad(f, 0, 0, N, N, NEAR_Z, 1, 0, 0);
	assert_int_equal(p.size(), n);
	expect_mask(readback(f), all_red);
	close_fixture(f);
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(zstencil_create_all_formats_or_fallback),
		cmocka_unit_test(depth_test_less_hides_farther_quad),
		cmocka_unit_test(depth_test_off_overwrites_by_draw_order),
		cmocka_unit_test(depth_write_off_does_not_occlude),
		cmocka_unit_test(depth_func_greater_equal_and_always),
		cmocka_unit_test(clear_depth_resets_occlusion),
		cmocka_unit_test(stencil_test_masks_draw),
		cmocka_unit_test(stencil_clear_value),
		cmocka_unit_test(depth_texture_survives_render_target_switch),
		cmocka_unit_test(failed_submit_with_depth_does_not_hang),
		cmocka_unit_test(pipeline_key_distinguishes_depth_state),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
