#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <string.h>

#include <graphics/shader-parser.h>

/* shader-parser.c directly; the effect tests drive it through compiled effects */

static const char *shader =
	"uniform float4x4 ViewProj;\n"
	"uniform texture2d image;\n"
	"uniform float values[3];\n"
	"uniform int count = 2;\n"
	"uniform bool on;\n"
	"uniform float init[3] = {1, 2, -3};\n"
	"uniform bool flag = true;\n"
	"uniform int2 ip = int2(3, 4);\n"
	"uniform float2 pts[2] = {{1, 2}, float2(3, 4)};\n"
	"uniform float2 v2 = {0.5, 0.5};\n"
	"uniform float3 v3;\n"
	"uniform float4 v4 = {1, 2, 3, 4};\n"
	"uniform int2 i2; uniform int3 i3; uniform int4 i4;\n"
	"uniform float3x3 m3;\n"
	"uniform texture3d volume; uniform texture_cube cube; uniform texture_rect rect;\n"
	"const float k = 2.0;\n"
	"sampler_state s {\n"
	"  Filter = Anisotropy; AddressU = Mirror; AddressV = Border; AddressW = MirrorOnce;\n"
	"  MaxAnisotropy = 8; BorderColor = 0xff00ff00;\n"
	"};\n"
	"sampler_state t { Filter = MIN_MAG_MIP_POINT; AddressU = Wrap; AddressV = Clamp; };\n"
	"struct V { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
	"float4 helper(inout float a, out float b, uniform float c) { b = a; return float4(a, b, c, k); }\n"
	"float4 main(V v) : TARGET { float b; return image.Sample(s, v.uv) * helper(v.uv.x, b, 1.0); }\n";

static void parse_test(void **state)
{
	UNUSED_PARAMETER(state);

	struct shader_parser sp;
	shader_parser_init(&sp);
	if (!shader_parse(&sp, shader, "test.shader")) {
		char *e = shader_parser_geterrors(&sp);
		fail_msg("parse failed:\n%s", e);
	}

	struct shader_var *var = shader_parser_getparam(&sp, "values");
	assert_non_null(var);
	assert_int_equal(var->array_count, 3);
	/* Regression: array initializers, bool defaults and int vector defaults were rejected */
	var = shader_parser_getparam(&sp, "init");
	assert_non_null(var);
	assert_int_equal(var->default_val.num, 3 * sizeof(float));
	const float *init = (const float *)var->default_val.array;
	assert_true(init[0] == 1.0f && init[1] == 2.0f && init[2] == -3.0f);
	var = shader_parser_getparam(&sp, "flag");
	assert_non_null(var);
	assert_int_equal(*(const long *)var->default_val.array, 1);
	var = shader_parser_getparam(&sp, "ip");
	assert_non_null(var);
	assert_int_equal(var->default_val.num, 2 * sizeof(long));
	assert_int_equal(((const long *)var->default_val.array)[1], 4);
	var = shader_parser_getparam(&sp, "pts");
	assert_non_null(var);
	assert_int_equal(var->default_val.num, 4 * sizeof(float));
	assert_true(((const float *)var->default_val.array)[3] == 4.0f);
	assert_non_null(shader_parser_getparam(&sp, "count"));
	assert_null(shader_parser_getparam(&sp, "missing"));
	assert_non_null(shader_parser_getstruct(&sp, "V"));
	assert_null(shader_parser_getstruct(&sp, "W"));
	assert_non_null(shader_parser_getfunc(&sp, "main"));
	assert_non_null(shader_parser_getfunc(&sp, "helper"));
	assert_null(shader_parser_getfunc(&sp, "nope"));

	struct shader_sampler *s = shader_parser_getsampler(&sp, "s");
	assert_non_null(s);
	assert_null(shader_parser_getsampler(&sp, "u"));
	struct gs_sampler_info info;
	shader_sampler_convert(s, &info);
	assert_int_equal(info.filter, GS_FILTER_ANISOTROPIC);
	assert_int_equal(info.address_u, GS_ADDRESS_MIRROR);
	assert_int_equal(info.address_v, GS_ADDRESS_BORDER);
	assert_int_equal(info.address_w, GS_ADDRESS_MIRRORONCE);
	assert_int_equal(info.max_anisotropy, 8);
	/* Regression: BorderColor was parsed with strtol() after skipping one character, so "0xff00ff00" became 0, and
	 * colors above 0x7fffffff clamped on Windows */
	assert_int_equal(info.border_color, 0xff00ff00);
	shader_sampler_convert(shader_parser_getsampler(&sp, "t"), &info);
	assert_int_equal(info.filter, GS_FILTER_POINT);

	char *errors = shader_parser_geterrors(&sp);
	bfree(errors);
	shader_parser_free(&sp);
}

