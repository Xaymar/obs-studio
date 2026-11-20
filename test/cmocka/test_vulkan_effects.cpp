// Effects through the generic libobs graphics API on the Vulkan module: gs_create("libobs-vulkan") runs
// graphics_init, every .effect in the tree is created with gs_effect_create_from_file and every pass is drawn once
// (so its pipeline is created against a real vertex layout and target), and texrender/stage/map round-trips.
// Runs under the validation layer with synchronization validation; it must report nothing.
#include <stdio.h>
#include <stdlib.h>

#include <ctype.h>

#include <algorithm>
#include <set>
#include <string>
#include <vector>

#include <graphics/graphics.h>
#include <graphics/effect.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <util/base.h>
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/platform.h>

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

static void *module; // our own reference: gs_destroy unloads its copy, the message counter must survive it
static int (*messages)(void);
static graphics_t *graphics;

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
	messages = (int (*)(void))os_dlsym(module, "obs_vulkan_validation_messages");
	return messages ? 0 : -1;
}

static int teardown(void **)
{
	os_dlclose(module);
	return 0;
}

// Starts a graphics subsystem on the Vulkan module; false (test skipped) without a Vulkan device.
static bool start()
{
	int r = gs_create(&graphics, OBS_VULKAN_MODULE, 0);
	if (r == GS_ERROR_NOT_SUPPORTED) {
		print_message("no Vulkan device, skipped\n");
		return false;
	}
	assert_int_equal(r, GS_SUCCESS);
	gs_enter_context(graphics);
	gs_set_cull_mode(GS_NEITHER); // as obs-video.c: sprites are clockwise on screen
	return true;
}

static void stop()
{
	gs_leave_context();
	gs_destroy(graphics);
	graphics = nullptr;
	int n = messages();
	if (n < 0) {
		require_validation();
		print_message("validation layer missing, not checked\n");
	} else {
		assert_int_equal(n, 0);
	}
}

// Collects LOG_ERROR lines while installed (the module logs through libobs's blog).
static std::vector<std::string> errors_seen;
static void collect_errors(int level, const char *format, va_list args, void *)
{
	char buf[4096];
	vsnprintf(buf, sizeof(buf), format, args);
	if (level <= LOG_ERROR) {
		errors_seen.push_back(buf);
	}
	fprintf(stderr, "%s\n", buf);
}

// Every .effect under `dir`, recursively.
static void add_effects(std::vector<std::string> &out, const std::string &dir)
{
	os_dir_t *d = os_opendir(dir.c_str());
	if (!d) {
		return;
	}
	for (os_dirent *e; (e = os_readdir(d));) {
		const std::string name = e->d_name, path = dir + "/" + name;
		if (e->directory) {
			if (name != "." && name != "..") {
				add_effects(out, path);
			}
		} else if (name.size() > 7 && name.compare(name.size() - 7, 7, ".effect") == 0) {
			out.push_back(path);
		}
	}
	os_closedir(d);
}

// Every .effect in the source tree (`find . -name '*.effect'`, without build copies): libobs/data, frontend/data,
// test/test-input/data and every plugins/*/data, each recursively.
static std::vector<std::string> all_effect_files()
{
	const std::string src = OBS_SOURCE_DIR;
	std::vector<std::string> files;
	add_effects(files, src + "/libobs/data");
	add_effects(files, src + "/frontend/data");
	add_effects(files, src + "/test/test-input/data");
	// platform-nix's os_opendir keeps the path pointer (no copy), so the string must outlive the walk.
	const std::string plugins = src + "/plugins";
	os_dir_t *dir = os_opendir(plugins.c_str());
	if (dir) {
		for (os_dirent *e; (e = os_readdir(dir));) {
			if (e->directory && e->d_name[0] != '.') {
				add_effects(files, src + "/plugins/" + e->d_name + "/data");
			}
		}
		os_closedir(dir);
	}
	std::sort(files.begin(), files.end());
	return files;
}

// Points, normals, tangents, colours and four float4 texcoord arrays: every input any effect's vertex shader reads.
static gs_vertbuffer_t *full_layout_triangle()
{
	gs_vb_data *vb = gs_vbdata_create();
	vb->num = 3;
	vb->points = (vec3 *)bzalloc(sizeof(vec3) * 3);
	vb->normals = (vec3 *)bzalloc(sizeof(vec3) * 3);
	vb->tangents = (vec3 *)bzalloc(sizeof(vec3) * 3);
	vb->colors = (uint32_t *)bzalloc(sizeof(uint32_t) * 3);
	vec3_set(&vb->points[0], 0.0f, 0.0f, 0.0f);
	vec3_set(&vb->points[1], 16.0f, 0.0f, 0.0f);
	vec3_set(&vb->points[2], 0.0f, 16.0f, 0.0f);
	vb->num_tex = 4;
	vb->tvarray = (gs_tvertarray *)bzalloc(sizeof(gs_tvertarray) * 4);
	for (size_t i = 0; i < 4; ++i) {
		vb->tvarray[i].width = 4;
		vb->tvarray[i].array = bzalloc(sizeof(vec4) * 3);
	}
	return gs_vertexbuffer_create(vb, 0);
}

