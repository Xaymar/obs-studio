// End-to-end: the real OBS video pipeline (obs_startup, obs_reset_video, a scene as program output, raw video
// callbacks) on a graphics module given on the command line, with the libobs effects from the source tree.
//
//   test_vulkan_video render <module path> vulkan|d3d11 <output file>
//                                                      renders, checks the device type and pixels, writes images
//   test_vulkan_video compare <file a> <file b>            per image and channel: max |a - b| <= 2
//   test_vulkan_video load <module path> vulkan|d3d11|d3d11-release <seconds>
//                          side load for the queue priority measurement (not run by ctest): a 1920x1080 NV12 60 fps
//                          canvas with a few scaled sources, every frame read back and copied out by a raw video
//                          callback. d3d11-release also does what release builds (GPU_PRIORITY_VAL set) do.
//
// ctest runs `render` for libobs-vulkan (under the validation layer with synchronization validation: zero messages)
// and for libobs-d3d11 (Windows) in separate processes, then `compare`. Nothing golden is committed.
//
// Scene (256x128 canvas): two colour sources as color_source_v3 (opaque red, 50% green overlapping it: linear sRGB
// blending), an 8x8 four-colour pattern upscaled 4x through the bilinear, bicubic, lanczos and point scale filters,
// a 64x64 pattern downscaled 0.625x through the area filter, a cropped pattern, an opaque-effect source and a
// premultiplied-alpha-effect source. Captured once as BGRA and once as NV12 (format_conversion.effect). A fade
// transition (plugins/obs-transitions fade_transition.effect, two textures, framebuffer sRGB) is rendered through
// the gs API inside obs_enter_graphics.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <algorithm>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winternl.h>
#include <d3d11.h>
#include <d3dkmthk.h>
#endif

#include <obs.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

// cmocka.h redefines `inline`, so it must come after the C++ standard headers.
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
extern "C" {
#include <cmocka.h>
}

static const uint32_t W = 256, H = 128;
static const char *arg_mode, *arg_a, *arg_b, *arg_device; // arg_device: "vulkan" or "d3d11" (render)

struct image {
	std::string name;
	uint32_t w = 0, h = 0, bpp = 0; // bytes per pixel
	std::vector<uint8_t> px;        // tightly packed rows
	uint8_t *at(uint32_t x, uint32_t y) { return &px[((size_t)y * w + x) * bpp]; }
};

/* ---- sRGB helpers (as libobs's gs_srgb_*) ---- */

static float decode(float u)
{
	return u <= 0.04045f ? u / 12.92f : powf((u + 0.055f) / 1.055f, 2.4f);
}

static float encode(float l)
{
	return l <= 0.0031308f ? l * 12.92f : 1.055f * powf(l, 1.0f / 2.4f) - 0.055f;
}

static uint8_t u8(float f)
{
	return (uint8_t)std::clamp((int)lroundf(f * 255.0f), 0, 255);
}

/* ---- test sources ---- */

// As color_source_v3 (plugins/image-source/color-source.c): solid effect, linear blending when translucent.
struct color_src {
	vec4 color, color_srgb;
	uint32_t w, h;
};

static void *color_create(obs_data_t *settings, obs_source_t *)
{
	auto c = new color_src;
	const uint32_t rgba = (uint32_t)obs_data_get_int(settings, "color");
	vec4_from_rgba(&c->color, rgba);
	vec4_from_rgba_srgb(&c->color_srgb, rgba);
	c->w = (uint32_t)obs_data_get_int(settings, "width");
	c->h = (uint32_t)obs_data_get_int(settings, "height");
	return c;
}

static void color_destroy(void *data)
{
	delete (color_src *)data;
}

static void color_render(void *data, gs_effect_t *)
{
	auto c = (color_src *)data;
	const bool linear_srgb = gs_get_linear_srgb() || c->color.w < 1.0f;
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear_srgb);
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), linear_srgb ? &c->color_srgb : &c->color);
	gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
	gs_technique_begin(tech);
	gs_technique_begin_pass(tech, 0);
	gs_draw_sprite(nullptr, 0, c->w, c->h);
	gs_technique_end_pass(tech);
	gs_technique_end(tech);
	gs_enable_framebuffer_srgb(previous);
}

static uint32_t color_width(void *data)
{
	return ((color_src *)data)->w;
}