static void types_test(void **state)
{
	UNUSED_PARAMETER(state);

	assert_int_equal(get_shader_param_type("float"), GS_SHADER_PARAM_FLOAT);
	assert_int_equal(get_shader_param_type("float2"), GS_SHADER_PARAM_VEC2);
	assert_int_equal(get_shader_param_type("float3"), GS_SHADER_PARAM_VEC3);
	assert_int_equal(get_shader_param_type("float4"), GS_SHADER_PARAM_VEC4);
	assert_int_equal(get_shader_param_type("int2"), GS_SHADER_PARAM_INT2);
	assert_int_equal(get_shader_param_type("int3"), GS_SHADER_PARAM_INT3);
	assert_int_equal(get_shader_param_type("int4"), GS_SHADER_PARAM_INT4);
	assert_int_equal(get_shader_param_type("int"), GS_SHADER_PARAM_INT);
	assert_int_equal(get_shader_param_type("bool"), GS_SHADER_PARAM_BOOL);
	assert_int_equal(get_shader_param_type("float4x4"), GS_SHADER_PARAM_MATRIX4X4);
	assert_int_equal(get_shader_param_type("texture2d"), GS_SHADER_PARAM_TEXTURE);
	assert_int_equal(get_shader_param_type("unknown"), GS_SHADER_PARAM_UNKNOWN);

	const char *filters[] = {"Anisotropy",
				 "Point",
				 "Linear",
				 "MIN_MAG_MIP_POINT",
				 "MIN_MAG_POINT_MIP_LINEAR",
				 "MIN_POINT_MAG_LINEAR_MIP_POINT",
				 "MIN_POINT_MAG_MIP_LINEAR",
				 "MIN_LINEAR_MAG_MIP_POINT",
				 "MIN_LINEAR_MAG_POINT_MIP_LINEAR",
				 "MIN_MAG_LINEAR_MIP_POINT",
				 "MIN_MAG_MIP_LINEAR",
				 "unknown"};
	for (size_t i = 0; i < sizeof(filters) / sizeof(filters[0]); i++)
		get_sample_filter(filters[i]);
	assert_int_equal(get_sample_filter("Anisotropy"), GS_FILTER_ANISOTROPIC);

	const char *modes[] = {"Wrap", "Clamp", "Mirror", "Border", "MirrorOnce", "unknown"};
	for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
		get_address_mode(modes[i]);
	assert_int_equal(get_address_mode("Clamp"), GS_ADDRESS_CLAMP);
}

static void error_test(void **state)
{
	UNUSED_PARAMETER(state);

	const char *bad[] = {
		"uniform float x",
		"sampler_state s { Filter = ; };",
		"sampler_state { };",
		"struct { float x; };",
		"struct S { float x : ; };",
		"float4 f(float x { }",
		"float4 f(float x) { return x;",
		"uniform float a[;",
		"uniform float a[2] = {1, 2;",
		"uniform float a = ;",
		"const x;",
		"$ % ^",
		"/* unterminated",
	};
	for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		struct shader_parser sp;
		shader_parser_init(&sp);
		shader_parse(&sp, bad[i], "bad.shader");
		char *errors = shader_parser_geterrors(&sp);
		bfree(errors);
		shader_parser_free(&sp);
	}

	struct shader_parser sp;
	shader_parser_init(&sp);
	assert_false(shader_parse(&sp, "", "empty.shader"));
	shader_parser_free(&sp);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(parse_test),
		obs_unit_test(types_test),
		obs_unit_test(error_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
