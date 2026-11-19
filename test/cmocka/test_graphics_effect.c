#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"
#include "gs-test.h"

#include <string.h>

#include <graphics/matrix4.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>
#include <util/dstr.h>

/* effect.c and effect-parser.c (and through them shader-parser.c and the cf lexer/parser) on libobs-null */

static void data_effects_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* every effect libobs ships parses and compiles */
	os_glob_t *glob;
	assert_int_equal(os_glob(OBS_DATA_DIR "/*.effect", 0, &glob), 0);
	assert_true(glob->gl_pathc >= 20);
	for (size_t i = 0; i < glob->gl_pathc; i++) {
		char *errors = NULL;
		gs_effect_t *effect = gs_effect_create_from_file(glob->gl_pathv[i].path, &errors);
		if (!effect)
			fail_msg("%s failed:\n%s", glob->gl_pathv[i].path, errors ? errors : "(no errors)");
		bfree(errors);
		gs_effect_destroy(effect);
	}
	os_globfree(glob);

	assert_null(gs_effect_create_from_file(OBS_DATA_DIR "/does-not-exist.effect", NULL));
	assert_null(gs_effect_create_from_file(NULL, NULL));
	/* the same file twice returns the cached effect */
	gs_effect_t *a = gs_effect_create_from_file(OBS_DATA_DIR "/solid.effect", NULL);
	gs_effect_t *b = gs_effect_create_from_file(OBS_DATA_DIR "/solid.effect", NULL);
	assert_ptr_equal(a, b);
	gs_effect_destroy(a);
}

static void technique_test(void **state)
{
	UNUSED_PARAMETER(state);

	gs_effect_t *effect = gs_effect_create_from_file(OBS_DATA_DIR "/default.effect", NULL);
	assert_non_null(effect);

	gs_technique_t *tech = gs_effect_get_technique(effect, "Draw");
	assert_non_null(tech);
	assert_null(gs_effect_get_technique(effect, "NoSuchTechnique"));
	assert_null(gs_effect_get_current_technique(effect));

	assert_int_equal(gs_technique_begin(tech), 1);
	assert_ptr_equal(gs_effect_get_current_technique(effect), tech);
	assert_ptr_equal(gs_get_effect(), effect);
	assert_non_null(gs_technique_get_pass_by_idx(tech, 0));
	assert_null(gs_technique_get_pass_by_idx(tech, 5));
	/* Regression: the index one past the last pass returned a pointer past the end */
	assert_null(gs_technique_get_pass_by_idx(tech, 1));
	/* Regression: looking passes up by name crashed on unnamed passes, like default.effect's */
	assert_null(gs_technique_get_pass_by_name(tech, "NoSuchPass"));
	assert_true(gs_technique_begin_pass(tech, 0));
	assert_non_null(gs_get_vertex_shader());
	assert_non_null(gs_get_pixel_shader());
	gs_technique_end_pass(tech);
	assert_false(gs_technique_begin_pass(tech, 5));
	assert_false(gs_technique_begin_pass_by_name(tech, "NoSuchPass"));
	gs_technique_end(tech);
	assert_null(gs_get_effect());

	/* gs_effect_loop runs every pass once */
	int passes = 0;
	while (gs_effect_loop(effect, "Draw")) {
		gs_draw_sprite(NULL, 0, 4, 4);
		passes++;
	}
	assert_int_equal(passes, 1);
	assert_false(gs_effect_loop(effect, "NoSuchTechnique"));
	assert_false(gs_effect_loop(NULL, "Draw"));

	/* a second effect cannot loop while one is active */
	gs_effect_t *solid = gs_effect_create_from_file(OBS_DATA_DIR "/solid.effect", NULL);
	assert_true(gs_effect_loop(effect, "Draw"));
	assert_false(gs_effect_loop(solid, "Solid"));
	while (gs_effect_loop(effect, "Draw"))
		;

	gs_technique_begin(NULL);
	gs_technique_end(NULL);
	assert_false(gs_technique_begin_pass(NULL, 0));
	gs_technique_end_pass(NULL);
	assert_null(gs_effect_get_technique(NULL, "Draw"));

	gs_effect_destroy(solid);
	gs_effect_destroy(effect);
	gs_effect_destroy(NULL);
}

