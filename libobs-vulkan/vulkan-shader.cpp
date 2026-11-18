// gs_shader_* exports. Values go into a CPU shadow of the shader's std140 block; device_draw uploads it when dirty.
#include "vulkan-subsystem.hpp"

#include <graphics/vec2.h>
#include <graphics/matrix3.h>
#include <graphics/matrix4.h>
#include <util/base.h>
#include <util/bmem.h>

#include <new>

// Bytes one element of a param takes in gs_shader_set_val's input; 0 for types it does not accept.
static size_t element_size(enum gs_shader_param_type type)
{
	switch (type) {
	case GS_SHADER_PARAM_BOOL:
	case GS_SHADER_PARAM_INT:
	case GS_SHADER_PARAM_FLOAT:
		return 4;
	case GS_SHADER_PARAM_INT2:
	case GS_SHADER_PARAM_VEC2:
		return 8;
	case GS_SHADER_PARAM_INT3:
	case GS_SHADER_PARAM_VEC3:
		return 12;
	case GS_SHADER_PARAM_INT4:
	case GS_SHADER_PARAM_VEC4:
		return 16;
	case GS_SHADER_PARAM_MATRIX4X4:
		return 64;
	case GS_SHADER_PARAM_TEXTURE:
		return sizeof(struct gs_shader_texture);
	default:
		return 0;
	}
}

// Copies tightly packed elements into the block at the param's std140 offset and array stride.
static void write_uniform(gs_shader_param *p, const void *data, size_t elem)
{
	std::vector<uint8_t> &u = p->shader->uniforms;
	size_t count = p->array_count ? (size_t)p->array_count : 1;
	size_t stride = p->array_count ? p->stride : elem;
	if (p->offset == UINT32_MAX || p->offset + (count - 1) * stride + elem > u.size()) {
		blog(LOG_ERROR, "[libobs-vulkan] shader param '%s' has no room in the uniform block", p->name.c_str());
		return;
	}
	for (size_t i = 0; i < count; ++i) {
		uint8_t *dst = u.data() + p->offset + i * stride;
		const uint8_t *src = (const uint8_t *)data + i * elem;
		if (memcmp(dst, src, elem)) { // an unchanged value keeps this frame's upload
			memcpy(dst, src, elem);
			p->shader->dirty = true;
		}
	}
}

static gs_shader_t *shader_create_unguarded(gs_device_t *device, enum gs_shader_type type, const char *text,
					    const char *file, char **error_string)
{
	std::string error;
	gs_shader *s = new (std::nothrow) gs_shader;
	if (s) {
		s->device = device;
		s->type = type;
		s->spirv = hlsl_to_spirv(text, type, error, s, file);
	} else {
		error = "out of memory";
	}

	if (!s || s->spirv.empty()) {
		blog(LOG_ERROR, "[libobs-vulkan] %s shader '%s' failed:\n%s",
		     type == GS_SHADER_VERTEX ? "vertex" : "pixel", file ? file : "(unnamed)", error.c_str());
		if (error_string) {
			*error_string = bstrdup(error.c_str());
		}
		delete s;
		return nullptr;
	}

	for (gs_shader_param &p : s->params) {
		if (p.type != GS_SHADER_PARAM_TEXTURE && !p.def.empty()) {
			gs_shader_set_default(&p);
		}
	}
	s->viewproj = gs_shader_get_param_by_name(s, "ViewProj");
	s->world = gs_shader_get_param_by_name(s, "World");
	s->dirty = true;
	return s;
}

// Nothing may throw across the C boundary.
static gs_shader_t *shader_create(gs_device_t *device, enum gs_shader_type type, const char *text, const char *file,
				  char **error_string)
{
	try {
		return shader_create_unguarded(device, type, text, file, error_string);
	} catch (...) {
		if (error_string) {
			*error_string = bstrdup("exception while creating shader");
		}
		return nullptr;
	}
}

