// HLSL as emitted by the effect parser -> Vulkan GLSL 450 -> SPIR-V (glslang, linked statically).
// The translation is adapted from libobs-opengl/gl-shaderparser.c; differences are marked.
//
// Layout: each stage has its own descriptor set, 0 for the vertex shader and 1 for the pixel shader. Non-texture
// uniforms live in one std140 block at binding 0 of it (offsets from glslang reflection); each texture is a combined
// image sampler (sampler2D etc., an array of them for a texture array) at binding 1..n in declaration order.
// Vertex inputs take locations in declaration order; varyings take a location from their semantic so the vertex
// and pixel shaders of a pass match even when their structs differ; pixel outputs start at location 0.
#include "vulkan-subsystem.hpp"

#include <graphics/shader-parser.h>
#include <util/base.h>
#include <util/bmem.h>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <new>

#ifdef _WIN32
#include <windows.h>
#endif

static std::mutex glslang_mutex;
static int glslang_refs;

void vk_glslang_ref()
{
	std::lock_guard<std::mutex> lock(glslang_mutex);
	if (glslang_refs++ == 0) {
		glslang::InitializeProcess();
	}
}

void vk_glslang_unref()
{
	std::lock_guard<std::mutex> lock(glslang_mutex);
	if (--glslang_refs == 0) {
		glslang::FinalizeProcess();
	}
}

extern "C" EXPORT int obs_vulkan_glslang_refs(void)
{
	std::lock_guard<std::mutex> lock(glslang_mutex);
	return glslang_refs;
}

namespace {

constexpr int max_nesting = 256; // our translator recurses per bracket; deeper input is rejected up front

const char *glsl_type(const char *type, size_t len)
{
	static const struct {
		const char *hlsl, *glsl;
	} map[] = {
		{"float2", "vec2"},
		{"float3", "vec3"},
		{"float4", "vec4"},
		{"int2", "ivec2"},
		{"int3", "ivec3"},
		{"int4", "ivec4"},
		{"uint2", "uvec2"},
		{"uint3", "uvec3"},
		{"uint4", "uvec4"},
		{"float3x3", "mat3x3"},
		{"float3x4", "mat3x4"},
		{"float4x4", "mat4x4"},
		{"texture2d", "sampler2D"},
		{"texture3d", "sampler3D"},
		{"texture_cube", "samplerCube"},
		// Differs from GL: Vulkan has no sampler2DRect; D3D11 also treats texture_rect as a 2D texture.
		{"texture_rect", "sampler2D"},
	};
	for (const auto &m : map) {
		if (strlen(m.hlsl) == len && strncmp(m.hlsl, type, len) == 0) {
			return m.glsl;
		}
	}
	return nullptr;
}

bool is_texture(const char *type)
{
	return astrcmp_n(type, "texture", 7) == 0;
}

bool is_integer(const char *type)
{
	return strncmp(type, "int", 3) == 0 || strncmp(type, "uint", 4) == 0;
}

// Varying location by semantic, so stages agree by semantic like D3D. -1: unsupported semantic.
int varying_location(const char *mapping)
{
	if (astrcmp_n(mapping, "TEXCOORD", 8) == 0) {
		int n = mapping[8] ? atoi(mapping + 8) : 0;
		return n >= 0 && n < 16 ? n : -1;
	}
	if (strcmp(mapping, "COLOR") == 0 || strcmp(mapping, "COLOR0") == 0) {
		return 16;
	}
	if (strcmp(mapping, "COLOR1") == 0) {
		return 17;
	}
	if (strcmp(mapping, "NORMAL") == 0) {
		return 18;
	}
	if (strcmp(mapping, "TANGENT") == 0) {
		return 19;
	}
	return -1;
}

struct translator {
	enum gs_shader_type type;
	shader_parser parser;
	std::string out;
	std::string error;
	int sincos_counter = 1;
	uint32_t next_input = 0, next_output = 0;
	int max_varying = -1;
	std::vector<std::string> inputs;

