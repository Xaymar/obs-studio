#include <stdio.h>
#include <stdlib.h>

#include <chrono>
#include <deque>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <graphics/graphics.h>
#include <graphics/matrix3.h>
#include <graphics/matrix4.h>
#include <util/base.h>
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
static int (*glslang_refs)(void);
static int (*num_params)(const gs_shader_t *);
static gs_sparam_t *(*param_by_idx)(gs_shader_t *, uint32_t);
static void (*param_info)(const gs_sparam_t *, struct gs_shader_param_info *);
static gs_sparam_t *(*viewproj)(const gs_shader_t *);
static gs_sparam_t *(*world)(const gs_shader_t *);
static void (*set_float)(gs_sparam_t *, float);
static void (*set_matrix4)(gs_sparam_t *, const struct matrix4 *);
static void (*set_matrix3)(gs_sparam_t *, const struct matrix3 *);
static void (*set_val)(gs_sparam_t *, const void *, size_t);

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
		  load(messages, "obs_vulkan_validation_messages") & load(glslang_refs, "obs_vulkan_glslang_refs") &
		  load(vkt.vertexshader_create, "device_vertexshader_create") &
		  load(vkt.pixelshader_create, "device_pixelshader_create") &
		  load(vkt.get_param_by_name, "gs_shader_get_param_by_name") &
		  load(vkt.shader_destroy, "gs_shader_destroy") & load(num_params, "gs_shader_get_num_params") &
		  load(param_by_idx, "gs_shader_get_param_by_idx") & load(param_info, "gs_shader_get_param_info") &
		  load(viewproj, "gs_shader_get_viewproj_matrix") & load(world, "gs_shader_get_world_matrix") &
		  load(set_float, "gs_shader_set_float") & load(set_matrix4, "gs_shader_set_matrix4") &
		  load(set_matrix3, "gs_shader_set_matrix3") & load(set_val, "gs_shader_set_val");
	return ok ? 0 : -1;
}

static int teardown(void **)
{
	os_dlclose(module);
	return 0;
}

// Runs fn on this thread; aborts the process when it takes longer than 5 s, naming what was being compiled.
template<typename F> static void within_5s(const char *what, F fn)
{
	std::promise<void> done;
	std::thread watchdog([f = done.get_future(), what]() mutable {
		if (f.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
			fprintf(stderr, "watchdog: timed out after 5 s on: %s\n", what);
			fflush(stderr);
			_Exit(2);
		}
	});
	fn();
	done.set_value();
	watchdog.join();
}

static gs_shader_t *compile(enum gs_shader_type type, const char *text, const char *what, char **errors)
{
	gs_shader_t *s = nullptr;
	within_5s(what, [&] {
		s = type == GS_SHADER_VERTEX ? vkt.vertexshader_create(nullptr, text, what, errors)
					     : vkt.pixelshader_create(nullptr, text, what, errors);
	});
	return s;
}

static std::string effect_path(const char *name)
{
	return std::string(OBS_SOURCE_DIR) + "/libobs/data/" + name;
}

static gs_effect_t *parse_effect(const char *name, char **errors)
{
	std::string path = effect_path(name);
	gs_effect_t *effect = nullptr;
	within_5s(path.c_str(), [&] { effect = vkt_effect_create(path.c_str(), errors); });
	return effect;
}

static bool is_spirv(gs_shader_t *s)
{
	return s && !s->spirv.empty() && s->spirv[0] == 0x07230203;
}

static void test_solid_effect_vertex_and_pixel_compile(void **)
{
	// The brief names default.effect as the source; solid, opaque and premultiplied_alpha must pass too.
	for (const char *name : {"default.effect", "solid.effect", "opaque.effect", "premultiplied_alpha.effect"}) {
		std::vector<gs_shader_t *> seen;
		vkt.on_shader = nullptr;
		char *errors = nullptr;
		gs_effect_t *effect = parse_effect(name, &errors);
		if (!effect) {
			print_message("%s failed:\n%s\n", name, errors ? errors : "(no message)");
		}
		bfree(errors);
		assert_non_null(effect);
		gs_shader_t *vs = vkt_effect_shader(effect, name == std::string("solid.effect") ? "Solid" : "Draw",
						    GS_SHADER_VERTEX);
		gs_shader_t *ps = vkt_effect_shader(effect, name == std::string("solid.effect") ? "Solid" : "Draw",
						    GS_SHADER_PIXEL);
		assert_true(is_spirv(vs));
		assert_true(is_spirv(ps));
		assert_int_equal(vs->type, GS_SHADER_VERTEX);
		assert_int_equal(ps->type, GS_SHADER_PIXEL);
		vkt_effect_destroy(effect);
	}
}