// Names declared `texture3d <name>` in the file: the effect param type does not say a texture's dimension.
static std::set<std::string> texture3d_names(const std::string &path)
{
	std::set<std::string> names;
	char *text = os_quick_read_utf8_file(path.c_str());
	for (const char *p = text; p && (p = strstr(p, "texture3d"));) {
		p += strlen("texture3d");
		while (*p == ' ' || *p == '\t') {
			++p;
		}
		const char *end = p;
		while (isalnum((unsigned char)*end) || *end == '_') {
			++end;
		}
		names.emplace(p, end);
	}
	bfree(text);
	return names;
}

static bool expected_no_techniques(const std::string &path)
{
	// Include-only files: functions shared by other effects, no technique to draw.
	for (const char *name : {"/libobs/data/color.effect", "/libobs/data/deinterlace_base.effect",
				 "/plugins/obs-filters/data/color.effect", "/plugins/nv-filters/data/color.effect"}) {
		if (path.size() >= strlen(name) && !path.compare(path.size() - strlen(name), strlen(name), name)) {
			return true;
		}
	}
	return false;
}

static void test_all_effects_compile(void **)
{
	if (!start()) {
		return;
	}
	std::vector<std::string> files = all_effect_files();
	print_message("%zu effect files\n", files.size());
	// The same directories globbed by CMake at configure time: a new effect is drawn automatically, a vanished
	// one (or a broken walk) fails here. Reconfigure after adding or removing effects.
	assert_int_equal(files.size(), OBS_EFFECT_COUNT);

	gs_texture_t *target = gs_texture_create(16, 16, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	const uint8_t zero[4 * 4 * 4] = {};
	const uint8_t *zp = zero;
	gs_texture_t *tex = gs_texture_create(4, 4, GS_RGBA, 1, &zp, 0);
	const uint8_t *vp = zero; // 2x2x2 RGBA
	gs_texture_t *vol = gs_voltexture_create(2, 2, 2, GS_RGBA, 1, &vp, 0);
	gs_vertbuffer_t *vb = full_layout_triangle();
	assert_non_null(target);
	assert_non_null(tex);
	assert_non_null(vol);
	assert_int_equal(gs_get_texture_type(vol), GS_TEXTURE_3D);
	assert_int_equal(gs_voltexture_get_depth(vol), 2);
	assert_non_null(vb);

	log_handler_t old_handler;
	void *old_param;
	base_get_log_handler(&old_handler, &old_param);
	base_set_log_handler(collect_errors, nullptr);

	std::vector<std::string> failed;
	size_t passes = 0;
	for (const std::string &path : files) {
		errors_seen.clear();
		char *errors = nullptr;
		gs_effect_t *effect = gs_effect_create_from_file(path.c_str(), &errors);
		if (!effect) {
			failed.push_back(path + ": create: " + (errors ? errors : "(no message)"));
			bfree(errors);
			continue;
		}
		bfree(errors);
		const std::set<std::string> volumes = texture3d_names(path);
		if (!effect->techniques.num && !expected_no_techniques(path)) {
			failed.push_back(path + ": no techniques");
		}
		for (size_t t = 0; t < effect->techniques.num; ++t) {
			gs_technique_t *tech = &effect->techniques.array[t];
			for (size_t pi = 0; pi < tech->passes.num; ++pi) {
				// gs_technique_end clears every value: set the textures for each pass.
				for (size_t i = 0; i < gs_effect_get_num_params(effect); ++i) {
					gs_eparam_t *p = gs_effect_get_param_by_idx(effect, i);
					gs_effect_param_info info;
					gs_effect_get_param_info(p, &info);
					if (info.type == GS_SHADER_PARAM_TEXTURE) {
						gs_effect_set_texture(p, volumes.count(info.name) ? vol : tex);
					}
				}
				gs_set_render_target(target, nullptr);
				gs_set_viewport(0, 0, 16, 16);
				gs_ortho(0.0f, 16.0f, 0.0f, 16.0f, -100.0f, 100.0f);
				gs_technique_begin(tech);
				gs_technique_begin_pass(tech, pi);
				gs_load_vertexbuffer(vb);
				gs_load_indexbuffer(nullptr);
				gs_draw(GS_TRIS, 0, 3);
				gs_technique_end_pass(tech);
				gs_technique_end(tech);
				++passes;
			}
		}
		gs_flush();
		for (const std::string &e : errors_seen) {
			failed.push_back(path + ": " + e);
		}
		print_message("%s %s (%zu techniques)\n", errors_seen.empty() ? "PASS" : "FAIL",
			      path.c_str() + strlen(OBS_SOURCE_DIR) + 1, effect->techniques.num);
		gs_effect_destroy(effect);
	}
	base_set_log_handler(old_handler, old_param);

	gs_vertexbuffer_destroy(vb);
	gs_voltexture_destroy(vol);
	gs_texture_destroy(tex);
	gs_texture_destroy(target);
	print_message("%zu passes drawn\n", passes);
	for (const std::string &f : failed) {
		print_message("FAIL %s\n", f.c_str());
	}
	assert_int_equal(failed.size(), 0);
	stop();
}

static void read_rgba(gs_texture_t *tex, uint32_t w, uint32_t h, std::vector<uint8_t> &out,
		      enum gs_color_format format = GS_RGBA)
{
	gs_stagesurf_t *stage = gs_stagesurface_create(w, h, format);
	assert_non_null(stage);
	gs_stage_texture(stage, tex);
	uint8_t *data;
	uint32_t linesize;
	assert_true(gs_stagesurface_map(stage, &data, &linesize));
	out.resize(w * h * 4);
	for (uint32_t y = 0; y < h; ++y) {
		memcpy(&out[y * w * 4], data + y * linesize, w * 4);
	}
	gs_stagesurface_unmap(stage);
	gs_stagesurface_destroy(stage);
}

// Render into a texrender with the solid effect, sample its texture into a second target with the default effect
// through gs_draw_sprite, read that back.
static void test_texrender_roundtrip(void **)
{
	if (!start()) {
		return;
	}
	// Forward slashes: the effect parser cannot resolve #include next to a backslashed path.
	gs_effect_t *solid = gs_effect_create_from_file(OBS_SOURCE_DIR "/libobs/data/solid.effect", nullptr);
	gs_effect_t *def = gs_effect_create_from_file(OBS_SOURCE_DIR "/libobs/data/default.effect", nullptr);
	assert_non_null(solid);
	assert_non_null(def);

	gs_texrender_t *tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	assert_true(gs_texrender_begin(tr, 8, 8));
	struct vec4 clear;
	vec4_set(&clear, 0.0f, 0.0f, 1.0f, 1.0f); // blue
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, 8.0f, 0.0f, 8.0f, -100.0f, 100.0f);
	struct vec4 red;
	vec4_set(&red, 1.0f, 0.0f, 0.0f, 1.0f);
	gs_effect_set_vec4(gs_effect_get_param_by_name(solid, "color"), &red);
	gs_matrix_push();
	gs_matrix_identity();
	while (gs_effect_loop(solid, "Solid")) {
		gs_draw_sprite(nullptr, 0, 4, 4); // top-left quadrant
	}
	gs_matrix_pop();
	gs_texrender_end(tr);

	gs_texture_t *out = gs_texture_create(8, 8, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	gs_texture_t *prev = gs_get_render_target();
	gs_set_render_target(out, nullptr);
	gs_set_viewport(0, 0, 8, 8);
	gs_projection_push();
	gs_ortho(0.0f, 8.0f, 0.0f, 8.0f, -100.0f, 100.0f);
	struct vec4 black;
	vec4_zero(&black);
	gs_clear(GS_CLEAR_COLOR, &black, 0.0f, 0);
	gs_blend_state_push();
	gs_reset_blend_state();
	gs_enable_blending(false);
	gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), gs_texrender_get_texture(tr));
	while (gs_effect_loop(def, "Draw")) {
		gs_draw_sprite(gs_texrender_get_texture(tr), 0, 0, 0);
	}
	gs_blend_state_pop();
	gs_projection_pop();
	gs_set_render_target(prev, nullptr);

	std::vector<uint8_t> px;
	read_rgba(out, 8, 8, px);
	for (uint32_t y = 0; y < 8; ++y) {
		for (uint32_t x = 0; x < 8; ++x) {
			const uint8_t *p = &px[(y * 8 + x) * 4];
			const bool in = x < 4 && y < 4;
			const uint8_t want[4] = {(uint8_t)(in ? 255 : 0), 0, (uint8_t)(in ? 0 : 255), 255};
			if (memcmp(p, want, 4)) {
				fail_msg("(%u,%u) = %u %u %u %u, want %u %u %u %u", x, y, p[0], p[1], p[2], p[3],
					 want[0], want[1], want[2], want[3]);
			}
		}
	}

	gs_texture_destroy(out);
	gs_texrender_destroy(tr);
	gs_effect_destroy(solid);
	gs_effect_destroy(def);
	stop();
}