	explicit translator(enum gs_shader_type t) : type(t) { shader_parser_init(&parser); }
	~translator() { shader_parser_free(&parser); }

	void cat(const char *s) { out += s; }
	void cat(const strref &s) { out.append(s.array, s.len); }

	void write_type(const char *type)
	{
		const char *t = glsl_type(type, strlen(type));
		out += t ? t : type;
	}

	bool write_type_token(cf_token *token)
	{
		const char *t = glsl_type(token->str.array, token->str.len);
		if (t) {
			out += t;
		}
		return t != nullptr;
	}

	shader_var *getparam(cf_token *token)
	{
		for (size_t i = 0; i < parser.params.num; i++) {
			shader_var *param = parser.params.array + i;
			if (strref_cmp(&token->str, param->name) == 0) {
				return param;
			}
		}
		return nullptr;
	}

	size_t getsampler(cf_token *token)
	{
		for (size_t i = 0; i < parser.samplers.num; i++) {
			if (strref_cmp(&token->str, parser.samplers.array[i].name) == 0) {
				return i;
			}
		}
		return (size_t)-1;
	}

	// Differs from GL: `uniform` is dropped (HLSL uniform function parameters) and array sizes are kept.
	void write_var(shader_var *var)
	{
		if (var->var_type == SHADER_VAR_CONST) {
			cat("const ");
		} else if (var->var_type == SHADER_VAR_INOUT) {
			cat("inout ");
		} else if (var->var_type == SHADER_VAR_OUT) {
			cat("out ");
		}
		write_type(var->type);
		cat(" ");
		cat(var->name);
		if (var->array_count) {
			out += "[" + std::to_string(var->array_count) + "]";
		}
	}

	void write_params()
	{
		std::string block;
		const std::string set = type == GS_SHADER_PIXEL ? "1" : "0";
		uint32_t binding = 1;
		for (size_t i = 0; i < parser.params.num; i++) {
			shader_var *var = parser.params.array + i;
			if (is_texture(var->type)) {
				out += "layout(set = " + set + ", binding = " + std::to_string(binding++) +
				       ") uniform ";
				write_var(var);
				cat(";\n");
			} else if (var->var_type == SHADER_VAR_UNIFORM) {
				std::swap(out, block);
				cat("\t");
				write_var(var);
				cat(";\n");
				std::swap(out, block);
			} else {
				write_var(var);
				cat(";\n");
			}
		}
		if (!block.empty()) {
			out += "layout(std140, set = " + set + ", binding = 0) uniform obs_uniforms {\n" + block +
			       "};\n";
		}
		cat("\n");
	}

	// Flattens struct inputs/outputs into individual in/out variables named <name>_<member>.
	bool write_storage_var(shader_var *var, bool input, const std::string &name)
	{
		if (shader_struct *st = shader_parser_getstruct(&parser, var->type)) {
			for (size_t i = 0; i < st->vars.num; i++) {
				shader_var *m = st->vars.array + i;
				if (!write_storage_var(m, input, name + "_" + m->name)) {
					return false;
				}
			}
			return true;
		}

		const char *mapping = var->mapping ? var->mapping : "";
		bool vs = type == GS_SHADER_VERTEX;
		if (input && strcmp(mapping, "VERTEXID") == 0) {
			return true;
		}
		if (strcmp(mapping, "POSITION") == 0 && (input != vs)) {
			return true; // VS output -> gl_Position, PS input -> gl_FragCoord
		}

		int location;
		if (input && vs) {
			location = (int)next_input++;
			inputs.push_back(mapping);
		} else if (!input && !vs) {
			location = (int)next_output++;
		} else {
			location = varying_location(mapping);
			if (location < 0) {
				error = std::string("unsupported varying semantic '") + mapping + "' on '" + var->name +
					"'";
				return false;
			}
			max_varying = (std::max)(max_varying, location);
		}

		out += "layout(location = " + std::to_string(location) + ") ";
		if (is_integer(var->type) && (vs != input)) {
			cat("flat ");
		}
		cat(input ? "in " : "out ");
		write_type(var->type);
		out += " " + name + ";\n";
		return true;
	}