static void test_invalid_hlsl_returns_error_not_crash(void **)
{
	// Rejected by the shared parser.
	char *errors = nullptr;
	gs_shader_t *s = compile(GS_SHADER_PIXEL, "this is not hlsl {", "garbage", &errors);
	assert_null(s);
	assert_non_null(errors);
	bfree(errors);

	// Parses, but glslang rejects it: the message names the line of our source and quotes it.
	const char *text = "uniform float4 color;\n"
			   "\n"
			   "float4 main(float4 pos : POSITION) : TARGET\n"
			   "{\n"
			   "\tfloat4 a = color;\n"
			   "\treturn a + undefined_thing;\n"
			   "}\n";
	errors = nullptr;
	s = compile(GS_SHADER_PIXEL, text, "undefined identifier", &errors);
	assert_null(s);
	assert_non_null(errors);
	print_message("%s\n", errors);
	assert_non_null(strstr(errors, "undefined_thing"));
	assert_non_null(strstr(errors, ":6:"));
	assert_non_null(strstr(errors, "return a + undefined_thing;"));
	bfree(errors);

	// The macro cycle guard leaves ordinary nested macros alone.
	s = compile(GS_SHADER_PIXEL,
		    "#define ONE 1.0\n#define WHITE float4(ONE, ONE, ONE, ONE)\n"
		    "float4 main(float4 pos : POSITION) : TARGET { return WHITE; }",
		    "nested macros", nullptr);
	assert_non_null(s);
	vkt.shader_destroy(s);

	// No error_string pointer: still no crash.
	assert_null(compile(GS_SHADER_VERTEX, "garbage", "garbage, no error pointer", nullptr));
}

static gs_sparam_t *named(gs_shader_t *s, const char *name)
{
	return vkt.get_param_by_name(s, name);
}

static void test_param_info_names_and_types_match_effect(void **)
{
	char *errors = nullptr;
	gs_effect_t *effect = parse_effect("default.effect", &errors);
	bfree(errors);
	assert_non_null(effect);
	gs_shader_t *vs = vkt_effect_shader(effect, "Draw", GS_SHADER_VERTEX);
	gs_shader_t *ps = vkt_effect_shader(effect, "Draw", GS_SHADER_PIXEL);

	struct gs_shader_param_info info;
	gs_sparam_t *vp = named(vs, "ViewProj");
	assert_non_null(vp);
	param_info(vp, &info);
	assert_string_equal(info.name, "ViewProj");
	assert_int_equal(info.type, GS_SHADER_PARAM_MATRIX4X4);
	assert_ptr_equal(viewproj(vs), vp);
	assert_null(world(vs));

	gs_sparam_t *image = named(ps, "image");
	assert_non_null(image);
	param_info(image, &info);
	assert_int_equal(info.type, GS_SHADER_PARAM_TEXTURE);
	assert_null(named(ps, "no_such_param"));

	// Every param is reachable by index and by its own name.
	for (gs_shader_t *s : {vs, ps}) {
		int n = num_params(s);
		assert_true(n > 0);
		for (int i = 0; i < n; ++i) {
			gs_sparam_t *p = param_by_idx(s, (uint32_t)i);
			param_info(p, &info);
			assert_ptr_equal(named(s, info.name), p);
		}
	}
	vkt_effect_destroy(effect);

	// The effect parser does not pass defaults to shaders (the effect applies them at draw), so use HLSL directly.
	gs_shader_t *s = compile(GS_SHADER_PIXEL,
				 "uniform float4 color = {1.0, 1.0, 1.0, 1.0};\n"
				 "float4 main(float4 pos : POSITION) : TARGET { return color; }\n",
				 "default value", nullptr);
	assert_non_null(s);
	gs_sparam_t *color = named(s, "color");
	assert_non_null(color);
	const float ones[4] = {1, 1, 1, 1};
	assert_memory_equal(s->uniforms.data() + color->offset, ones, sizeof(ones));
	vkt.shader_destroy(s);
}