// A 2x2x2 volume texture sampled per slice with point filtering (color_grade_filter's clut_3d path).
static void test_volume_texture_samples_slices(void **)
{
	if (!start()) {
		return;
	}
	const char *text =
		"uniform float4x4 ViewProj;\n"
		"uniform texture3d vol;\n"
		"uniform float slice;\n"
		"sampler_state pt { Filter = Point; AddressU = Clamp; AddressV = Clamp; AddressW = Clamp; };\n"
		"struct V { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
		"V VS(V v) { V o; o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj); o.uv = v.uv; return o; }\n"
		"float4 PS(V v) : TARGET { return vol.Sample(pt, float3(v.uv, slice)); }\n"
		"technique Draw { pass { vertex_shader = VS(v); pixel_shader = PS(v); } }\n";
	char *errors = nullptr;
	gs_effect_t *effect = gs_effect_create(text, "volume", &errors);
	if (!effect) {
		print_message("%s\n", errors ? errors : "");
	}
	bfree(errors);
	assert_non_null(effect);
	uint8_t texels[2 * 2 * 2 * 4];
	for (uint8_t i = 0; i < 8; ++i) { // texel (x, y, z) = index x + 2y + 4z in red, 255 alpha
		texels[i * 4 + 0] = (uint8_t)(i * 30);
		texels[i * 4 + 1] = texels[i * 4 + 2] = 0;
		texels[i * 4 + 3] = 255;
	}
	const uint8_t *tp = texels;
	gs_texture_t *vol = gs_voltexture_create(2, 2, 2, GS_RGBA, 1, &tp, 0);
	assert_non_null(vol);
	gs_texture_t *out = gs_texture_create(2, 2, GS_RGBA, 1, nullptr, GS_RENDER_TARGET);
	for (int z = 0; z < 2; ++z) {
		gs_set_render_target(out, nullptr);
		gs_set_viewport(0, 0, 2, 2);
		gs_ortho(0.0f, 2.0f, 0.0f, 2.0f, -100.0f, 100.0f);
		gs_enable_blending(false);
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "vol"), vol);
		gs_effect_set_float(gs_effect_get_param_by_name(effect, "slice"), z ? 0.75f : 0.25f);
		while (gs_effect_loop(effect, "Draw")) {
			gs_draw_sprite(nullptr, 0, 2, 2);
		}
		std::vector<uint8_t> px;
		read_rgba(out, 2, 2, px);
		for (int i = 0; i < 4; ++i) {
			assert_int_equal(px[i * 4], (i + 4 * z) * 30);
		}
	}
	gs_enable_blending(true);
	gs_texture_destroy(out);
	gs_voltexture_destroy(vol);
	gs_effect_destroy(effect);
	stop();
}