	/* ---- intrinsics, as gl-shaderparser.c ---- */

	bool write_mad(cf_token **p_token)
	{
		cf_parser *cfp = &parser.cfp;
		cfp->cur_token = *p_token;
		if (!cf_next_token(cfp) || !cf_token_is(cfp, "(")) {
			return false;
		}
		cat("(");
		write_contents(&cfp->cur_token, ",");
		cat(") * (");
		cf_next_token(cfp);
		write_contents(&cfp->cur_token, ",");
		cat(") + (");
		cf_next_token(cfp);
		write_contents(&cfp->cur_token, ")");
		cat("))");
		*p_token = cfp->cur_token;
		return true;
	}

	bool write_mul(cf_token **p_token)
	{
		cf_parser *cfp = &parser.cfp;
		cfp->cur_token = *p_token;
		if (!cf_next_token(cfp) || !cf_token_is(cfp, "(")) {
			return false;
		}
		cat("(");
		write_contents(&cfp->cur_token, ",");
		cat(") * (");
		cf_next_token(cfp);
		write_contents(&cfp->cur_token, ")");
		cat("))");
		*p_token = cfp->cur_token;
		return true;
	}

	bool write_sincos(cf_token **p_token)
	{
		cf_parser *cfp = &parser.cfp;
		cfp->cur_token = *p_token;
		if (!cf_next_token(cfp) || !cf_token_is(cfp, "(")) {
			return false;
		}

		std::string var = "sincos_var_internal_" + std::to_string(sincos_counter++);
		out += "float " + var + " = ";
		write_contents(&cfp->cur_token, ",");
		cat("); ");

		bool ok = false;
		if (cf_next_token(cfp)) {
			write_contents(&cfp->cur_token, ",");
			out += " = sin(" + var + "); ";
			if (cf_next_token(cfp)) {
				write_contents(&cfp->cur_token, ")");
				out += " = cos(" + var + ")";
				ok = true;
			}
		}
		*p_token = cfp->cur_token;
		return ok;
	}

	bool write_saturate(cf_token **p_token)
	{
		cf_parser *cfp = &parser.cfp;
		cfp->cur_token = *p_token;
		if (!cf_next_token(cfp) || !cf_token_is(cfp, "(")) {
			return false;
		}
		cat("clamp");
		write_contents(&cfp->cur_token, ")");
		cat(", 0.0, 1.0)");
		*p_token = cfp->cur_token;
		return true;
	}

	bool write_texture_call(shader_var *var, const char *call, bool sampler)
	{
		cf_parser *cfp = &parser.cfp;
		if (!cf_next_token(cfp) || !cf_token_is(cfp, "(")) {
			return false;
		}
		if (sampler) {
			if (!cf_next_token(cfp)) {
				return false;
			}
			size_t id = getsampler(cfp->cur_token);
			if (id == (size_t)-1 || !cf_next_token(cfp) || !cf_token_is(cfp, ",")) {
				return false;
			}
			var->gl_sampler_id = id;
		}
		out += std::string(call) + "(" + var->name + ", ";
		return true;
	}

	// texture.Sample(sampler, uv) and friends.
	bool write_texture_code(cf_token **p_token, shader_var *var)
	{
		cf_parser *cfp = &parser.cfp;
		bool written = false;
		cfp->cur_token = *p_token;
		if (!cf_next_token(cfp) || !cf_token_is(cfp, ".") || !cf_next_token(cfp)) {
			return false;
		}

		if (cf_token_is(cfp, "Sample") || cf_token_is(cfp, "SampleBias")) {
			written = write_texture_call(var, "texture", true);
		} else if (cf_token_is(cfp, "SampleGrad")) {
			written = write_texture_call(var, "textureGrad", true);
		} else if (cf_token_is(cfp, "SampleLevel")) {
			written = write_texture_call(var, "textureLod", true);
		} else if (cf_token_is(cfp, "Load")) {
			written = write_texture_call(
				var, strcmp(var->type, "texture3d") == 0 ? "obs_load_3d" : "obs_load_2d", false);
		}

		if (!written || !cf_next_token(cfp)) {
			return false;
		}
		write_contents(&cfp->cur_token, ")");
		cat(")");
		*p_token = cfp->cur_token;
		return true;
	}