static void test_set_float_writes_std140_offset(void **)
{
	// std140: a@0, b@16 (vec3 aligns to 16), c@28 (packs after the vec3), m@32, d@96 (array stride 16).
	const char *text = "uniform float a;\n"
			   "uniform float3 b;\n"
			   "uniform float c;\n"
			   "uniform float4x4 m;\n"
			   "uniform float d[2];\n"
			   "float4 main(float4 pos : POSITION) : TARGET\n"
			   "{\n"
			   "\treturn float4(a, b.x, c, d[0] + d[1]) + m[0];\n"
			   "}\n";
	char *errors = nullptr;
	gs_shader_t *s = compile(GS_SHADER_PIXEL, text, "std140 layout", &errors);
	if (!s) {
		print_message("%s\n", errors ? errors : "(no message)");
	}
	bfree(errors);
	assert_non_null(s);

	assert_int_equal(named(s, "a")->offset, 0);
	assert_int_equal(named(s, "b")->offset, 16);
	assert_int_equal(named(s, "c")->offset, 28);
	assert_int_equal(named(s, "m")->offset, 32);
	assert_int_equal(named(s, "d")->offset, 96);
	assert_true(s->uniforms.size() >= 128);

	s->dirty = false;
	set_float(named(s, "c"), 2.5f);
	assert_true(s->dirty);
	float c;
	memcpy(&c, s->uniforms.data() + 28, sizeof(c));
	assert_true(c == 2.5f);

	// Arrays: tightly packed input, written at the std140 stride.
	const float d[2] = {3.0f, 4.0f};
	set_val(named(s, "d"), d, sizeof(d));
	float d0, d1;
	memcpy(&d0, s->uniforms.data() + 96, sizeof(d0));
	memcpy(&d1, s->uniforms.data() + 112, sizeof(d1));
	assert_true(d0 == 3.0f && d1 == 4.0f);

	// Wrong size is ignored, not written past the block.
	s->dirty = false;
	set_val(named(s, "d"), d, 1);
	assert_false(s->dirty);

	struct matrix4 mat;
	matrix4_identity(&mat);
	set_matrix4(named(s, "m"), &mat);
	assert_memory_equal(s->uniforms.data() + 32, &mat, sizeof(mat));
	vkt.shader_destroy(s);
}

static struct {
	VkDevice dev;
	PFN_vkCreateShaderModule create;
	PFN_vkDestroyShaderModule destroy;
	int shaders, failures;
} module_check;

static int unsupported_warnings;

static void count_unsupported_warnings(int level, const char *format, va_list args, void *)
{
	char buf[1024];
	vsnprintf(buf, sizeof(buf), format, args);
	if (level == LOG_WARNING && strstr(buf, "'m3'")) {
		++unsupported_warnings;
	}
}

static void test_float3x3_param_warns_once_instead_of_silently_ignoring(void **)
{
	char *errors = nullptr;
	gs_shader_t *s = compile(GS_SHADER_PIXEL,
				 "uniform float3x3 m3;\n"
				 "float4 main(float4 pos : POSITION) : TARGET { return float4(m3[0], 1.0); }\n",
				 "float3x3 param", &errors);
	if (!s) {
		print_message("%s\n", errors ? errors : "(no message)");
	}
	bfree(errors);
	assert_non_null(s);
	gs_sparam_t *m3 = named(s, "m3");
	assert_non_null(m3);
	struct gs_shader_param_info info;
	param_info(m3, &info);
	assert_int_equal(info.type, GS_SHADER_PARAM_UNKNOWN); // shader-parser.c has no float3x3 param type

	log_handler_t old_handler;
	void *old_param;
	base_get_log_handler(&old_handler, &old_param);
	base_set_log_handler(count_unsupported_warnings, nullptr);
	unsupported_warnings = 0;
	struct matrix3 mat;
	matrix3_identity(&mat);
	s->dirty = false;
	set_matrix3(m3, &mat);
	set_matrix3(m3, &mat);
	const float raw[9] = {};
	set_val(m3, raw, sizeof(raw));
	base_set_log_handler(old_handler, old_param);

	assert_int_equal(unsupported_warnings, 1);
	assert_false(s->dirty);
	vkt.shader_destroy(s);
}