static const char *param_effect =
	"uniform float4x4 ViewProj;\n"
	"uniform float4x4 World;\n"
	"uniform texture2d image;\n"
	"uniform float  f = 1.5;\n"
	"uniform int    i = 3;\n"
	"uniform bool   b = true;\n"
	"uniform float2 v2 = {1.0, 2.0};\n"
	"uniform float3 v3;\n"
	"uniform float4 color <string name = \"Color\"; float minimum = 0.0;> = {1, 1, 1, 1};\n"
	"uniform float  arr[4];\n"
	"sampler_state def { Filter = Linear; AddressU = Clamp; AddressV = Wrap; "
	"MaxAnisotropy = 4; BorderColor = 0xff000000; };\n"
	"struct VertData { float4 pos : POSITION; float2 uv : TEXCOORD0; };\n"
	"float helper(float x) { return x * f; }\n"
	"VertData VSMain(VertData v) {\n"
	"  VertData o;\n"
	"  o.pos = mul(float4(v.pos.xyz, 1.0), ViewProj);\n"
	"  o.uv = v.uv;\n"
	"  return o;\n"
	"}\n"
	"float4 PSMain(VertData v) : TARGET {\n"
	"  return image.Sample(def, v.uv) * color * helper(arr[i]) * (b ? 1.0 : 0.5) + "
	"float4(v3, v2.x);\n"
	"}\n"
	"technique Draw { pass p0 { vertex_shader = VSMain(v); pixel_shader = PSMain(v); } "
	"pass p1 { vertex_shader = VSMain(v); pixel_shader = PSMain(v); } }\n";

static void params_test(void **state)
{
	UNUSED_PARAMETER(state);

	char *errors = NULL;
	gs_effect_t *effect = gs_effect_create(param_effect, "params.effect", &errors);
	if (!effect)
		fail_msg("compile failed:\n%s", errors ? errors : "(none)");
	bfree(errors);

	assert_true(gs_effect_get_num_params(effect) >= 10);
	assert_non_null(gs_effect_get_viewproj_matrix(effect));
	assert_non_null(gs_effect_get_world_matrix(effect));
	assert_null(gs_effect_get_param_by_name(effect, "missing"));
	assert_null(gs_effect_get_param_by_idx(effect, 1000));
	assert_non_null(gs_effect_get_param_by_idx(effect, 0));

	gs_eparam_t *f = gs_effect_get_param_by_name(effect, "f");
	struct gs_effect_param_info info;
	gs_effect_get_param_info(f, &info);
	assert_string_equal(info.name, "f");
	assert_int_equal(info.type, GS_SHADER_PARAM_FLOAT);

	/* defaults and current values */
	assert_int_equal(gs_effect_get_default_val_size(f), sizeof(float));
	float *def = gs_effect_get_default_val(f);
	assert_true(*def == 1.5f);
	bfree(def);
	gs_effect_set_float(f, 2.5f);
	assert_int_equal(gs_effect_get_val_size(f), sizeof(float));
	float *cur = gs_effect_get_val(f);
	assert_true(*cur == 2.5f);
	bfree(cur);
	gs_effect_set_default(f);
	cur = gs_effect_get_val(f);
	assert_true(*cur == 1.5f);
	bfree(cur);

	gs_effect_set_int(gs_effect_get_param_by_name(effect, "i"), 2);
	gs_effect_set_bool(gs_effect_get_param_by_name(effect, "b"), false);
	struct vec2 v2;
	vec2_set(&v2, 3.0f, 4.0f);
	gs_effect_set_vec2(gs_effect_get_param_by_name(effect, "v2"), &v2);
	struct vec3 v3;
	vec3_set(&v3, 1.0f, 2.0f, 3.0f);
	gs_effect_set_vec3(gs_effect_get_param_by_name(effect, "v3"), &v3);
	gs_eparam_t *color = gs_effect_get_param_by_name(effect, "color");
	struct vec4 v4;
	vec4_set(&v4, 0.5f, 0.5f, 0.5f, 1.0f);
	gs_effect_set_vec4(color, &v4);
	gs_effect_set_color(color, 0xff00ff00);
	struct matrix4 m;
	matrix4_identity(&m);
	gs_effect_set_matrix4(gs_effect_get_viewproj_matrix(effect), &m);
	float arr[4] = {1, 2, 3, 4};
	gs_effect_set_val(gs_effect_get_param_by_name(effect, "arr"), arr, sizeof(arr));

	/* annotations */
	assert_int_equal(gs_param_get_num_annotations(color), 2);
	gs_eparam_t *anno = gs_param_get_annotation_by_name(color, "name");
	assert_non_null(anno);
	assert_non_null(gs_param_get_annotation_by_idx(color, 1));
	assert_null(gs_param_get_annotation_by_idx(color, 5));
	assert_null(gs_param_get_annotation_by_name(color, "nope"));
	assert_int_equal(gs_param_get_num_annotations(f), 0);

	/* textures and samplers reach the shader on update */
	gs_texture_t *tex = gs_texture_create(2, 2, GS_RGBA, 1, NULL, 0);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, tex);
	gs_effect_set_texture_srgb(image, tex);
	struct gs_sampler_info si = {.filter = GS_FILTER_POINT};
	gs_samplerstate_t *ss = gs_samplerstate_create(&si);
	gs_effect_set_next_sampler(image, ss);

	int passes = 0;
	while (gs_effect_loop(effect, "Draw")) {
		gs_effect_update_params(effect);
		passes++;
	}
	assert_int_equal(passes, 2);
	assert_true(gs_test_calls("gs_shader_set_val") > 0);

	/* NULL params are refused */
	gs_effect_set_float(NULL, 1.0f);
	gs_effect_set_val(NULL, arr, 4);
	assert_null(gs_effect_get_val(NULL));
	assert_null(gs_effect_get_default_val(NULL));
	assert_int_equal(gs_effect_get_val_size(NULL), 0);
	assert_int_equal(gs_effect_get_default_val_size(NULL), 0);
	gs_effect_set_default(NULL); /* regression: dereferenced the NULL param */
	gs_effect_set_next_sampler(NULL, ss);
	gs_effect_update_params(NULL);
	assert_int_equal(gs_effect_get_num_params(NULL), 0);
	assert_null(gs_effect_get_param_by_name(NULL, "f"));
	assert_null(gs_effect_get_viewproj_matrix(NULL));
	assert_int_equal(gs_param_get_num_annotations(NULL), 0);

	gs_samplerstate_destroy(ss);
	gs_texture_destroy(tex);
	gs_effect_destroy(effect);
}