	bool write_intrinsic(cf_token **p_token)
	{
		cf_token *token = *p_token;
		bool written = true;
		static const struct {
			const char *hlsl, *glsl;
		} renames[] = {{"atan2", "atan"}, {"ddx", "dFdx"}, {"ddy", "dFdy"},         {"fmod", "mod"},
			       {"frac", "fract"}, {"lerp", "mix"}, {"rsqrt", "inversesqrt"}};
		for (const auto &r : renames) {
			if (strref_cmp(&token->str, r.hlsl) == 0) {
				cat(r.glsl);
				return true;
			}
		}

		if (strref_cmp(&token->str, "mad") == 0) {
			written = write_mad(&token);
		} else if (strref_cmp(&token->str, "mul") == 0) {
			written = write_mul(&token);
		} else if (strref_cmp(&token->str, "saturate") == 0) {
			written = write_saturate(&token);
		} else if (strref_cmp(&token->str, "sincos") == 0) {
			written = write_sincos(&token);
		} else {
			shader_var *var = getparam(token);
			written = var && is_texture(var->type) && write_texture_code(&token, var);
		}

		if (written) {
			*p_token = token;
		}
		return written;
	}

	void write_contents(cf_token **p_token, const char *end)
	{
		cf_token *token = *p_token;

		if (token->type != CFTOKEN_NAME || (!write_type_token(token) && !write_intrinsic(&token))) {
			cat(token->str);
		}

		while (token->type != CFTOKEN_NONE) {
			token++;
			if (end && strref_cmp(&token->str, end) == 0) {
				break;
			}

			if (token->type == CFTOKEN_NAME) {
				if (!write_type_token(token) && !write_intrinsic(&token)) {
					cat(token->str);
				}
			} else if (token->type == CFTOKEN_OTHER) {
				if (*token->str.array == '{') {
					write_contents(&token, "}");
				} else if (*token->str.array == '(') {
					write_contents(&token, ")");
				}
				cat(token->str);
			} else {
				cat(token->str);
			}
		}

		*p_token = token;
	}

	void write_function(shader_func *func)
	{
		write_type(func->return_type);
		cat(" ");
		cat(strcmp(func->name, "main") == 0 ? "_main_wrap" : func->name);
		cat("(");
		for (size_t i = 0; i < func->params.num; i++) {
			if (i > 0) {
				cat(", ");
			}
			write_var(func->params.array + i);
		}
		cat(")\n");

		// Differs from GL: keep glslang's line numbers equal to the HLSL source lines.
		cf_token *token = func->start;
		uint32_t row = 0, col = 0;
		lexer_getstroffset(&token->lex->base_lexer, token->unmerged_str.array, &row, &col);
		out += "#line " + std::to_string(row) + "\n";

		write_contents(&token, "}");
		cat("}\n\n");
	}

	// Differs from GL: walks every main parameter (GL only the first), and VS POSITION goes to gl_Position.
	void assign_inputs(shader_var *var, const std::string &local, const std::string &global)
	{
		if (shader_struct *st = shader_parser_getstruct(&parser, var->type)) {
			for (size_t i = 0; i < st->vars.num; i++) {
				shader_var *m = st->vars.array + i;
				assign_inputs(m, local + "." + m->name, global + "_" + m->name);
			}
			return;
		}
		const char *mapping = var->mapping ? var->mapping : "";
		out += "\t" + local + " = ";
		if (strcmp(mapping, "VERTEXID") == 0) {
			out += "uint(gl_VertexIndex)";
		} else if (type == GS_SHADER_PIXEL && strcmp(mapping, "POSITION") == 0) {
			// D3D's SV_Position.w is the clip-space w; gl_FragCoord.w is 1/w.
			out += "vec4(gl_FragCoord.xyz, 1.0 / gl_FragCoord.w)";
		} else {
			out += global;
		}
		cat(";\n");
	}