extern "C" {

EXPORT gs_shader_t *device_vertexshader_create(gs_device_t *device, const char *shader, const char *file,
					       char **error_string)
{
	return shader_create(device, GS_SHADER_VERTEX, shader, file, error_string);
}

EXPORT gs_shader_t *device_pixelshader_create(gs_device_t *device, const char *shader, const char *file,
					      char **error_string)
{
	return shader_create(device, GS_SHADER_PIXEL, shader, file, error_string);
}

EXPORT void gs_shader_destroy(gs_shader_t *shader)
{
	if (shader && shader->device) {
		shader->device->pipelines.forget(shader);
	}
	delete shader;
}

EXPORT int gs_shader_get_num_params(const gs_shader_t *shader)
{
	return (int)shader->params.size();
}

EXPORT gs_sparam_t *gs_shader_get_param_by_idx(gs_shader_t *shader, uint32_t param)
{
	return param < shader->params.size() ? &shader->params[param] : nullptr;
}

EXPORT gs_sparam_t *gs_shader_get_param_by_name(gs_shader_t *shader, const char *name)
{
	for (gs_shader_param &p : shader->params) {
		if (p.name == name) {
			return &p;
		}
	}
	return nullptr;
}

EXPORT gs_sparam_t *gs_shader_get_viewproj_matrix(const gs_shader_t *shader)
{
	return shader->viewproj;
}

EXPORT gs_sparam_t *gs_shader_get_world_matrix(const gs_shader_t *shader)
{
	return shader->world;
}

EXPORT void gs_shader_get_param_info(const gs_sparam_t *param, struct gs_shader_param_info *info)
{
	info->type = param->type;
	info->name = param->name.c_str();
}

EXPORT void gs_shader_set_val(gs_sparam_t *param, const void *val, size_t size)
{
	size_t elem = element_size(param->type);
	size_t expected = elem * (param->array_count ? param->array_count : 1);
	if (!elem) {
		// shortcut: float3x3 (std140 mat3, 48 bytes) and other types get_shader_param_type maps to UNKNOWN are not
		// settable yet; carried to a later task.
		if (!param->warned) {
			blog(LOG_WARNING,
			     "[libobs-vulkan] shader param '%s' has an unsupported type; values are ignored",
			     param->name.c_str());
			param->warned = true;
		}
		return;
	}
	if (size != expected) {
		blog(LOG_ERROR, "[libobs-vulkan] gs_shader_set_val: '%s' expects %zu bytes, got %zu",
		     param->name.c_str(), expected, size);
		return;
	}
	if (param->type == GS_SHADER_PARAM_TEXTURE) {
		struct gs_shader_texture tex;
		memcpy(&tex, val, sizeof(tex));
		param->texture = tex.tex;
		param->srgb = tex.srgb;
		return;
	}
	write_uniform(param, val, elem);
}

EXPORT void gs_shader_set_bool(gs_sparam_t *param, bool val)
{
	int i = val;
	gs_shader_set_val(param, &i, sizeof(i));
}

EXPORT void gs_shader_set_float(gs_sparam_t *param, float val)
{
	gs_shader_set_val(param, &val, sizeof(val));
}

EXPORT void gs_shader_set_int(gs_sparam_t *param, int val)
{
	gs_shader_set_val(param, &val, sizeof(val));
}

// As GL: a matrix3 is widened to a matrix4 (the only matrix param type the effect system has).
EXPORT void gs_shader_set_matrix3(gs_sparam_t *param, const struct matrix3 *val)
{
	struct matrix4 mat;
	matrix4_from_matrix3(&mat, val);
	gs_shader_set_val(param, &mat, sizeof(mat));
}

EXPORT void gs_shader_set_matrix4(gs_sparam_t *param, const struct matrix4 *val)
{
	gs_shader_set_val(param, val, sizeof(*val));
}

EXPORT void gs_shader_set_vec2(gs_sparam_t *param, const struct vec2 *val)
{
	gs_shader_set_val(param, val->ptr, sizeof(float) * 2);
}

EXPORT void gs_shader_set_vec3(gs_sparam_t *param, const struct vec3 *val)
{
	gs_shader_set_val(param, val->ptr, sizeof(float) * 3);
}

EXPORT void gs_shader_set_vec4(gs_sparam_t *param, const struct vec4 *val)
{
	gs_shader_set_val(param, val->ptr, sizeof(float) * 4);
}

EXPORT void gs_shader_set_texture(gs_sparam_t *param, gs_texture_t *val)
{
	param->texture = val;
}

EXPORT void gs_shader_set_default(gs_sparam_t *param)
{
	if (param->type != GS_SHADER_PARAM_TEXTURE && !param->def.empty()) {
		gs_shader_set_val(param, param->def.data(), param->def.size());
	}
}

EXPORT void gs_shader_set_next_sampler(gs_sparam_t *param, gs_samplerstate_t *sampler)
{
	param->next_sampler = sampler;
}
}