// gs_copy_texture (whole texture) and gs_copy_texture_region (a 2x2 block to an offset), as obs-video.c's encoder
// texture copies use them.
static void test_copy_texture_and_region(void **)
{
	if (!start()) {
		return;
	}
	uint8_t a[4 * 4 * 4], b[4 * 4 * 4];
	for (int i = 0; i < 16; ++i) {
		a[i * 4] = (uint8_t)i, a[i * 4 + 1] = 10, a[i * 4 + 2] = 20, a[i * 4 + 3] = 255;
		b[i * 4] = 200, b[i * 4 + 1] = b[i * 4 + 2] = b[i * 4 + 3] = 0;
	}
	const uint8_t *pa = a, *pb = b;
	gs_texture_t *src = gs_texture_create(4, 4, GS_RGBA, 1, &pa, 0);
	gs_texture_t *whole = gs_texture_create(4, 4, GS_RGBA_UNORM, 1, nullptr, 0); // same copy format
	gs_texture_t *part = gs_texture_create(4, 4, GS_RGBA, 1, &pb, 0);
	gs_copy_texture(whole, src);
	gs_copy_texture_region(part, 2, 1, src, 1, 2, 2, 2); // src (1..2, 2..3) -> dst (2..3, 1..2)
	std::vector<uint8_t> px;
	read_rgba(whole, 4, 4, px, GS_RGBA_UNORM);
	assert_memory_equal(px.data(), a, sizeof(a));
	read_rgba(part, 4, 4, px);
	for (int y = 0; y < 4; ++y) {
		for (int x = 0; x < 4; ++x) {
			const bool in = x >= 2 && y >= 1 && y <= 2;
			const int want = in ? (y + 1) * 4 + (x - 1) : 200; // source index (x - 1, y + 1)
			assert_int_equal(px[(y * 4 + x) * 4], want);
		}
	}
	gs_texture_destroy(src);
	gs_texture_destroy(whole);
	gs_texture_destroy(part);
	stop();
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_volume_texture_samples_slices),
		cmocka_unit_test(test_copy_texture_and_region),
		cmocka_unit_test(test_texrender_roundtrip),
		cmocka_unit_test(test_all_effects_compile),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