	void assign_outputs(shader_var *var, const std::string &local, const std::string &global)
	{
		if (shader_struct *st = shader_parser_getstruct(&parser, var->type)) {
			for (size_t i = 0; i < st->vars.num; i++) {
				shader_var *m = st->vars.array + i;
				assign_outputs(m, local + "." + m->name, global + "_" + m->name);
			}
			return;
		}
		const char *mapping = var->mapping ? var->mapping : "";
		if (type == GS_SHADER_VERTEX && strcmp(mapping, "POSITION") == 0) {
			out += "\tgl_Position = " + local + ";\n";
		} else {
			out += "\t" + global + " = " + local + ";\n";
		}
	}

	bool build()
	{
		shader_func *main_func = shader_parser_getfunc(&parser, "main");
		if (!main_func) {
			error = "function 'main' not found";
			return false;
		}

		// obs_glsl_compile = false: effects take their D3D branches. Vulkan's framebuffer and texture origin is
		// top-left like D3D; its clip-space Y points down, and that flip is done ONLY by a negative-height viewport
		// in the draw path (Task 7), never in shaders, with D3D's [0, 1] depth range, so nothing flips twice.
		out = "#version 450\n\n"
		      "const bool obs_glsl_compile = false;\n\n"
		      "vec4 obs_load_2d(sampler2D s, ivec3 p_lod)\n{\n\treturn texelFetch(s, p_lod.xy, p_lod.z);\n}\n\n"
		      "vec4 obs_load_3d(sampler3D s, ivec4 p_lod)\n{\n\treturn texelFetch(s, p_lod.xyz, p_lod.w);\n}\n\n";
		write_params();

		for (size_t i = 0; i < main_func->params.num; i++) {
			shader_var *var = main_func->params.array + i;
			if (!write_storage_var(var, true, std::string("obs_in_") + var->name)) {
				return false;
			}
		}
		shader_var ret = {};
		ret.type = main_func->return_type;
		ret.name = (char *)"outputval";
		ret.mapping = main_func->mapping;
		if (!write_storage_var(&ret, false, "obs_out")) {
			return false;
		}
		cat("\n");

		for (size_t i = 0; i < parser.structs.num; i++) {
			shader_struct *st = parser.structs.array + i;
			out += std::string("struct ") + st->name + " {\n";
			for (size_t j = 0; j < st->vars.num; j++) {
				cat("\t");
				write_var(st->vars.array + j);
				cat(";\n");
			}
			cat("};\n\n");
		}

		for (size_t i = 0; i < parser.funcs.num; i++) {
			write_function(parser.funcs.array + i);
		}

		cat("void main(void)\n{\n");
		std::string call = "_main_wrap(";
		for (size_t i = 0; i < main_func->params.num; i++) {
			shader_var *var = main_func->params.array + i;
			cat("\t");
			write_type(var->type);
			out += std::string(" ") + var->name + ";\n";
			assign_inputs(var, var->name, std::string("obs_in_") + var->name);
			call += (i ? ", " : "") + std::string(var->name);
		}
		cat("\t");
		write_type(main_func->return_type);
		out += " outputval = " + call + ");\n";
		assign_outputs(&ret, "outputval", "obs_out");
		cat("}\n");
		return true;
	}
};

bool nesting_ok(const char *text, std::string &error)
{
	int depth = 0;
	for (const char *p = text; *p; ++p) {
		if (*p == '(' || *p == '[' || *p == '{') {
			++depth;
		} else if (*p == ')' || *p == ']' || *p == '}') {
			--depth;
		}
		if (depth > max_nesting) {
			error = "shader nests brackets deeper than " + std::to_string(max_nesting);
			return false;
		}
	}
	return true;
}

// The shared preprocessor expands macros recursively with no guard (cf-lexer.c, cf_preprocess_addtoken), so a
// recursive, very deep or exponentially growing #define overflows the stack or exhausts memory, which is fatal.
// All three are rejected before it runs. The tokens come from the same cf_lexer_lex the preprocessor uses, so
// splices, comments and string rules match by construction; any '#' token (not only at line start, so at least as
// permissive) followed by space tokens and the name `define` starts a definition.
// shortcut: #defines inside #included files are not seen, and growth through nested macro ARGUMENTS is not
// counted, neither at a use site (D(D(D(x)))) nor inside function-like macro bodies (#define D(x) x x,
// #define E1(x) D(D(x)), #define E2(x) E1(E1(x)) ... E5 yields ~2^32 copies while the guard counts a few dozen
// tokens). That needs adversarial input; effect-parser output never contains directives.
bool macros_ok(const char *text, std::string &error)
{
	struct lexer_guard {
		cf_lexer lex;
		lexer_guard() { cf_lexer_init(&lex); }
		~lexer_guard() { cf_lexer_free(&lex); }
	} guard; // frees the lexer even if building the macro map throws
	cf_lexer &lex = guard.lex;
	cf_lexer_lex(&lex, text, nullptr); // false also for an unterminated comment; its tokens are still checked
	std::map<std::string, std::vector<std::string>> uses;
	for (const cf_token *t = cf_lexer_get_tokens(&lex); t && t->type != CFTOKEN_NONE; ++t) {
		if (t->type != CFTOKEN_OTHER || t->str.array[0] != '#') {
			continue;
		}
		const cf_token *d = t + 1;
		while (d->type == CFTOKEN_SPACETAB) {
			++d;
		}
		if (d->type != CFTOKEN_NAME || strref_cmp(&d->str, "define") != 0) {
			continue;
		}
		const cf_token *n = d + 1;
		while (n->type == CFTOKEN_SPACETAB) {
			++n;
		}
		if (n->type != CFTOKEN_NAME) {
			continue;
		}
		std::vector<std::string> &body = uses[std::string(n->str.array, n->str.len)];
		for (t = n + 1; t->type != CFTOKEN_NONE && t->type != CFTOKEN_NEWLINE; ++t) {
			if (t->type == CFTOKEN_NAME) {
				body.emplace_back(t->str.array, t->str.len);
			} else if (t->type != CFTOKEN_SPACETAB) {
				body.emplace_back(); // any other token expands to itself
			}
		}
		if (t->type == CFTOKEN_NONE) {
			break;
		}
	}

	// Iterative depth-first search: a cycle, or a path longer than max_macro_depth, is rejected. On the way back
	// (post-order, so the graph is a DAG by then) each macro's worst-case expansion size is summed from its body,
	// saturating just above the limit.
	constexpr size_t max_macro_depth = 64;
	constexpr uint64_t max_expansion = 1000000;
	std::map<std::string, int> state; // 1 = on the current path, 2 = done
	std::map<std::string, uint64_t> size;
	std::vector<std::pair<const std::string *, size_t>> path;
	for (const auto &root : uses) {
		if (state[root.first]) {
			continue;
		}
		state[root.first] = 1;
		path.emplace_back(&root.first, 0);
		while (!path.empty()) {
			const std::string &m = *path.back().first;
			const std::vector<std::string> &body = uses[m];
			size_t &i = path.back().second;
			if (i == body.size()) {
				uint64_t total = 0;
				for (const std::string &u : body) {
					total = (std::min)(total + (uses.count(u) ? size[u] : 1), max_expansion + 1);
				}
				if (total > max_expansion) {
					error = "macro expansion of '" + m + "' exceeds " +
						std::to_string(max_expansion) + " tokens";
					return false;
				}
				size[m] = total;
				state[m] = 2;
				path.pop_back();
				continue;
			}
			const std::string &d = body[i++];
			auto def = uses.find(d);
			if (def == uses.end()) {
				continue;
			}
			int &s = state[d];
			if (s == 1) {
				error = "recursive macro '" + d + "'";
				return false;
			}
			if (s == 0) {
				if (path.size() >= max_macro_depth) {
					error = "macro expansion deeper than " + std::to_string(max_macro_depth) +
						" at '" + d + "'";
					return false;
				}
				s = 1;
				path.emplace_back(&def->first, 0);
			}
		}
	}
	return true;
}

// Appends the quoted source line after each glslang "ERROR: 0:<line>:" message.
std::string annotate(const char *log, const char *source)
{
	std::vector<std::string> lines(1);
	for (const char *p = source; *p; ++p) {
		if (*p == '\n') {
			lines.emplace_back();
		} else if (*p != '\r') {
			lines.back() += *p;
		}
	}

	std::string result;
	const char *p = log;
	while (*p) {
		const char *eol = strchr(p, '\n');
		std::string line(p, eol ? eol - p : strlen(p));
		result += line + "\n";
		unsigned n = 0;
		if (sscanf(line.c_str(), "ERROR: %*u:%u:", &n) == 1 && n >= 1 && n <= lines.size()) {
			result += "    > " + lines[n - 1] + "\n";
		}
		if (!eol) {
			break;
		}
		p = eol + 1;
	}
	return result;
}

void fill_shader(translator &t, gs_shader *shader)
{
	uint32_t binding = 1;
	for (size_t i = 0; i < t.parser.params.num; i++) {
		shader_var *var = t.parser.params.array + i;
		bool texture = is_texture(var->type);
		if (!texture && var->var_type != SHADER_VAR_UNIFORM) {
			continue;
		}
		gs_shader_param p;
		p.shader = shader;
		p.name = var->name;
		p.type = get_shader_param_type(var->type);
		p.array_count = var->array_count;
		p.def.assign(var->default_val.array, var->default_val.array + var->default_val.num);
		if (texture) {
			p.binding = binding++;
			p.sampler = var->gl_sampler_id;
		}
		shader->params.push_back(std::move(p));
	}
	for (size_t i = 0; i < t.parser.samplers.num; i++) {
		gs_sampler_info info;
		shader_sampler_convert(t.parser.samplers.array + i, &info);
		shader->samplers.push_back(info);
	}
	shader->inputs = std::move(t.inputs);
	shader->max_varying = t.max_varying;
	shader->set = t.type == GS_SHADER_PIXEL ? 1 : 0;
}

bool compile(const std::string &glsl, enum gs_shader_type type, std::vector<uint32_t> &spirv, gs_shader *shader,
	     const char *hlsl, std::string &error)
{
	EShLanguage lang = type == GS_SHADER_VERTEX ? EShLangVertex : EShLangFragment;
	glslang::TShader sh(lang);
	const char *src = glsl.c_str();
	sh.setStrings(&src, 1);
	sh.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
	sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
	sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
	EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
	if (!sh.parse(GetDefaultResources(), 450, false, msgs)) {
		error = annotate(sh.getInfoLog(), hlsl);
		return false;
	}

	glslang::TProgram program;
	program.addShader(&sh);
	if (!program.link(msgs)) {
		error = program.getInfoLog();
		return false;
	}

	if (shader) {
		program.buildReflection(EShReflectionAllBlockVariables);
		for (int i = 0; i < program.getNumUniformBlocks(); ++i) {
			const glslang::TObjectReflection &b = program.getUniformBlock(i);
			if (b.name == "obs_uniforms") {
				shader->uniforms.assign((size_t)b.size, 0);
			}
		}
		for (int i = 0; i < program.getNumUniformVariables(); ++i) {
			const glslang::TObjectReflection &u = program.getUniform(i);
			std::string name = u.name;
			size_t bracket = name.find('[');
			if (bracket != std::string::npos) {
				name.resize(bracket);
			}
			for (gs_shader_param &p : shader->params) {
				if (p.type != GS_SHADER_PARAM_TEXTURE && p.name == name && u.offset >= 0) {
					p.offset = (uint32_t)u.offset;
					p.stride = (uint32_t)u.arrayStride;
				}
			}
		}
	}

	glslang::SpvOptions options;
	options.disableOptimizer = true; // compile latency matters more than SPIR-V size here
	options.validate = false;
	spv::SpvBuildLogger logger;
	glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &logger, &options);
	if (spirv.empty()) {
		error = "SPIR-V generation failed: " + logger.getAllMessages();
	}
	return !spirv.empty();
}

