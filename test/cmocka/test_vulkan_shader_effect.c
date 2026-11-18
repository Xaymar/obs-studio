// Runs .effect files through the shared effect parser (effect-parser.c is compiled into this test, since ep_parse
// is not exported) the way gs_effect_create does. The gs_* shader calls the parser makes resolve to the overrides
// below, which forward to the Vulkan module, so no graphics subsystem (and no device) is needed.
#include <graphics/effect.h>
#include <graphics/effect-parser.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "test_vulkan_shader_effect.h"

struct vkt_shader_fns vkt;

gs_shader_t *gs_vertexshader_create(const char *shader, const char *file, char **error_string)
{
	gs_shader_t *s = vkt.vertexshader_create(vkt.device, shader, file, error_string);
	if (vkt.on_shader)
		vkt.on_shader(GS_SHADER_VERTEX, shader, s);
	return s;
}

gs_shader_t *gs_pixelshader_create(const char *shader, const char *file, char **error_string)
{
	gs_shader_t *s = vkt.pixelshader_create(vkt.device, shader, file, error_string);
	if (vkt.on_shader)
		vkt.on_shader(GS_SHADER_PIXEL, shader, s);
	return s;
}

gs_sparam_t *gs_shader_get_param_by_name(gs_shader_t *shader, const char *name)
{
	return shader ? vkt.get_param_by_name(shader, name) : NULL;
}

void gs_shader_destroy(gs_shader_t *shader)
{
	if (shader)
		vkt.shader_destroy(shader);
}

const char *gs_preprocessor_name(void)
{
	return "_vulkan";
}

gs_effect_t *vkt_effect_create(const char *file, char **errors)
{
	char *text = os_quick_read_utf8_file(file);
	if (!text)
		return NULL;

	gs_effect_t *effect = bzalloc(sizeof(*effect));
	effect->effect_path = bstrdup(file);

	struct effect_parser ep;
	ep_init(&ep);
	bool ok = ep_parse(&ep, effect, text, file);
	if (!ok && errors)
		*errors = error_data_buildstring(&ep.cfp.error_list);
	ep_free(&ep);
	bfree(text);

	if (!ok) {
		vkt_effect_destroy(effect);
		return NULL;
	}
	return effect;
}

void vkt_effect_destroy(gs_effect_t *effect)
{
	effect_free(effect);
	bfree(effect);
}

gs_shader_t *vkt_effect_shader(gs_effect_t *effect, const char *technique, enum gs_shader_type type)
{
	for (size_t i = 0; i < effect->techniques.num; i++) {
		struct gs_effect_technique *t = effect->techniques.array + i;
		if (strcmp(t->name, technique) == 0 && t->passes.num)
			return type == GS_SHADER_VERTEX ? t->passes.array[0].vertshader
							: t->passes.array[0].pixelshader;
	}
	return NULL;
}

size_t vkt_effect_techniques(gs_effect_t *effect)
{
	return effect->techniques.num;
}