struct effect_case {
	const char *name;
	const char *known_unsupported; // reason, or nullptr when the effect must translate
};

static void test_core_effects_translate(void **)
{
	const effect_case cases[] = {
		{"default.effect", nullptr},
		{"default_rect.effect", nullptr},
		{"solid.effect", nullptr},
		{"opaque.effect", nullptr},
		{"premultiplied_alpha.effect", nullptr},
		{"repeat.effect", nullptr},
		{"area.effect", nullptr},
		{"bicubic_scale.effect", nullptr},
		{"lanczos_scale.effect", nullptr},
		{"bilinear_lowres_scale.effect", nullptr},
		{"format_conversion.effect", nullptr},
		{"deinterlace_blend.effect", nullptr},
		{"deinterlace_blend_2x.effect", nullptr},
		{"deinterlace_discard.effect", nullptr},
		{"deinterlace_discard_2x.effect", nullptr},
		{"deinterlace_linear.effect", nullptr},
		{"deinterlace_linear_2x.effect", nullptr},
		{"deinterlace_yadif.effect", nullptr},
		{"deinterlace_yadif_2x.effect", nullptr},
	};
	// A device holds the glslang process reference, as in OBS; without one every compile re-initializes glslang.
	gs_device_t *device = nullptr;
	if (create(&device, 0) != GS_SUCCESS) {
		device = nullptr;
		print_message("no Vulkan device: timings include glslang start-up per shader\n");
	}
	int failures = 0;
	for (const effect_case &c : cases) {
		char *errors = nullptr;
		uint64_t t0 = os_gettime_ns();
		gs_effect_t *effect = parse_effect(c.name, &errors);
		double ms = (os_gettime_ns() - t0) / 1e6;
		if (effect) {
			print_message("PASS %-30s %8.1f ms (%zu techniques)\n", c.name, ms,
				      vkt_effect_techniques(effect));
			vkt_effect_destroy(effect);
			if (c.known_unsupported) {
				print_message("     (listed known_unsupported, now passes: update the list)\n");
				++failures;
			}
		} else if (c.known_unsupported) {
			print_message("SKIP %-30s known_unsupported: %s\n", c.name, c.known_unsupported);
		} else {
			print_message("FAIL %-30s\n%s\n", c.name, errors ? errors : "(no message)");
			++failures;
		}
		bfree(errors);
	}
	assert_int_equal(failures, 0);
	if (!device) {
		return;
	}

	// Untimed second pass: every SPIR-V module goes through vkCreateShaderModule with the validation layer on.
#ifdef _WIN32
	void *loader = os_dlopen("vulkan-1");
#else
	void *loader = os_dlopen("libvulkan.so.1");
#endif
	assert_non_null(loader);
	auto get_instance_proc = (PFN_vkGetInstanceProcAddr)os_dlsym(loader, "vkGetInstanceProcAddr");
	auto get_device_proc = (PFN_vkGetDeviceProcAddr)get_instance_proc(device->instance, "vkGetDeviceProcAddr");
	module_check.dev = device->dev;
	module_check.create = (PFN_vkCreateShaderModule)get_device_proc(device->dev, "vkCreateShaderModule");
	module_check.destroy = (PFN_vkDestroyShaderModule)get_device_proc(device->dev, "vkDestroyShaderModule");
	module_check.shaders = module_check.failures = 0;
	vkt.on_shader = [](enum gs_shader_type, const char *, gs_shader_t *s) {
		if (!s) {
			return;
		}
		VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		ci.codeSize = s->spirv.size() * sizeof(uint32_t);
		ci.pCode = s->spirv.data();
		VkShaderModule m = VK_NULL_HANDLE;
		++module_check.shaders;
		if (module_check.create(module_check.dev, &ci, nullptr, &m) != VK_SUCCESS) {
			++module_check.failures;
			return;
		}
		module_check.destroy(module_check.dev, m, nullptr);
	};
	for (const effect_case &c : cases) {
		gs_effect_t *effect = parse_effect(c.name, nullptr);
		if (effect) {
			vkt_effect_destroy(effect);
		}
	}
	vkt.on_shader = nullptr;
	print_message("%d shader modules created\n", module_check.shaders);
	assert_int_equal(module_check.failures, 0);
	within_5s("device destroy", [&] { destroy(device); });
	os_dlclose(loader);
	int n = messages();
	if (n < 0) {
		require_validation();
		print_message("validation layer missing, not checked\n");
	} else {
		assert_int_equal(n, 0);
	}
}

