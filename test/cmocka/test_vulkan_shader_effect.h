#pragma once

#include <graphics/graphics.h>

#ifdef __cplusplus
extern "C" {
#endif

// Module exports the effect parser's shader calls forward to; on_shader sees every shader the parser creates.
struct vkt_shader_fns {
	gs_shader_t *(*vertexshader_create)(gs_device_t *, const char *, const char *, char **);
	gs_shader_t *(*pixelshader_create)(gs_device_t *, const char *, const char *, char **);
	gs_sparam_t *(*get_param_by_name)(gs_shader_t *, const char *);
	void (*shader_destroy)(gs_shader_t *);
	void (*on_shader)(enum gs_shader_type type, const char *text, gs_shader_t *shader);
	gs_device_t *device; // passed to the create calls; NULL compiles without a device
};
extern struct vkt_shader_fns vkt;

// NULL (with *errors set when the parser reported any) when parsing or any shader compile failed.
gs_effect_t *vkt_effect_create(const char *file, char **errors);
void vkt_effect_destroy(gs_effect_t *effect);
size_t vkt_effect_techniques(gs_effect_t *effect);
// First pass of `technique`.
gs_shader_t *vkt_effect_shader(gs_effect_t *effect, const char *technique, enum gs_shader_type type);

#ifdef __cplusplus
}
#endif