/* Malformed effects must fail cleanly with an error string. The two macro inputs used to crash the cf
 * preprocessor before 9cb523235. */
static const char *bad_effects[] = {
	"uniform float x",                                         /* missing ';' */
	"technique Draw { pass { vertex_shader = Missing(v); } }", /* missing function */
	"technique { }",                                           /* unnamed technique */
	"technique Draw { pass p0 { bogus = 1; } }",               /* unknown pass entry */
	"sampler_state s { Filter = Linear }",                     /* missing ';' in a sampler */
	"struct S { float4 pos : POSITION }",                      /* missing ';' in a struct */
	"float4 f(float x { return x; }",                          /* broken parameter list */
	"uniform float4 c = {1, 2;",                               /* broken initializer */
	"uniform float a <float b = 1.0> = 1.0;",                  /* broken annotation */
	"technique Draw { pass p0 { vertex_shader = VS(v); pixel_shader = PS(v); } }\n"
	"float4 VS(float4 v : POSITION) : POSITION { return v; }\n"
	"float4 PS(float4 v : POSITION) : TARGET { return undefined_thing; }", /* order and undefined names */
	"#include \"does-not-exist.effect\"\nuniform float x;",
	"#if 1 ~ 2\n#endif\n",
	"/* unterminated comment",
	"\"unterminated string",
	"uniform float4x4 ViewProj; uniform texture2d image; float4 PS(float2 uv : TEXCOORD0) : TARGET { return "
	"image.Sample(missing_sampler, uv); }\ntechnique Draw { pass { pixel_shader = PS(uv); } }",
	"",
	"#define M(a, b) a b\nM 2\n",
	"#define M(a) a\nuniform float x = M(1, 2",
	"uniform unknowntype x;", /* unknown type */
	"sampler_state s { Filter = ; };", /* empty sampler state value */
};

static int plain_setup(void **state);
static int plain_teardown(void **state);

static void malformed_test(void **state)
{
	/* Regression: a missing function and a sampler state missing ';' leaked on the error path. Effects that
	 * parse are kept in the graphics context's effect cache until gs_destroy(), so each input gets its own
	 * context and the count is taken around it. */
	size_t leaking = 0;
	for (size_t i = 0; i < sizeof(bad_effects) / sizeof(bad_effects[0]); i++) {
		long before = bnum_allocs();
		assert_int_equal(plain_setup(state), 0);
		char *errors = NULL;
		gs_effect_t *effect = gs_effect_create(bad_effects[i], "bad.effect", &errors);
		/* some inputs only produce warnings; either way nothing must leak or crash */
		gs_effect_destroy(effect);
		bfree(errors);
		plain_teardown(state);
		if (bnum_allocs() != before) {
			print_message("bad effect %zu leaked %ld allocation(s)\n", i, bnum_allocs() - before);
			leaking++;
		}
	}
	assert_int_equal(leaking, 0);

	assert_int_equal(plain_setup(state), 0);
	assert_null(gs_effect_create(NULL, "null.effect", NULL));
	plain_teardown(state);
}

static int plain_setup(void **state)
{
	UNUSED_PARAMETER(state);
	if (gs_create(&gs_test_graphics, NULL_GRAPHICS_MODULE, 0) != GS_SUCCESS)
		return -1;
	gs_enter_context(gs_test_graphics);
	return 0;
}

static int plain_teardown(void **state)
{
	UNUSED_PARAMETER(state);
	gs_leave_context();
	gs_destroy(gs_test_graphics);
	return 0;
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		gs_unit_test(data_effects_test),
		gs_unit_test(technique_test),
		gs_unit_test(params_test),
		cmocka_unit_test(malformed_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