struct job {
	const char *hlsl;
	const char *file;
	enum gs_shader_type type;
	gs_shader *shader;
	std::vector<uint32_t> spirv;
	std::string error;
};

void run(void *p)
{
	job &j = *(job *)p;
	try {
		// The shared lexer reads (and in strings writes) past the end when a splice ends exactly at the NUL
		// (cf-lexer.c cf_pass_any_splices, :191, :198, :268-286). A splice takes at most '\' plus two bytes, and
		// "\n\n" is not one newline pair, so with it appended every splice stops on an appended '\n' before the
		// NUL, whether the input ends in '\', "\\\r", "\\\n" or "\\\n\r". It changes no line numbers.
		const std::string text = std::string(j.hlsl) + "\n\n";
		if (!nesting_ok(text.c_str(), j.error) || !macros_ok(text.c_str(), j.error)) {
			return;
		}
		translator t(j.type);
		bool parsed = shader_parse(&t.parser, text.c_str(), j.file);
		char *errors = shader_parser_geterrors(&t.parser);
		if (errors) {
			j.error = errors;
			bfree(errors);
		}
		if (!parsed) {
			if (j.error.empty()) {
				j.error = "shader parser failed";
			}
			return;
		}
		j.error.clear(); // parser warnings only
		if (!t.build()) {
			j.error = t.error;
			return;
		}
		if (j.shader) {
			fill_shader(t, j.shader);
		}
		if (!compile(t.out, j.type, j.spirv, j.shader, j.hlsl, j.error)) {
			j.spirv.clear();
			blog(LOG_DEBUG, "[libobs-vulkan] generated GLSL for %s:\n%s", j.file, t.out.c_str());
		}
	} catch (const std::exception &e) {
		j.spirv.clear();
		j.error = std::string("exception while compiling shader: ") + e.what();
	} catch (...) {
		j.spirv.clear();
		j.error = "unknown exception while compiling shader";
	}
}