static uint32_t color_height(void *data)
{
	return ((color_src *)data)->h;
}

// A texture drawn as image_source does (sRGB texture view when blending linearly), with the base effect named by
// the "effect" setting: OBS_EFFECT_DEFAULT, OBS_EFFECT_OPAQUE or OBS_EFFECT_PREMULTIPLIED_ALPHA.
struct tex_src {
	gs_texture_t *tex = nullptr;
	uint32_t w, h;
	enum obs_base_effect effect;
};

// "kind": 0 four 4x4 blocks red/green/blue/white (8x8), 1 the same with 32x32 blocks (64x64), 2 a uniform
// (100, 150, 200, 64) (8x8), 3 the premultiplied (50, 75, 100, 128) (8x8).
static std::vector<uint8_t> tex_pixels(int kind, uint32_t &size)
{
	size = kind == 1 ? 64 : 8;
	std::vector<uint8_t> px(size * size * 4);
	for (uint32_t y = 0; y < size; ++y) {
		for (uint32_t x = 0; x < size; ++x) {
			uint8_t *p = &px[(y * size + x) * 4];
			if (kind <= 1) {
				const bool r = x >= size / 2, b = y >= size / 2;
				const uint8_t c[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
				memcpy(p, c[(b ? 2 : 0) + (r ? 1 : 0)], 3);
				p[3] = 255;
			} else if (kind == 2) {
				p[0] = 100, p[1] = 150, p[2] = 200, p[3] = 64;
			} else {
				p[0] = 50, p[1] = 75, p[2] = 100, p[3] = 128;
			}
		}
	}
	return px;
}

static void *tex_create(obs_data_t *settings, obs_source_t *)
{
	auto t = new tex_src;
	std::vector<uint8_t> px = tex_pixels((int)obs_data_get_int(settings, "kind"), t->w);
	t->h = t->w;
	t->effect = (enum obs_base_effect)obs_data_get_int(settings, "effect");
	const uint8_t *data = px.data();
	obs_enter_graphics();
	t->tex = gs_texture_create(t->w, t->h, GS_RGBA, 1, &data, 0);
	obs_leave_graphics();
	return t;
}

static void tex_destroy(void *data)
{
	auto t = (tex_src *)data;
	obs_enter_graphics();
	gs_texture_destroy(t->tex);
	obs_leave_graphics();
	delete t;
}

static void tex_render(void *data, gs_effect_t *)
{
	auto t = (tex_src *)data;
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(gs_get_linear_srgb());
	gs_effect_t *effect = obs_get_base_effect(t->effect);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	if (gs_get_linear_srgb()) {
		gs_effect_set_texture_srgb(image, t->tex);
	} else {
		gs_effect_set_texture(image, t->tex);
	}
	while (gs_effect_loop(effect, "Draw")) {
		gs_draw_sprite(t->tex, 0, t->w, t->h);
	}
	gs_enable_framebuffer_srgb(previous);
}

static uint32_t tex_width(void *data)
{
	return ((tex_src *)data)->w;
}

static uint32_t tex_height(void *data)
{
	return ((tex_src *)data)->h;
}

static void register_sources()
{
	obs_source_info color = {};
	color.id = "test_color_source";
	color.type = OBS_SOURCE_TYPE_INPUT;
	color.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	color.get_name = [](void *) {
		return "test colour";
	};
	color.create = color_create;
	color.destroy = color_destroy;
	color.video_render = color_render;
	color.get_width = color_width;
	color.get_height = color_height;
	obs_register_source(&color);

	obs_source_info tex = color;
	tex.id = "test_texture_source";
	tex.get_name = [](void *) {
		return "test texture";
	};
	tex.create = tex_create;
	tex.destroy = tex_destroy;
	tex.video_render = tex_render;
	tex.get_width = tex_width;
	tex.get_height = tex_height;
	obs_register_source(&tex);
}

/* ---- scene ---- */

static obs_sceneitem_t *add(obs_scene_t *scene, obs_source_t *src, float x, float y, float scale,
			    enum obs_scale_type filter = OBS_SCALE_DISABLE)
{
	obs_sceneitem_t *item = obs_scene_add(scene, src);
	vec2 pos, s;
	vec2_set(&pos, x, y);
	vec2_set(&s, scale, scale);
	obs_sceneitem_set_pos(item, &pos);
	obs_sceneitem_set_scale(item, &s);
	obs_sceneitem_set_scale_filter(item, filter);
	obs_source_release(src);
	return item;
}

static const char *unique_name()
{
	static int n;
	static std::string name;
	name = "source " + std::to_string(++n);
	return name.c_str();
}

static obs_source_t *color_source(uint32_t rgba, int w, int h)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "color", rgba);
	obs_data_set_int(s, "width", w);
	obs_data_set_int(s, "height", h);
	obs_source_t *src = obs_source_create("test_color_source", unique_name(), s, nullptr);
	obs_data_release(s);
	return src;
}