struct malformed_case {
	const char *name;
	std::string text;
	const char *known_parser_hang;      // reason, or nullptr
	const char *expect_error = nullptr; // when set: must be rejected with an error containing this text
};

// A chain m0 -> m1 -> ... -> m<n-1> -> 1.0 used by main; expanding it recursively would overflow the stack.
static std::string macro_chain(int n)
{
	std::string text;
	for (int i = 0; i < n - 1; ++i) {
		text += "#define m" + std::to_string(i) + " m" + std::to_string(i + 1) + "\n";
	}
	text += "#define m" + std::to_string(n - 1) + " 1.0\n";
	return text + "float4 main(float4 pos : POSITION) : TARGET { return float4(m0, m0, m0, m0); }";
}

// a0 -> a1 a1, ..., a<n-1> -> a<n> a<n>, a<n> -> 1.0: only n + 1 deep, but a0 expands to 2^n tokens.
static std::string macro_doubling(int n)
{
	std::string text;
	for (int i = 0; i < n; ++i) {
		std::string next = "a" + std::to_string(i + 1);
		text += "#define a" + std::to_string(i) + " " + next + " " + next + "\n";
	}
	text += "#define a" + std::to_string(n) + " 1.0\n";
	return text + "float4 main(float4 pos : POSITION) : TARGET { return float4(a0, 1.0, 1.0, 1.0); }";
}