#ifdef _WIN32
// Structured exceptions (access violation, stack overflow) inside the shared parser or glslang are NOT survived:
// destructors were skipped, glslang may hold its global lock and the heap may be corrupt. Log, then let the
// exception continue to OBS's crash handler.
int log_fatal(unsigned long code, const job *j)
{
	// After a stack overflow almost no stack is left; logging could overflow again before OBS's crash handler runs.
	if (code != EXCEPTION_STACK_OVERFLOW) {
		blog(LOG_ERROR, "[libobs-vulkan] fatal exception 0x%08lX while compiling shader '%s'", code, j->file);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

// No C++ objects in this frame, so __try is allowed.
void run_logged(job *j)
{
	__try {
		run(j);
	} __except (log_fatal(GetExceptionCode(), j)) {
	}
}
#endif

} // namespace

std::vector<uint32_t> hlsl_to_spirv(const char *hlsl, enum gs_shader_type type, std::string &error, gs_shader *shader,
				    const char *file)
{
	if (!hlsl) {
		error = "no shader text";
		return {};
	}
	job j{hlsl, file ? file : "shader", type, shader};
	vk_glslang_ref();
#ifdef _WIN32
	run_logged(&j);
#else
	run(&j);
#endif
	vk_glslang_unref();

	if (j.spirv.empty() && j.error.empty()) {
		j.error = "shader compile failed";
	}
	error = std::move(j.error);
	return std::move(j.spirv);
}