static obs_source_t *texture_source(int kind, enum obs_base_effect effect)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_int(s, "kind", kind);
	obs_data_set_int(s, "effect", effect);
	obs_source_t *src = obs_source_create("test_texture_source", unique_name(), s, nullptr);
	obs_data_release(s);
	return src;
}

static obs_scene_t *build_scene()
{
	obs_scene_t *scene = obs_scene_create("scene");
	add(scene, color_source(0xFF0000FF, 64, 48), 8, 8, 1);   // opaque red (ABGR)
	add(scene, color_source(0x8000FF00, 64, 48), 40, 32, 1); // 50% green, overlapping it
	add(scene, texture_source(0, OBS_EFFECT_DEFAULT), 8, 72, 4, OBS_SCALE_BILINEAR);
	add(scene, texture_source(0, OBS_EFFECT_DEFAULT), 48, 72, 4, OBS_SCALE_BICUBIC);
	add(scene, texture_source(0, OBS_EFFECT_DEFAULT), 88, 72, 4, OBS_SCALE_LANCZOS);
	add(scene, texture_source(0, OBS_EFFECT_DEFAULT), 128, 72, 4, OBS_SCALE_POINT);
	add(scene, texture_source(1, OBS_EFFECT_DEFAULT), 168, 72, 0.625f, OBS_SCALE_AREA);
	add(scene, texture_source(2, OBS_EFFECT_OPAQUE), 120, 8, 4);
	add(scene, texture_source(3, OBS_EFFECT_PREMULTIPLIED_ALPHA), 160, 8, 4);
	obs_sceneitem_t *cropped = add(scene, texture_source(0, OBS_EFFECT_DEFAULT), 216, 8, 4, OBS_SCALE_POINT);
	obs_sceneitem_crop crop = {4, 0, 0, 0}; // left: the green and white blocks remain
	obs_sceneitem_set_crop(cropped, &crop);
	return scene;
}

/* ---- capture ---- */

struct capture {
	os_event_t *done;
	enum video_format format;
	int frames = 0;
	std::vector<image> last; // planes of the last frame kept
	bool frames_differ = false;
};

static const int skip_frames = 8, keep_frames = 3; // the first frames may predate the scene

static void on_frame(void *param, struct video_data *frame)
{
	auto c = (capture *)param;
	const int n = c->frames++;
	if (n < skip_frames || n >= skip_frames + keep_frames) {
		return;
	}
	std::vector<image> planes;
	if (c->format == VIDEO_FORMAT_BGRA) {
		planes.push_back({"bgra", W, H, 4});
	} else {
		planes.push_back({"nv12_y", W, H, 1});
		planes.push_back({"nv12_uv", W / 2, H / 2, 2});
	}
	for (size_t p = 0; p < planes.size(); ++p) {
		image &img = planes[p];
		img.px.resize((size_t)img.w * img.h * img.bpp);
		for (uint32_t y = 0; y < img.h; ++y) {
			memcpy(&img.px[(size_t)y * img.w * img.bpp], frame->data[p] + (size_t)y * frame->linesize[p],
			       (size_t)img.w * img.bpp);
		}
	}
	if (!c->last.empty()) {
		for (size_t p = 0; p < planes.size(); ++p) {
			c->frames_differ |= planes[p].px != c->last[p].px;
		}
	}
	c->last = planes;
	if (n == skip_frames + keep_frames - 1) {
		os_event_signal(c->done);
	}
}

static int reset_video(const char *module, enum video_format format)
{
	obs_video_info ovi = {};
	ovi.graphics_module = module;
	ovi.fps_num = 30;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = W;
	ovi.base_height = ovi.output_height = H;
	ovi.output_format = format;
	ovi.gpu_conversion = true;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	return obs_reset_video(&ovi);
}