static void test_malformed_input_never_hangs_or_crashes(void **)
{
	const std::string body = "float4 main(float4 pos : POSITION) : TARGET { return float4(1.0, 1.0, 1.0, 1.0); }";
	std::string nul_text = "float4 main(float4 pos : POSITION) : TARGET { return float4(1.0,";
	nul_text += '\0';
	nul_text += " 1.0, 1.0, 1.0); }";
	const malformed_case cases[] = {
		{"empty string", "", nullptr},
		{"whitespace only", " \n\t\n", nullptr},
		{"unterminated block comment", body + " /* never closed", nullptr},
		{"unterminated block comment inside main",
		 "float4 main(float4 pos : POSITION) : TARGET { /* return float4(1.0, 1.0, 1.0, 1.0); }", nullptr},
		{"huge identifier",
		 "float4 main(float4 pos : POSITION) : TARGET { float " + std::string(1 << 20, 'a') +
			 " = 1.0; return float4(1.0, 1.0, 1.0, 1.0); }",
		 nullptr},
		{"deeply nested expression",
		 "float4 main(float4 pos : POSITION) : TARGET { return float4(" + std::string(100000, '(') + "1.0" +
			 std::string(100000, ')') + "); }",
		 nullptr},
		{"deeply nested braces",
		 "float4 main(float4 pos : POSITION) : TARGET { " + std::string(100000, '{') +
			 std::string(100000, '}') + " return float4(1.0, 1.0, 1.0, 1.0); }",
		 nullptr},
		{"unknown type", "uniform foo4 x;\nfloat4 main(float4 pos : POSITION) : TARGET { return x; }", nullptr},
		{"unknown return type", "bar4 main(float4 pos : POSITION) : TARGET { return 1.0; }", nullptr},
		{"missing main",
		 "float4 notmain(float4 pos : POSITION) : TARGET { return float4(1.0, 1.0, 1.0, 1.0); }", nullptr},
		{"unbalanced braces (missing close)",
		 "float4 main(float4 pos : POSITION) : TARGET { return float4(1.0, 1.0, 1.0, 1.0);", nullptr},
		{"unbalanced braces (extra close)", "}}}} " + body, nullptr},
		{"unbalanced parens (missing close)",
		 "float4 main(float4 pos : POSITION) : TARGET { return float4(1.0, 1.0, 1.0, 1.0; }", nullptr},
		{"unbalanced parens (extra close)",
		 "float4 main(float4 pos : POSITION)) : TARGET { return float4(1.0, 1.0, 1.0, 1.0)); }", nullptr},
		{"unbalanced parens in parameter list", "float4 main(float4 pos : POSITION : TARGET { return 1; }",
		 nullptr},
		{"NUL in the middle", nul_text, nullptr},
		{"struct never closed", "struct S { float4 pos : POSITION;\n" + body, nullptr},
		{"sampler_state never closed", "sampler_state s { Filter = Linear;\n" + body, nullptr},
		{"main without body", "float4 main(float4 pos : POSITION) : TARGET", nullptr},
		{"stray tokens", "@@@ ### $$$ " + body, nullptr},
		// Recursive macros overflow the stack in the shared preprocessor (libobs/util/cf-lexer.c:1220/1251), which is
		// fatal, so the module must reject them before the preprocessor runs. Reaching the preprocessor would crash
		// this test process; the guard's own error text shows it stopped them first.
		{"self-referencing macro", "#define pos pos\n" + body, nullptr, "recursive macro"},
		{"self-referencing macro, tab after #", "#\tdefine pos pos\n" + body, nullptr, "recursive macro"},
		{"self-referencing macro, space after #", "# define pos pos\n" + body, nullptr, "recursive macro"},
		{"self-referencing macro, indented", "  \t#  \t define pos pos\n" + body, nullptr, "recursive macro"},
		{"self-referencing macro, comment after #", "#/* c */define pos pos\n" + body, nullptr,
		 "recursive macro"},
		{"self-referencing macro, multi-line comment after #", "#/*\n*/define pos pos\n" + body, nullptr,
		 "recursive macro"},
		{"self-referencing macro, splice after #", "#\\\ndefine pos pos\n" + body, nullptr, "recursive macro"},
		{"self-referencing macro, splice in define", "#def\\\r\nine pos pos\n" + body, nullptr,
		 "recursive macro"},
		{"self-referencing macro, splice in body", "#define pos p\\\nos\n" + body, nullptr, "recursive macro"},
		{"mutually recursive macros",
		 "#define A B\n#define B A\nfloat4 main(float4 A : POSITION) : TARGET { return A; }", nullptr,
		 "recursive macro"},
		{"mutually recursive macros, lower case",
		 "#define a b\n#define b a\nfloat4 main(float4 pos : POSITION) : TARGET { return float4(a, a, a, a); }",
		 nullptr, "recursive macro"},
		{"function-like macro recursion",
		 "#define F(x) F(x)\nfloat4 main(float4 pos : POSITION) : TARGET { return F(pos); }", nullptr,
		 "recursive macro"},
		// The lexer's string rules: a splice inside a string, the escape flag after "\\", and #include <...> as a
		// string. A guard that ends the string early sees the "/*" as a comment and misses the #define after it.
		{"string with a splice hides /*", "\"a\\\n/*\"\n#define pos pos\n" + body, nullptr, "recursive macro"},
		{"string ending in an escaped backslash hides /*", "\"\\\\\" /* \"\n#define pos pos\n" + body, nullptr,
		 "recursive macro"},
		{"#include <x/*> hides the rest", "#include <x/*>\n#define pos pos\n" + body, nullptr,
		 "recursive macro"},
		{"block comment ending in a splice at the end", body + " /* x \\\n", nullptr},
		{"10,000-link macro chain", macro_chain(10000), nullptr, "deeper than"},
		{"exponential macro expansion", macro_doubling(40), nullptr, "exceeds 1000000 tokens"},
		{"#if without #endif", "#if 1\n" + body, nullptr},
		{"#endif without #if", "#endif\n" + body, nullptr},
		{"#include of missing file", "#include \"does_not_exist.effect\"\n" + body, nullptr},
		{"unterminated string", "float4 main(float4 pos : POSITION) : TARGET { \"never closed; return pos; }",
		 nullptr},
		{"unterminated initializer", "uniform float4 x = {1.0, 1.0\n", nullptr},
		{"empty sampler state value", "sampler_state s { Filter = ; };\n" + body, nullptr},
		{"struct member without semantic name", "struct S { float4 pos : ; };\n" + body, nullptr},
		{"duplicate main", body + "\n" + body, nullptr},
		{"main param list hits EOF", "float4 main(float4 pos : ", nullptr},
		{"lone uniform keyword", "uniform", nullptr},
		{"trailing backslash", body + "\\", nullptr},
		{"long run of semicolons", std::string(1 << 20, ';') + body, nullptr},
	};
	// Lone backslash at the very end, inside a block comment, a line comment and an unterminated string, with each
	// ending the shared lexer treats differently (cf-lexer.c newline_size: "\r\n" and "\n\r" are one newline).
	// shortcut: these only document the case; the shared lexer's over-read stays inside its heap block, so it
	// cannot be made to fail deterministically here (obs.dll is not instrumented, page heap needs admin gflags).
	std::deque<std::string> names; // stable storage for the generated case names
	std::vector<malformed_case> all(std::begin(cases), std::end(cases));
	const std::pair<const char *, const char *> contexts[] = {{"block comment", " /* x "},
								  {"line comment", " // x "},
								  {"unterminated string", " \"abc"}};
	const std::pair<const char *, const char *> endings[] = {{"\\", "\\"},
								 {"\\<CR>", "\\\r"},
								 {"\\<LF>", "\\\n"},
								 {"\\<LF><CR>", "\\\n\r"}};
	for (const auto &ctx : contexts) {
		for (const auto &end : endings) {
			names.push_back(std::string("lone backslash at end: ") + ctx.first + ", " + end.first);
			all.push_back({names.back().c_str(), body + ctx.second + end.second, nullptr});
		}
	}
	for (const malformed_case &c : all) {
		if (c.known_parser_hang) {
			print_message("SKIP %s: known_parser_hang: %s\n", c.name, c.known_parser_hang);
			continue;
		}
		for (enum gs_shader_type type : {GS_SHADER_VERTEX, GS_SHADER_PIXEL}) {
			char *errors = nullptr;
			gs_shader_t *s = compile(type, c.text.c_str(), c.name, &errors);
			print_message("%-40s %s: %s\n", c.name, type == GS_SHADER_VERTEX ? "vs" : "ps",
				      s ? "compiled" : "rejected");
			if (c.expect_error) {
				assert_null(s);
				assert_non_null(errors);
				if (!strstr(errors, c.expect_error)) {
					print_message("expected '%s' in:\n%s\n", c.expect_error, errors);
				}
				assert_non_null(strstr(errors, c.expect_error));
			} else if (s) {
				assert_true(is_spirv(s));
				vkt.shader_destroy(s);
			} else {
				assert_non_null(errors);
			}
			bfree(errors);
		}
	}
}