static std::vector<image> capture_frames(enum video_format format)
{
	capture c;
	c.format = format;
	assert_int_equal(os_event_init(&c.done, OS_EVENT_TYPE_MANUAL), 0);
	obs_add_raw_video_callback(nullptr, on_frame, &c);
	const int r = os_event_timedwait(c.done, 20000);
	obs_remove_raw_video_callback(on_frame, &c);
	os_event_destroy(c.done);
	if (r != 0) {
		fail_msg("timed out after %d frames", c.frames);
	}
	assert_false(c.frames_differ); // a static scene: every kept frame is the same
	return c.last;
}

/* ---- analytic checks ---- */

static int analytic_failures = 0;

static void expect(image &img, uint32_t x, uint32_t y, std::vector<int> want, int tol)
{
	const uint8_t *p = img.at(x, y);
	bool ok = true;
	for (uint32_t c = 0; c < img.bpp; ++c) {
		ok &= abs((int)p[c] - want[c]) <= tol;
	}
	if (!ok) {
		std::string got, exp;
		for (uint32_t c = 0; c < img.bpp; ++c) {
			got += std::to_string(p[c]) + " ";
			exp += std::to_string(want[c]) + " ";
		}
		print_message("ANALYTIC FAIL %s (%u,%u): got %s want %s(tol %d)\n", img.name.c_str(), x, y, got.c_str(),
			      exp.c_str(), tol);
		++analytic_failures;
	}
}

// BGRA order. Interior points only (filter edges are compared against D3D11, not analytically).
static void check_bgra(image &img)
{
	const int half = u8(encode(0.5f));            // 50% green, linear blend
	expect(img, 2, 2, {0, 0, 0, 0}, 0);           // clear colour
	expect(img, 20, 20, {0, 0, 255, 255}, 1);     // red
	expect(img, 60, 44, {0, half, half, 255}, 1); // green over red
	expect(img, 90, 70, {0, half, 0, 128}, 1);    // green over nothing (y 32..80)
	const std::vector<std::vector<int>> blocks = {{0, 0, 255, 255},
						      {0, 255, 0, 255},
						      {255, 0, 0, 255},
						      {255, 255, 255, 255}}; // red, green, blue, white
	// 4x upscales: block centres (8, 8) in each 16x16 block. Not lanczos (x 88): its 3-texel lobes reach past the
	// 4-texel blocks and ring by design; it is compared against D3D11 only.
	for (uint32_t x0 : {8u, 48u, 128u}) {
		for (int b = 0; b < 4; ++b) {
			expect(img, x0 + 8 + (b & 1) * 16, 72 + 8 + (b >> 1) * 16, blocks[b], 1);
		}
	}
	for (int b = 0; b < 4; ++b) { // area: 64x64 -> 40x40, blocks of 20
		expect(img, 168 + 10 + (b & 1) * 20, 72 + 10 + (b >> 1) * 20, blocks[b], 1);
	}
	expect(img, 136, 24, {200, 150, 100, 255}, 1); // opaque effect: alpha forced to one
	// Premultiplied effect: the shader divides the (linearized) sample by its alpha; blended over transparent
	// black with SRCALPHA/INVSRCALPHA, ONE/INVSRCALPHA, in linear space.
	const float a = 128 / 255.0f;
	std::vector<int> pm(4);
	const float c[3] = {50, 75, 100};
	for (int i = 0; i < 3; ++i) {
		pm[2 - i] = u8(encode(a * std::min(decode(c[i] / 255.0f) / a, 1.0f)));
	}
	pm[3] = 128;
	expect(img, 176, 24, pm, 2);
	expect(img, 216 + 8, 8 + 8, {0, 255, 0, 255}, 0);      // cropped: green block on top
	expect(img, 216 + 8, 8 + 24, {255, 255, 255, 255}, 0); // white below it
}

// BT.709 limited range from non-linear RGB.
static int luma(float r, float g, float b)
{
	return (int)lroundf(16.0f + 219.0f * (0.2126f * r + 0.7152f * g + 0.0722f * b));
}

// BT.709 limited range Cb, Cr (as libobs/media-io/video-matrices.c builds the canvas matrix from Kr 0.2126,
// Kb 0.0722 for VIDEO_CS_709 / VIDEO_RANGE_PARTIAL): 128 + 224 (B' - Y') / (2 (1 - Kb)), 128 + 224 (R' - Y') /
// (2 (1 - Kr)).
static std::vector<int> chroma(float r, float g, float b)
{
	const float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
	return {(int)lroundf(128.0f + 224.0f * (b - y) / 1.8556f), (int)lroundf(128.0f + 224.0f * (r - y) / 1.5748f)};
}

// Luma at four flat points; chroma at the sample covering each (x / 2, y / 2): every luma texel that chroma
// sample averages over has the same colour there.
static void check_nv12(image &y, image &uv)
{
	const struct {
		uint32_t x, y;
		float r, g, b;
	} points[] = {{2, 2, 0, 0, 0},
		      {20, 20, 1, 0, 0},
		      {136, 24, 100 / 255.0f, 150 / 255.0f, 200 / 255.0f},
		      {128 + 8 + 16, 72 + 8 + 16, 1, 1, 1}};
	for (const auto &p : points) {
		expect(y, p.x, p.y, {luma(p.r, p.g, p.b)}, 1);
		expect(uv, p.x / 2, p.y / 2, chroma(p.r, p.g, p.b), 1);
	}
}

/* ---- gs-level transition ---- */

static image fade_transition()
{
	const uint32_t w = 64, h = 32;
	std::vector<uint8_t> a(w * h * 4), b(w * h * 4);
	for (uint32_t y = 0; y < h; ++y) {
		for (uint32_t x = 0; x < w; ++x) {
			uint8_t *pa = &a[(y * w + x) * 4], *pb = &b[(y * w + x) * 4];
			pa[0] = 200, pa[1] = 40, pa[2] = 20, pa[3] = 255;
			pb[0] = (uint8_t)(x * 4), pb[1] = (uint8_t)(y * 8), pb[2] = 230, pb[3] = 255;
		}
	}
	image img = {"fade", w, h, 4};
	obs_enter_graphics();
	gs_effect_t *effect = gs_effect_create_from_file(
		OBS_SOURCE_DIR "/plugins/obs-transitions/data/fade_transition.effect", nullptr);
	assert_non_null(effect);
	const uint8_t *pa = a.data(), *pb = b.data();
	gs_texture_t *ta = gs_texture_create(w, h, GS_RGBA, 1, &pa, 0);
	gs_texture_t *tb = gs_texture_create(w, h, GS_RGBA, 1, &pb, 0);
	gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	assert_true(gs_texrender_begin(tr, w, h));
	vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	gs_blend_state_push();
	gs_reset_blend_state();
	gs_enable_blending(false);
	gs_set_cull_mode(GS_NEITHER);
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true); // PSFade returns linear values, as the fade transition renders it
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "tex_a"), ta);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "tex_b"), tb);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "fade_val"), 0.25f);
	while (gs_effect_loop(effect, "Fade")) {
		gs_draw_sprite(ta, 0, w, h);
	}
	gs_enable_framebuffer_srgb(previous);
	gs_blend_state_pop();
	gs_texrender_end(tr);

	gs_stagesurf_t *stage = gs_stagesurface_create(w, h, GS_RGBA);
	gs_stage_texture(stage, gs_texrender_get_texture(tr));
	uint8_t *data;
	uint32_t linesize;
	assert_true(gs_stagesurface_map(stage, &data, &linesize));
	img.px.resize(w * h * 4);
	for (uint32_t y = 0; y < h; ++y) {
		memcpy(&img.px[y * w * 4], data + y * linesize, w * 4);
	}
	gs_stagesurface_unmap(stage);
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(tr);
	gs_texture_destroy(ta);
	gs_texture_destroy(tb);
	gs_effect_destroy(effect);
	obs_leave_graphics();

	for (uint32_t y = 0; y < h; y += 7) {
		for (uint32_t x = 0; x < w; x += 9) {
			std::vector<int> want(4);
			for (int c = 0; c < 4; ++c) {
				const float va = a[(y * w + x) * 4 + c] / 255.0f, vb = b[(y * w + x) * 4 + c] / 255.0f;
				want[c] = u8(va * 0.75f + vb * 0.25f);
			}
			expect(img, x, y, want, 1);
		}
	}
	return img;
}

/* ---- files ---- */