static void test_glslang_refcount_survives_device_cycles(void **)
{
	assert_int_equal(glslang_refs(), 0);
	for (int i = 0; i < 3; ++i) {
		gs_device_t *device = nullptr;
		int ret = create(&device, 0);
		if (ret == GS_ERROR_NOT_SUPPORTED) {
			skip();
		}
		assert_int_equal(ret, GS_SUCCESS);
		assert_int_equal(glslang_refs(), 1);

		char *errors = nullptr;
		gs_shader_t *s = nullptr;
		within_5s("compile with device", [&] {
			s = vkt.pixelshader_create(device,
						   "float4 main(float4 pos : POSITION) : TARGET { return pos; }",
						   "cycle", &errors);
		});
		bfree(errors);
		assert_true(is_spirv(s));
		assert_int_equal(glslang_refs(), 1);
		vkt.shader_destroy(s);

		within_5s("device destroy", [&] { destroy(device); });
		assert_int_equal(glslang_refs(), 0);
		int n = messages();
		if (n < 0) {
			require_validation();
			print_message("validation layer missing, not checked\n");
		} else {
			assert_int_equal(n, 0);
		}
	}
	// Without a device the compile holds its own reference only while it runs.
	gs_shader_t *s = compile(GS_SHADER_PIXEL, "float4 main(float4 pos : POSITION) : TARGET { return pos; }",
				 "no device", nullptr);
	assert_true(is_spirv(s));
	assert_int_equal(glslang_refs(), 0);
	vkt.shader_destroy(s);
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(test_solid_effect_vertex_and_pixel_compile),
		cmocka_unit_test(test_invalid_hlsl_returns_error_not_crash),
		cmocka_unit_test(test_param_info_names_and_types_match_effect),
		cmocka_unit_test(test_set_float_writes_std140_offset),
		cmocka_unit_test(test_float3x3_param_warns_once_instead_of_silently_ignoring),
		cmocka_unit_test(test_core_effects_translate),
		cmocka_unit_test(test_malformed_input_never_hangs_or_crashes),
		cmocka_unit_test(test_glslang_refcount_survives_device_cycles),
	};
	return cmocka_run_group_tests(tests, setup, teardown);
}