static void write_images(const char *path, const std::vector<image> &images)
{
	FILE *f = os_fopen(path, "wb");
	assert_non_null(f);
	for (const image &img : images) {
		const uint32_t hdr[4] = {(uint32_t)img.name.size(), img.w, img.h, img.bpp};
		fwrite(hdr, sizeof(hdr), 1, f);
		fwrite(img.name.data(), 1, img.name.size(), f);
		fwrite(img.px.data(), 1, img.px.size(), f);
	}
	fclose(f);
}

static bool read_images(const char *path, std::vector<image> &images)
{
	FILE *f = os_fopen(path, "rb");
	if (!f) {
		return false;
	}
	uint32_t hdr[4];
	while (fread(hdr, sizeof(hdr), 1, f) == 1) {
		image img;
		img.name.resize(hdr[0]);
		img.w = hdr[1], img.h = hdr[2], img.bpp = hdr[3];
		img.px.resize((size_t)img.w * img.h * img.bpp);
		if (fread(&img.name[0], 1, hdr[0], f) != hdr[0] ||
		    fread(img.px.data(), 1, img.px.size(), f) != img.px.size()) {
			fclose(f);
			return false;
		}
		images.push_back(std::move(img));
	}
	fclose(f);
	return true;
}

/* ---- priority measurement side load ---- */

static bool hags_enabled; // libobs-d3d11 logged that HAGS is on

static void load_log(int lvl, const char *fmt, va_list args, void *)
{
	char buf[4096];
	vsnprintf(buf, sizeof(buf), fmt, args);
	hags_enabled |= strstr(buf, "Hardware-Accelerated GPU Scheduling enabled") != nullptr;
	printf("%s: %s\n", lvl <= LOG_ERROR ? "error" : lvl <= LOG_WARNING ? "warning" : "info", buf);
}

struct load_stats {
	long frames = 0;
	std::vector<uint8_t> copy; // the encoder stand-in's destination
};

static void on_load_frame(void *param, struct video_data *frame)
{
	auto s = (load_stats *)param;
	s->copy.resize(1920 * 1080 * 3 / 2);
	for (uint32_t y = 0; y < 1080; ++y) {
		memcpy(&s->copy[y * 1920], frame->data[0] + (size_t)y * frame->linesize[0], 1920);
	}
	for (uint32_t y = 0; y < 540; ++y) {
		memcpy(&s->copy[1920 * 1080 + y * 1920], frame->data[1] + (size_t)y * frame->linesize[1], 1920);
	}
	++s->frames;
}

// What libobs-d3d11's set_priority does when built with GPU_PRIORITY_VAL (release builds; empty in this build, so
// the module itself does nothing). The value is not public; 7, the maximum, is assumed.
static void d3d11_release_priority()
{
#ifdef _WIN32
	obs_enter_graphics();
	auto dev = (ID3D11Device *)gs_get_device_obj();
	obs_leave_graphics();
	IDXGIDevice *dxgi = nullptr;
	dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi);
	const NTSTATUS st = D3DKMTSetProcessSchedulingPriorityClass(
		GetCurrentProcess(),
		hags_enabled ? D3DKMT_SCHEDULINGPRIORITYCLASS_HIGH : D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME);
	const HRESULT hr = st == 0 ? dxgi->SetGPUThreadPriority(7) : E_FAIL;
	printf("d3d11-release: HAGS %s, priority class %s: status 0x%lx, SetGPUThreadPriority(7): 0x%lx\n",
	       hags_enabled ? "on" : "off", hags_enabled ? "HIGH" : "REALTIME", (unsigned long)st, (unsigned long)hr);
	dxgi->Release();
#endif
}

static int run_load(const char *module, const char *kind, int seconds)
{
	base_set_log_handler(load_log, nullptr);
	if (!obs_startup("en-US", nullptr, nullptr)) {
		return 1;
	}
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
	obs_add_data_path(OBS_SOURCE_DIR "/libobs/data/");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
	register_sources();
	obs_video_info ovi = {};
	ovi.graphics_module = module;
	ovi.fps_num = 60;
	ovi.fps_den = 1;
	ovi.base_width = ovi.output_width = 1920;
	ovi.base_height = ovi.output_height = 1080;
	ovi.output_format = VIDEO_FORMAT_NV12;
	ovi.gpu_conversion = true;
	ovi.colorspace = VIDEO_CS_709;
	ovi.range = VIDEO_RANGE_PARTIAL;
	ovi.scale_type = OBS_SCALE_BICUBIC;
	if (obs_reset_video(&ovi) != OBS_VIDEO_SUCCESS) {
		fprintf(stderr, "obs_reset_video failed\n");
		return 1;
	}
	if (!strcmp(kind, "d3d11-release")) {
		d3d11_release_priority();
	}

	obs_scene_t *scene = obs_scene_create("load");
	add(scene, color_source(0xFF302010, 1920, 1080), 0, 0, 1);
	add(scene, texture_source(1, OBS_EFFECT_DEFAULT), 0, 0, 15, OBS_SCALE_BICUBIC);     // 960x960
	add(scene, texture_source(0, OBS_EFFECT_DEFAULT), 960, 0, 60, OBS_SCALE_LANCZOS);   // 480x480
	add(scene, texture_source(0, OBS_EFFECT_DEFAULT), 1440, 0, 60, OBS_SCALE_BILINEAR); // 480x480
	add(scene, texture_source(1, OBS_EFFECT_DEFAULT), 960, 480, 9, OBS_SCALE_BICUBIC);  // 576x576
	add(scene, color_source(0x80FF8000, 960, 540), 480, 270, 1);                        // 50% overlay
	obs_set_output_source(0, obs_scene_get_source(scene));

	load_stats stats;
	obs_add_raw_video_callback(nullptr, on_load_frame, &stats);
	printf("LOAD RUNNING\n");
	fflush(stdout);
	os_sleep_ms((uint32_t)seconds * 1000);
	obs_remove_raw_video_callback(on_load_frame, &stats);

	video_t *video = obs_get_video();
	printf("load %s: %ld frames received in %d s, rendered %u, lagged %u, output skipped %u\n", kind,
	       stats.frames, seconds, obs_get_total_frames(), obs_get_lagged_frames(),
	       video_output_get_skipped_frames(video));
#ifdef _WIN32
	FILETIME c, e, k, u;
	GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
	printf("load cpu %.1f s\n", ((((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
				    (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime)) /
					   1e7);
#endif
	obs_set_output_source(0, nullptr);
	obs_source_remove(obs_scene_get_source(scene));
	obs_scene_release(scene);
	obs_wait_for_destroy_queue();
	obs_shutdown();
	return 0;
}

/* ---- tests ---- */

static void test_render(void **)
{
	os_unlink(arg_b); // a skipped or failed render must not leave an older file for compare
	const bool vulkan = !strcmp(arg_device, "vulkan");
	assert_true(vulkan || !strcmp(arg_device, "d3d11"));
	// Our own reference keeps the module, and for Vulkan its validation counter, loaded past obs_shutdown. CMake
	// registers a render only for a module target that exists, so it must load.
	void *own = os_dlopen(arg_a);
	if (!own) {
		fail_msg("cannot load %s", arg_a);
	}
	auto messages = (int (*)(void))os_dlsym(own, "obs_vulkan_validation_messages");
	assert_true(!vulkan || messages);
	if (vulkan) {
#ifdef _WIN32
		_putenv_s("OBS_VULKAN_VALIDATION", "1");
#else
		setenv("OBS_VULKAN_VALIDATION", "1", 1);
#endif
	}

	assert_true(obs_startup("en-US", nullptr, nullptr));
	// Deprecated without a replacement; the only way to point libobs at its effects outside an install tree.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
	obs_add_data_path(OBS_SOURCE_DIR "/libobs/data/");
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
	register_sources();

	int r = reset_video(arg_a, VIDEO_FORMAT_BGRA);
	if (vulkan &&
	    r == OBS_VIDEO_NOT_SUPPORTED) { // no Vulkan device: ctest reports Skipped (SKIP_REGULAR_EXPRESSION)
		obs_shutdown();
		os_dlclose(own);
		print_message("no Vulkan device, skipped\n");
		skip();
	}
	assert_int_equal(r, OBS_VIDEO_SUCCESS); // D3D11 must come up: its target exists, so does the module
	obs_enter_graphics();
	const int type = gs_get_device_type();
	obs_leave_graphics();
	assert_int_equal(type, vulkan ? GS_DEVICE_VULKAN : GS_DEVICE_DIRECT3D_11);

	obs_scene_t *scene = build_scene();
	obs_set_output_source(0, obs_scene_get_source(scene));

	std::vector<image> images = capture_frames(VIDEO_FORMAT_BGRA);
	check_bgra(images[0]);

	assert_int_equal(reset_video(arg_a, VIDEO_FORMAT_NV12), OBS_VIDEO_SUCCESS);
	std::vector<image> nv12 = capture_frames(VIDEO_FORMAT_NV12);
	check_nv12(nv12[0], nv12[1]);
	images.insert(images.end(), nv12.begin(), nv12.end());

	images.push_back(fade_transition());

	obs_set_output_source(0, nullptr);
	// The main canvas holds the scene: remove it (as the frontend does) so it and its sources go before shutdown.
	obs_source_remove(obs_scene_get_source(scene));
	obs_scene_release(scene);
	obs_wait_for_destroy_queue();
	obs_shutdown();

	write_images(arg_b, images);
	assert_int_equal(analytic_failures, 0);
	if (vulkan) {
		const int n = messages();
		if (n < 0) {
			const char *rq = getenv("OBS_VULKAN_REQUIRE_VALIDATION");
			if (rq && !strcmp(rq, "1")) {
				fail_msg("validation layer required (OBS_VULKAN_REQUIRE_VALIDATION=1) but missing");
			}
			print_message("validation layer missing, not checked\n");
		} else {
			assert_int_equal(n, 0);
		}
	}
	os_dlclose(own);
}

static void test_compare(void **)
{
	// Never passes without comparing: a missing, empty or short file (a render that skipped or failed) fails.
	std::vector<image> a, b;
	if (!read_images(arg_a, a) || !read_images(arg_b, b)) {
		fail_msg("missing or unreadable output (%s or %s): a render did not complete", arg_a, arg_b);
	}
	const char *names[] = {"bgra", "nv12_y", "nv12_uv", "fade"};
	assert_int_equal(a.size(), 4);
	assert_int_equal(b.size(), 4);
	int failures = 0;
	for (size_t i = 0; i < a.size(); ++i) {
		assert_string_equal(a[i].name.c_str(), names[i]);
		assert_string_equal(b[i].name.c_str(), names[i]);
		assert_true(a[i].w && a[i].h && a[i].bpp);
		assert_true(a[i].w == b[i].w && a[i].h == b[i].h && a[i].bpp == b[i].bpp);
		std::vector<int> max(a[i].bpp, 0);
		size_t over = 0;
		uint32_t wx = 0, wy = 0;
		int worst = -1;
		for (uint32_t y = 0; y < a[i].h; ++y) {
			for (uint32_t x = 0; x < a[i].w; ++x) {
				const uint8_t *pa = a[i].at(x, y), *pb = b[i].at(x, y);
				int px_max = 0;
				for (uint32_t c = 0; c < a[i].bpp; ++c) {
					const int d = abs((int)pa[c] - (int)pb[c]);
					max[c] = std::max(max[c], d);
					px_max = std::max(px_max, d);
				}
				over += px_max > 2;
				if (px_max > worst) {
					worst = px_max, wx = x, wy = y;
				}
			}
		}
		std::string per;
		for (int m : max) {
			per += std::to_string(m) + " ";
		}
		print_message("%-8s %3ux%-3u max diff per channel: %s(worst at %u,%u), %zu pixels over 2\n",
			      a[i].name.c_str(), a[i].w, a[i].h, per.c_str(), wx, wy, over);
		failures += over > 0;
	}
	assert_int_equal(failures, 0);
}

int main(int argc, char **argv)
{
	if (argc == 5 && !strcmp(argv[1], "load")) {
		return run_load(argv[2], argv[3], atoi(argv[4]));
	}
	const bool render_mode = argc == 5 && !strcmp(argv[1], "render");
	if (!render_mode && !(argc == 4 && !strcmp(argv[1], "compare"))) {
		fprintf(stderr, "usage: %s render <module> vulkan|d3d11 <out> | compare <a> <b> | load <module> <kind> <s>\n", argv[0]);
		return 2;
	}
	arg_mode = argv[1], arg_a = argv[2];
	if (render_mode) {
		arg_device = argv[3], arg_b = argv[4];
	} else {
		arg_b = argv[3];
	}
	const struct CMUnitTest render[] = {cmocka_unit_test(test_render)};
	const struct CMUnitTest compare[] = {cmocka_unit_test(test_compare)};
	return strcmp(arg_mode, "render") ? cmocka_run_group_tests(compare, nullptr, nullptr)
					  : cmocka_run_group_tests(render, nullptr, nullptr);
}
