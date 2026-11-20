/******************************************************************************
    Copyright (C) 2026 by Narta Xaymar Dirks <info@xaymar.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

/* Null graphics module for unit tests: no GPU, but objects keep their description and CPU-side contents so that
 * libobs can map, copy, stage and read back real memory, and shaders keep the parameters libobs' effect system binds. */

#include <stdlib.h>
#include <string.h>

#include <util/base.h>
#include <util/bmem.h>
#include <util/darray.h>
#include <util/threading.h>
#include <graphics/graphics.h>
#include <graphics/device-exports.h>
#include <graphics/matrix3.h>
#include <graphics/matrix4.h>
#include <graphics/shader-parser.h>
#include <graphics/vec2.h>
#include <graphics/vec3.h>
#include <graphics/vec4.h>

#include "null-graphics.h"

/* ------------------------------------------------------------------------- */
/* Call counting                                                             */

enum null_call {
#define NULL_CALL(name) NULL_CALL_##name,
#include "null-calls.h"
#undef NULL_CALL
	NULL_CALL_COUNT
};

static const char *const call_names[NULL_CALL_COUNT] = {
#define NULL_CALL(name) #name,
#include "null-calls.h"
#undef NULL_CALL
};

static volatile long call_counts[NULL_CALL_COUNT];
static volatile long live_objects;

#define COUNT(name) os_atomic_inc_long(&call_counts[NULL_CALL_##name])

/* Failure injection: the next `count` calls of a named export fail, see null_gs_fail_next() */
static volatile long fail_counts[NULL_CALL_COUNT];

static bool fail_call(enum null_call call)
{
	long left = os_atomic_load_long(&fail_counts[call]);
	while (left > 0) {
		if (os_atomic_compare_exchange_long(&fail_counts[call], &left, left - 1))
			return true;
	}
	return false;
}

#define FAIL_IF_ASKED(name, ret)                  \
	do {                                      \
		if (fail_call(NULL_CALL_##name)) \
			return ret;               \
	} while (false)

EXPORT bool null_gs_fail_next(const char *name, long count)
{
	for (size_t i = 0; name && i < NULL_CALL_COUNT; i++) {
		if (strcmp(call_names[i], name) == 0) {
			os_atomic_set_long(&fail_counts[i], count);
			return true;
		}
	}
	return false;
}

EXPORT void null_gs_reset_failures(void)
{
	for (size_t i = 0; i < NULL_CALL_COUNT; i++)
		os_atomic_set_long(&fail_counts[i], 0);
}

EXPORT long null_gs_get_call_count(const char *name)
{
	for (size_t i = 0; name && i < NULL_CALL_COUNT; i++) {
		if (strcmp(call_names[i], name) == 0)
			return os_atomic_load_long(&call_counts[i]);
	}
	return -1;
}

EXPORT void null_gs_reset_call_counts(void)
{
	for (size_t i = 0; i < NULL_CALL_COUNT; i++)
		os_atomic_set_long(&call_counts[i], 0);
}

EXPORT long null_gs_get_live_objects(void)
{
	return os_atomic_load_long(&live_objects);
}

static inline void *obj_alloc(size_t size)
{
	os_atomic_inc_long(&live_objects);
	return bzalloc(size);
}

static inline void obj_free(void *obj)
{
	if (obj) {
		os_atomic_dec_long(&live_objects);
		bfree(obj);
	}
}

/* ------------------------------------------------------------------------- */
/* Objects                                                                   */

struct projection {
	bool ortho;
	float left, right, top, bottom, znear, zfar;
};

struct gs_device {
	struct gs_swap_chain *cur_swap;
	gs_texture_t *cur_render_target;
	gs_zstencil_t *cur_zstencil;
	int cur_render_side;
	enum gs_color_space cur_color_space;
	bool srgb;

	gs_shader_t *cur_vertex_shader;
	gs_shader_t *cur_pixel_shader;
	gs_vertbuffer_t *cur_vertex_buffer;
	gs_indexbuffer_t *cur_index_buffer;
	gs_texture_t *cur_textures[GS_MAX_TEXTURES];
	gs_samplerstate_t *cur_samplers[GS_MAX_TEXTURES];

	enum gs_cull_mode cull_mode;
	struct gs_rect viewport;
	struct projection projection;
	DARRAY(struct projection) projection_stack;
};

struct gs_swap_chain {
	gs_device_t *device;
	struct gs_init_data info;
};

struct gs_texture {
	gs_device_t *device;
	enum gs_texture_type type;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	enum gs_color_format format;
	uint32_t levels;
	uint32_t flags;

	/* Top mip level of every face (cube) or slice (volume), tightly packed. */
	uint8_t *data;
	uint32_t linesize;
	size_t face_size;
	bool mapped;
};

struct gs_stage_surface {
	gs_device_t *device;
	uint32_t width;
	uint32_t height;
	enum gs_color_format format;
	uint8_t *data;
	uint32_t linesize;
	bool mapped;
};

struct gs_zstencil_buffer {
	gs_device_t *device;
	uint32_t width;
	uint32_t height;
	enum gs_zstencil_format format;
};

struct gs_sampler_state {
	gs_device_t *device;
	struct gs_sampler_info info;
};

struct gs_vertex_buffer {
	gs_device_t *device;
	struct gs_vb_data *data;
	uint32_t flags;
};

struct gs_index_buffer {
	gs_device_t *device;
	enum gs_index_type type;
	void *indices;
	size_t num;
	uint32_t flags;
};

struct gs_timer {
	gs_device_t *device;
};

struct gs_timer_range {
	gs_device_t *device;
};

struct gs_shader_param {
	char *name;
	enum gs_shader_param_type type;
	int array_count;
	gs_shader_t *shader;

	DARRAY(uint8_t) cur_value;
	DARRAY(uint8_t) def_value;
	gs_texture_t *texture;
	bool srgb;
	gs_samplerstate_t *next_sampler;
};

struct gs_shader {
	gs_device_t *device;
	enum gs_shader_type type;
	DARRAY(struct gs_shader_param) params;
	gs_sparam_t *viewproj;
	gs_sparam_t *world;
};

/* ------------------------------------------------------------------------- */
/* Device                                                                    */

EXPORT const char *device_get_name(void)
{
	COUNT(device_get_name);
	return "Null";
}

EXPORT const char *gpu_get_driver_version(void)
{
	COUNT(gpu_get_driver_version);
	return "0.0.0";
}

EXPORT const char *gpu_get_renderer(void)
{
	COUNT(gpu_get_renderer);
	return "Null Renderer";
}

EXPORT uint64_t gpu_get_dmem(void)
{
	COUNT(gpu_get_dmem);
	return 0;
}

EXPORT uint64_t gpu_get_smem(void)
{
	COUNT(gpu_get_smem);
	return 0;
}

/* Report Direct3D 11 so libobs takes the same paths as on a real Windows device */
EXPORT int device_get_type(void)
{
	COUNT(device_get_type);
	return GS_DEVICE_DIRECT3D_11;
}

EXPORT bool device_enum_adapters(gs_device_t *device, bool (*callback)(void *param, const char *name, uint32_t id),
				 void *param)
{
	UNUSED_PARAMETER(device);
	COUNT(device_enum_adapters);
	callback(param, "Null Adapter", 0);
	return true;
}

EXPORT const char *device_preprocessor_name(void)
{
	COUNT(device_preprocessor_name);
	return "_D3D11";
}

EXPORT uint32_t gs_get_adapter_count(void)
{
	COUNT(gs_get_adapter_count);
	return 1;
}

EXPORT int device_create(gs_device_t **p_device, uint32_t adapter)
{
	UNUSED_PARAMETER(adapter);
	COUNT(device_create);
	FAIL_IF_ASKED(device_create, GS_ERROR_FAIL);

	gs_device_t *device = obj_alloc(sizeof(*device));
	device->cur_color_space = GS_CS_SRGB;
	device->cull_mode = GS_BACK;
	*p_device = device;
	return GS_SUCCESS;
}

EXPORT void device_destroy(gs_device_t *device)
{
	COUNT(device_destroy);
	if (!device)
		return;

	da_free(device->projection_stack);
	obj_free(device);
}

EXPORT void device_enter_context(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	COUNT(device_enter_context);
}

EXPORT void device_leave_context(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	COUNT(device_leave_context);
}

EXPORT void *device_get_device_obj(gs_device_t *device)
{
	COUNT(device_get_device_obj);
	return device;
}

EXPORT gs_swapchain_t *device_swapchain_create(gs_device_t *device, const struct gs_init_data *data)
{
	COUNT(device_swapchain_create);
	FAIL_IF_ASKED(device_swapchain_create, NULL);

	gs_swapchain_t *swap = obj_alloc(sizeof(*swap));
	swap->device = device;
	swap->info = *data;
	return swap;
}

EXPORT void device_resize(gs_device_t *device, uint32_t x, uint32_t y)
{
	COUNT(device_resize);
	if (device->cur_swap) {
		device->cur_swap->info.cx = x;
		device->cur_swap->info.cy = y;
	}
}

EXPORT enum gs_color_space device_get_color_space(gs_device_t *device)
{
	COUNT(device_get_color_space);
	return device->cur_color_space;
}

EXPORT void device_update_color_space(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	COUNT(device_update_color_space);
}

EXPORT void device_get_size(const gs_device_t *device, uint32_t *x, uint32_t *y)
{
	COUNT(device_get_size);
	*x = device->cur_swap ? device->cur_swap->info.cx : 0;
	*y = device->cur_swap ? device->cur_swap->info.cy : 0;
}

EXPORT uint32_t device_get_width(const gs_device_t *device)
{
	COUNT(device_get_width);
	return device->cur_swap ? device->cur_swap->info.cx : 0;
}

EXPORT uint32_t device_get_height(const gs_device_t *device)
{
	COUNT(device_get_height);
	return device->cur_swap ? device->cur_swap->info.cy : 0;
}

/* ------------------------------------------------------------------------- */
/* Textures                                                                  */

static gs_texture_t *texture_create(gs_device_t *device, enum gs_texture_type type, uint32_t width, uint32_t height,
				    uint32_t depth, enum gs_color_format format, uint32_t levels, uint32_t flags)
{
	uint32_t bpp = gs_get_format_bpp(format);
	if (!width || !height || !depth || !bpp) {
		blog(LOG_ERROR, "null: invalid texture description %ux%ux%u format %d", width, height, depth, format);
		return NULL;
	}

	uint32_t faces = type == GS_TEXTURE_CUBE ? 6 : depth;
	gs_texture_t *tex = obj_alloc(sizeof(*tex));
	tex->device = device;
	tex->type = type;
	tex->width = width;
	tex->height = height;
	tex->depth = depth;
	tex->format = format;
	tex->levels = levels;
	tex->flags = flags;
	tex->linesize = width * bpp / 8;
	tex->face_size = (size_t)tex->linesize * height;
	tex->data = bzalloc(tex->face_size * faces);
	return tex;
}

EXPORT gs_texture_t *device_texture_create(gs_device_t *device, uint32_t width, uint32_t height,
					   enum gs_color_format color_format, uint32_t levels, const uint8_t **data,
					   uint32_t flags)
{
	COUNT(device_texture_create);
	FAIL_IF_ASKED(device_texture_create, NULL);

	gs_texture_t *tex = texture_create(device, GS_TEXTURE_2D, width, height, 1, color_format, levels, flags);
	if (tex && data && data[0])
		memcpy(tex->data, data[0], tex->face_size);
	return tex;
}

EXPORT gs_texture_t *device_cubetexture_create(gs_device_t *device, uint32_t size, enum gs_color_format color_format,
					       uint32_t levels, const uint8_t **data, uint32_t flags)
{
	COUNT(device_cubetexture_create);
	FAIL_IF_ASKED(device_cubetexture_create, NULL);

	gs_texture_t *tex = texture_create(device, GS_TEXTURE_CUBE, size, size, 1, color_format, levels, flags);
	if (tex && data) {
		/* data holds every level of a face before the next face */
		uint32_t stride = levels ? levels : 1;
		for (uint32_t face = 0; face < 6; face++) {
			if (data[face * stride])
				memcpy(tex->data + tex->face_size * face, data[face * stride], tex->face_size);
		}
	}
	return tex;
}

EXPORT gs_texture_t *device_voltexture_create(gs_device_t *device, uint32_t width, uint32_t height, uint32_t depth,
					      enum gs_color_format color_format, uint32_t levels,
					      const uint8_t *const *data, uint32_t flags)
{
	COUNT(device_voltexture_create);
	FAIL_IF_ASKED(device_voltexture_create, NULL);

	gs_texture_t *tex = texture_create(device, GS_TEXTURE_3D, width, height, depth, color_format, levels, flags);
	if (tex && data && data[0])
		memcpy(tex->data, data[0], tex->face_size * depth);
	return tex;
}

EXPORT enum gs_texture_type device_get_texture_type(const gs_texture_t *texture)
{
	COUNT(device_get_texture_type);
	return texture->type;
}

static void texture_destroy(gs_texture_t *tex)
{
	if (!tex)
		return;

	gs_device_t *device = tex->device;
	if (device->cur_render_target == tex)
		device->cur_render_target = NULL;
	for (size_t i = 0; i < GS_MAX_TEXTURES; i++) {
		if (device->cur_textures[i] == tex)
			device->cur_textures[i] = NULL;
	}

	bfree(tex->data);
	obj_free(tex);
}

EXPORT void gs_texture_destroy(gs_texture_t *tex)
{
	COUNT(gs_texture_destroy);
	texture_destroy(tex);
}

EXPORT uint32_t gs_texture_get_width(const gs_texture_t *tex)
{
	COUNT(gs_texture_get_width);
	return tex->width;
}

EXPORT uint32_t gs_texture_get_height(const gs_texture_t *tex)
{
	COUNT(gs_texture_get_height);
	return tex->height;
}

EXPORT enum gs_color_format gs_texture_get_color_format(const gs_texture_t *tex)
{
	COUNT(gs_texture_get_color_format);
	return tex->format;
}

EXPORT bool gs_texture_map(gs_texture_t *tex, uint8_t **ptr, uint32_t *linesize)
{
	COUNT(gs_texture_map);
	FAIL_IF_ASKED(gs_texture_map, false);

	/* Same restriction as the real backends */
	if (tex->type != GS_TEXTURE_2D || !(tex->flags & GS_DYNAMIC) || tex->mapped) {
		blog(LOG_ERROR, "null: gs_texture_map: texture is not a dynamic 2D texture or is already mapped");
		return false;
	}

	tex->mapped = true;
	*ptr = tex->data;
	*linesize = tex->linesize;
	return true;
}

EXPORT void gs_texture_unmap(gs_texture_t *tex)
{
	COUNT(gs_texture_unmap);
	tex->mapped = false;
}

EXPORT bool gs_texture_is_rect(const gs_texture_t *tex)
{
	UNUSED_PARAMETER(tex);
	COUNT(gs_texture_is_rect);
	return false;
}

EXPORT void *gs_texture_get_obj(gs_texture_t *tex)
{
	COUNT(gs_texture_get_obj);
	return tex;
}

EXPORT void gs_cubetexture_destroy(gs_texture_t *cubetex)
{
	COUNT(gs_cubetexture_destroy);
	texture_destroy(cubetex);
}

EXPORT uint32_t gs_cubetexture_get_size(const gs_texture_t *cubetex)
{
	COUNT(gs_cubetexture_get_size);
	return cubetex->width;
}

EXPORT enum gs_color_format gs_cubetexture_get_color_format(const gs_texture_t *cubetex)
{
	COUNT(gs_cubetexture_get_color_format);
	return cubetex->format;
}

EXPORT void gs_voltexture_destroy(gs_texture_t *voltex)
{
	COUNT(gs_voltexture_destroy);
	texture_destroy(voltex);
}

EXPORT uint32_t gs_voltexture_get_width(const gs_texture_t *voltex)
{
	COUNT(gs_voltexture_get_width);
	return voltex->width;
}

EXPORT uint32_t gs_voltexture_get_height(const gs_texture_t *voltex)
{
	COUNT(gs_voltexture_get_height);
	return voltex->height;
}

EXPORT uint32_t gs_voltexture_get_depth(const gs_texture_t *voltex)
{
	COUNT(gs_voltexture_get_depth);
	return voltex->depth;
}

EXPORT enum gs_color_format gs_voltexture_get_color_format(const gs_texture_t *voltex)
{
	COUNT(gs_voltexture_get_color_format);
	return voltex->format;
}

/* Two-plane YUV textures (and with them libobs' GPU encoding path) are off unless a test sets
 * NULL_GRAPHICS_YUV_TEXTURES=1 before creating the device. */
static bool yuv_textures_enabled(void)
{
	const char *val = getenv("NULL_GRAPHICS_YUV_TEXTURES");
	return val && *val == '1';
}

EXPORT bool device_nv12_available(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	COUNT(device_nv12_available);
	return yuv_textures_enabled();
}

EXPORT bool device_p010_available(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	COUNT(device_p010_available);
	return yuv_textures_enabled();
}

static bool create_two_plane(gs_device_t *device, gs_texture_t **tex_y, gs_texture_t **tex_uv, uint32_t width,
			     uint32_t height, uint32_t flags, enum gs_color_format y_format,
			     enum gs_color_format uv_format)
{
	*tex_y = NULL;
	*tex_uv = NULL;
	if (!yuv_textures_enabled())
		return false;

	*tex_y = device_texture_create(device, width, height, y_format, 1, NULL, flags);
	*tex_uv = device_texture_create(device, width / 2, height / 2, uv_format, 1, NULL, flags);
	return true;
}

EXPORT bool device_texture_create_nv12(gs_device_t *device, gs_texture_t **tex_y, gs_texture_t **tex_uv, uint32_t width,
				       uint32_t height, uint32_t flags)
{
	COUNT(device_texture_create_nv12);
	return create_two_plane(device, tex_y, tex_uv, width, height, flags, GS_R8, GS_R8G8);
}

EXPORT bool device_texture_create_p010(gs_device_t *device, gs_texture_t **tex_y, gs_texture_t **tex_uv, uint32_t width,
				       uint32_t height, uint32_t flags)
{
	COUNT(device_texture_create_p010);
	return create_two_plane(device, tex_y, tex_uv, width, height, flags, GS_R16, GS_RG16);
}

/* ------------------------------------------------------------------------- */
/* Stage surfaces, z-stencil, samplers                                       */

EXPORT gs_stagesurf_t *device_stagesurface_create(gs_device_t *device, uint32_t width, uint32_t height,
						  enum gs_color_format color_format)
{
	COUNT(device_stagesurface_create);
	FAIL_IF_ASKED(device_stagesurface_create, NULL);

	uint32_t bpp = gs_get_format_bpp(color_format);
	if (!width || !height || !bpp) {
		blog(LOG_ERROR, "null: invalid stage surface description %ux%u format %d", width, height, color_format);
		return NULL;
	}

	gs_stagesurf_t *surf = obj_alloc(sizeof(*surf));
	surf->device = device;
	surf->width = width;
	surf->height = height;
	surf->format = color_format;
	surf->linesize = width * bpp / 8;
	surf->data = bzalloc((size_t)surf->linesize * height);
	return surf;
}

EXPORT void gs_stagesurface_destroy(gs_stagesurf_t *stagesurf)
{
	COUNT(gs_stagesurface_destroy);
	if (!stagesurf)
		return;

	bfree(stagesurf->data);
	obj_free(stagesurf);
}

EXPORT uint32_t gs_stagesurface_get_width(const gs_stagesurf_t *stagesurf)
{
	COUNT(gs_stagesurface_get_width);
	return stagesurf->width;
}

EXPORT uint32_t gs_stagesurface_get_height(const gs_stagesurf_t *stagesurf)
{
	COUNT(gs_stagesurface_get_height);
	return stagesurf->height;
}

EXPORT enum gs_color_format gs_stagesurface_get_color_format(const gs_stagesurf_t *stagesurf)
{
	COUNT(gs_stagesurface_get_color_format);
	return stagesurf->format;
}

EXPORT bool gs_stagesurface_map(gs_stagesurf_t *stagesurf, uint8_t **data, uint32_t *linesize)
{
	COUNT(gs_stagesurface_map);
	FAIL_IF_ASKED(gs_stagesurface_map, false);
	if (stagesurf->mapped)
		return false;

	stagesurf->mapped = true;
	*data = stagesurf->data;
	*linesize = stagesurf->linesize;
	return true;
}

EXPORT void gs_stagesurface_unmap(gs_stagesurf_t *stagesurf)
{
	COUNT(gs_stagesurface_unmap);
	stagesurf->mapped = false;
}

#ifdef _WIN32
EXPORT gs_stagesurf_t *device_stagesurface_create_nv12(gs_device_t *device, uint32_t width, uint32_t height)
{
	COUNT(device_stagesurface_create_nv12);
	FAIL_IF_ASKED(device_stagesurface_create_nv12, NULL);
	if (!yuv_textures_enabled())
		return NULL;
	/* both planes in one buffer, the UV plane after the Y plane */
	return device_stagesurface_create(device, width, height + height / 2, GS_R8);
}

EXPORT gs_stagesurf_t *device_stagesurface_create_p010(gs_device_t *device, uint32_t width, uint32_t height)
{
	COUNT(device_stagesurface_create_p010);
	FAIL_IF_ASKED(device_stagesurface_create_p010, NULL);
	if (!yuv_textures_enabled())
		return NULL;
	/* both planes in one buffer, the UV plane after the Y plane */
	return device_stagesurface_create(device, width, height + height / 2, GS_R16);
}
#endif

EXPORT gs_zstencil_t *device_zstencil_create(gs_device_t *device, uint32_t width, uint32_t height,
					     enum gs_zstencil_format format)
{
	COUNT(device_zstencil_create);
	FAIL_IF_ASKED(device_zstencil_create, NULL);
	if (!width || !height || format == GS_ZS_NONE)
		return NULL;

	gs_zstencil_t *zs = obj_alloc(sizeof(*zs));
	zs->device = device;
	zs->width = width;
	zs->height = height;
	zs->format = format;
	return zs;
}

EXPORT void gs_zstencil_destroy(gs_zstencil_t *zstencil)
{
	COUNT(gs_zstencil_destroy);
	if (!zstencil)
		return;

	if (zstencil->device->cur_zstencil == zstencil)
		zstencil->device->cur_zstencil = NULL;
	obj_free(zstencil);
}

EXPORT gs_samplerstate_t *device_samplerstate_create(gs_device_t *device, const struct gs_sampler_info *info)
{
	COUNT(device_samplerstate_create);
	FAIL_IF_ASKED(device_samplerstate_create, NULL);

	gs_samplerstate_t *ss = obj_alloc(sizeof(*ss));
	ss->device = device;
	ss->info = *info;
	return ss;
}

EXPORT void gs_samplerstate_destroy(gs_samplerstate_t *samplerstate)
{
	COUNT(gs_samplerstate_destroy);
	if (!samplerstate)
		return;

	gs_device_t *device = samplerstate->device;
	for (size_t i = 0; i < GS_MAX_TEXTURES; i++) {
		if (device->cur_samplers[i] == samplerstate)
			device->cur_samplers[i] = NULL;
	}
	obj_free(samplerstate);
}

/* ------------------------------------------------------------------------- */
/* Buffers                                                                   */

/* Takes ownership of data, like the real backends; libobs duplicates it first when asked to. */
EXPORT gs_vertbuffer_t *device_vertexbuffer_create(gs_device_t *device, struct gs_vb_data *data, uint32_t flags)
{
	COUNT(device_vertexbuffer_create);
	/* like the real backends, the buffer owns the data even when it fails */
	if (!data || !data->num || fail_call(NULL_CALL_device_vertexbuffer_create)) {
		blog(LOG_ERROR, "null: vertex buffer created without data");
		gs_vbdata_destroy(data);
		return NULL;
	}

	gs_vertbuffer_t *vb = obj_alloc(sizeof(*vb));
	vb->device = device;
	vb->data = data;
	vb->flags = flags;
	return vb;
}

EXPORT void gs_vertexbuffer_destroy(gs_vertbuffer_t *vertbuffer)
{
	COUNT(gs_vertexbuffer_destroy);
	if (!vertbuffer)
		return;

	if (vertbuffer->device->cur_vertex_buffer == vertbuffer)
		vertbuffer->device->cur_vertex_buffer = NULL;
	gs_vbdata_destroy(vertbuffer->data);
	obj_free(vertbuffer);
}

static void copy_vb_data(struct gs_vb_data *dst, const struct gs_vb_data *src)
{
	size_t num = dst->num < src->num ? dst->num : src->num;

	if (dst->points && src->points)
		memcpy(dst->points, src->points, num * sizeof(struct vec3));
	if (dst->normals && src->normals)
		memcpy(dst->normals, src->normals, num * sizeof(struct vec3));
	if (dst->tangents && src->tangents)
		memcpy(dst->tangents, src->tangents, num * sizeof(struct vec3));
	if (dst->colors && src->colors)
		memcpy(dst->colors, src->colors, num * sizeof(uint32_t));

	size_t num_tex = dst->num_tex < src->num_tex ? dst->num_tex : src->num_tex;
	for (size_t i = 0; i < num_tex; i++) {
		struct gs_tvertarray *d = dst->tvarray + i;
		const struct gs_tvertarray *s = src->tvarray + i;
		if (d->array && s->array && d->width == s->width)
			memcpy(d->array, s->array, num * s->width * sizeof(float));
	}
}

EXPORT void gs_vertexbuffer_flush(gs_vertbuffer_t *vertbuffer)
{
	COUNT(gs_vertexbuffer_flush);
	if (!(vertbuffer->flags & GS_DYNAMIC))
		blog(LOG_ERROR, "null: gs_vertexbuffer_flush: vertex buffer is not dynamic");
}

EXPORT void gs_vertexbuffer_flush_direct(gs_vertbuffer_t *vertbuffer, const struct gs_vb_data *data)
{
	COUNT(gs_vertexbuffer_flush_direct);
	if (!(vertbuffer->flags & GS_DYNAMIC)) {
		blog(LOG_ERROR, "null: gs_vertexbuffer_flush_direct: vertex buffer is not dynamic");
		return;
	}
	if (data && data != vertbuffer->data)
		copy_vb_data(vertbuffer->data, data);
}

EXPORT struct gs_vb_data *gs_vertexbuffer_get_data(const gs_vertbuffer_t *vertbuffer)
{
	COUNT(gs_vertexbuffer_get_data);
	return vertbuffer->data;
}

static inline size_t index_size(enum gs_index_type type)
{
	return type == GS_UNSIGNED_SHORT ? sizeof(uint16_t) : sizeof(uint32_t);
}

/* Takes ownership of indices, like the real backends. */
EXPORT gs_indexbuffer_t *device_indexbuffer_create(gs_device_t *device, enum gs_index_type type, void *indices,
						   size_t num, uint32_t flags)
{
	COUNT(device_indexbuffer_create);
	if (!indices || !num || fail_call(NULL_CALL_device_indexbuffer_create)) {
		blog(LOG_ERROR, "null: index buffer created without indices");
		bfree(indices);
		return NULL;
	}

	gs_indexbuffer_t *ib = obj_alloc(sizeof(*ib));
	ib->device = device;
	ib->type = type;
	ib->indices = indices;
	ib->num = num;
	ib->flags = flags;
	return ib;
}

EXPORT void gs_indexbuffer_destroy(gs_indexbuffer_t *indexbuffer)
{
	COUNT(gs_indexbuffer_destroy);
	if (!indexbuffer)
		return;

	if (indexbuffer->device->cur_index_buffer == indexbuffer)
		indexbuffer->device->cur_index_buffer = NULL;
	bfree(indexbuffer->indices);
	obj_free(indexbuffer);
}

EXPORT void gs_indexbuffer_flush(gs_indexbuffer_t *indexbuffer)
{
	COUNT(gs_indexbuffer_flush);
	if (!(indexbuffer->flags & GS_DYNAMIC))
		blog(LOG_ERROR, "null: gs_indexbuffer_flush: index buffer is not dynamic");
}

EXPORT void gs_indexbuffer_flush_direct(gs_indexbuffer_t *indexbuffer, const void *data)
{
	COUNT(gs_indexbuffer_flush_direct);
	if (!(indexbuffer->flags & GS_DYNAMIC)) {
		blog(LOG_ERROR, "null: gs_indexbuffer_flush_direct: index buffer is not dynamic");
		return;
	}
	if (data && data != indexbuffer->indices)
		memcpy(indexbuffer->indices, data, indexbuffer->num * index_size(indexbuffer->type));
}

EXPORT void *gs_indexbuffer_get_data(const gs_indexbuffer_t *indexbuffer)
{
	COUNT(gs_indexbuffer_get_data);
	return indexbuffer->indices;
}

EXPORT size_t gs_indexbuffer_get_num_indices(const gs_indexbuffer_t *indexbuffer)
{
	COUNT(gs_indexbuffer_get_num_indices);
	return indexbuffer->num;
}

EXPORT enum gs_index_type gs_indexbuffer_get_type(const gs_indexbuffer_t *indexbuffer)
{
	COUNT(gs_indexbuffer_get_type);
	return indexbuffer->type;
}

/* ------------------------------------------------------------------------- */
/* Timers                                                                    */

EXPORT gs_timer_t *device_timer_create(gs_device_t *device)
{
	COUNT(device_timer_create);
	FAIL_IF_ASKED(device_timer_create, NULL);
	gs_timer_t *timer = obj_alloc(sizeof(*timer));
	timer->device = device;
	return timer;
}

EXPORT gs_timer_range_t *device_timer_range_create(gs_device_t *device)
{
	COUNT(device_timer_range_create);
	FAIL_IF_ASKED(device_timer_range_create, NULL);
	gs_timer_range_t *range = obj_alloc(sizeof(*range));
	range->device = device;
	return range;
}

EXPORT void gs_timer_destroy(gs_timer_t *timer)
{
	COUNT(gs_timer_destroy);
	obj_free(timer);
}

EXPORT void gs_timer_begin(gs_timer_t *timer)
{
	UNUSED_PARAMETER(timer);
	COUNT(gs_timer_begin);
}

EXPORT void gs_timer_end(gs_timer_t *timer)
{
	UNUSED_PARAMETER(timer);
	COUNT(gs_timer_end);
}

EXPORT bool gs_timer_get_data(gs_timer_t *timer, uint64_t *ticks)
{
	UNUSED_PARAMETER(timer);
	COUNT(gs_timer_get_data);
	*ticks = 0;
	return true;
}

EXPORT void gs_timer_range_destroy(gs_timer_range_t *range)
{
	COUNT(gs_timer_range_destroy);
	obj_free(range);
}

EXPORT void gs_timer_range_begin(gs_timer_range_t *range)
{
	UNUSED_PARAMETER(range);
	COUNT(gs_timer_range_begin);
}

EXPORT void gs_timer_range_end(gs_timer_range_t *range)
{
	UNUSED_PARAMETER(range);
	COUNT(gs_timer_range_end);
}

EXPORT bool gs_timer_range_get_data(gs_timer_range_t *range, bool *disjoint, uint64_t *frequency)
{
	UNUSED_PARAMETER(range);
	COUNT(gs_timer_range_get_data);
	*disjoint = false;
	*frequency = 1000000000;
	return true;
}

/* ------------------------------------------------------------------------- */
/* Shaders                                                                   */

static gs_shader_t *shader_create(gs_device_t *device, enum gs_shader_type type, const char *shader_str,
				  const char *file, char **error_string)
{
	struct shader_parser parser;
	shader_parser_init(&parser);

	if (!shader_parse(&parser, shader_str, file)) {
		char *errors = shader_parser_geterrors(&parser);
		blog(LOG_ERROR, "null: failed to parse shader '%s':\n%s", file ? file : "(null)", errors);
		if (error_string)
			*error_string = errors;
		else
			bfree(errors);
		shader_parser_free(&parser);
		return NULL;
	}

	gs_shader_t *shader = obj_alloc(sizeof(*shader));
	shader->device = device;
	shader->type = type;

	da_reserve(shader->params, parser.params.num);
	for (size_t i = 0; i < parser.params.num; i++) {
		struct shader_var *var = parser.params.array + i;
		struct gs_shader_param *param = da_push_back_new(shader->params);

		param->name = bstrdup(var->name);
		param->type = get_shader_param_type(var->type);
		param->array_count = var->array_count;
		param->shader = shader;
		da_move(param->def_value, var->default_val);
		da_copy(param->cur_value, param->def_value);
	}

	/* params no longer grows, so pointers into it stay valid */
	shader->viewproj = gs_shader_get_param_by_name(shader, "ViewProj");
	shader->world = gs_shader_get_param_by_name(shader, "World");

	shader_parser_free(&parser);
	return shader;
}

EXPORT gs_shader_t *device_vertexshader_create(gs_device_t *device, const char *shader, const char *file,
					       char **error_string)
{
	COUNT(device_vertexshader_create);
	FAIL_IF_ASKED(device_vertexshader_create, NULL);
	return shader_create(device, GS_SHADER_VERTEX, shader, file, error_string);
}

EXPORT gs_shader_t *device_pixelshader_create(gs_device_t *device, const char *shader, const char *file,
					      char **error_string)
{
	COUNT(device_pixelshader_create);
	FAIL_IF_ASKED(device_pixelshader_create, NULL);
	return shader_create(device, GS_SHADER_PIXEL, shader, file, error_string);
}

EXPORT void gs_shader_destroy(gs_shader_t *shader)
{
	COUNT(gs_shader_destroy);
	if (!shader)
		return;

	gs_device_t *device = shader->device;
	if (device->cur_vertex_shader == shader)
		device->cur_vertex_shader = NULL;
	if (device->cur_pixel_shader == shader)
		device->cur_pixel_shader = NULL;

	for (size_t i = 0; i < shader->params.num; i++) {
		struct gs_shader_param *param = shader->params.array + i;
		bfree(param->name);
		da_free(param->cur_value);
		da_free(param->def_value);
	}
	da_free(shader->params);
	obj_free(shader);
}

EXPORT int gs_shader_get_num_params(const gs_shader_t *shader)
{
	COUNT(gs_shader_get_num_params);
	return (int)shader->params.num;
}

EXPORT gs_sparam_t *gs_shader_get_param_by_idx(gs_shader_t *shader, uint32_t param)
{
	COUNT(gs_shader_get_param_by_idx);
	return param < shader->params.num ? shader->params.array + param : NULL;
}

EXPORT gs_sparam_t *gs_shader_get_param_by_name(gs_shader_t *shader, const char *name)
{
	COUNT(gs_shader_get_param_by_name);
	for (size_t i = 0; i < shader->params.num; i++) {
		struct gs_shader_param *param = shader->params.array + i;
		if (strcmp(param->name, name) == 0)
			return param;
	}
	return NULL;
}

EXPORT gs_sparam_t *gs_shader_get_viewproj_matrix(const gs_shader_t *shader)
{
	COUNT(gs_shader_get_viewproj_matrix);
	return shader->viewproj;
}

EXPORT gs_sparam_t *gs_shader_get_world_matrix(const gs_shader_t *shader)
{
	COUNT(gs_shader_get_world_matrix);
	return shader->world;
}

EXPORT void gs_shader_get_param_info(const gs_sparam_t *param, struct gs_shader_param_info *info)
{
	COUNT(gs_shader_get_param_info);
	if (!param)
		return;

	info->name = param->name;
	info->type = param->type;
}

static size_t param_value_size(const struct gs_shader_param *param)
{
	size_t size;

	switch (param->type) {
	case GS_SHADER_PARAM_BOOL:
	case GS_SHADER_PARAM_INT:
	case GS_SHADER_PARAM_FLOAT:
		size = 4;
		break;
	case GS_SHADER_PARAM_INT2:
	case GS_SHADER_PARAM_VEC2:
		size = 8;
		break;
	case GS_SHADER_PARAM_INT3:
	case GS_SHADER_PARAM_VEC3:
		size = 12;
		break;
	case GS_SHADER_PARAM_INT4:
	case GS_SHADER_PARAM_VEC4:
		size = 16;
		break;
	case GS_SHADER_PARAM_MATRIX4X4:
		size = sizeof(struct matrix4);
		break;
	case GS_SHADER_PARAM_TEXTURE:
		size = sizeof(struct gs_shader_texture);
		break;
	default:
		return 0;
	}

	return size * (param->array_count ? param->array_count : 1);
}

static void shader_set_val(gs_sparam_t *param, const void *val, size_t size)
{
	if (!param)
		return;

	size_t expected = param_value_size(param);
	if (!expected)
		return;
	if (expected != size) {
		blog(LOG_ERROR, "null: shader param '%s' expects %zu bytes, got %zu", param->name, expected, size);
		return;
	}

	if (param->type == GS_SHADER_PARAM_TEXTURE) {
		struct gs_shader_texture shader_tex;
		memcpy(&shader_tex, val, sizeof(shader_tex));
		param->texture = shader_tex.tex;
		param->srgb = shader_tex.srgb;
	} else {
		da_copy_array(param->cur_value, val, size);
	}
}

EXPORT void gs_shader_set_bool(gs_sparam_t *param, bool val)
{
	COUNT(gs_shader_set_bool);
	int b_val = (int)val;
	shader_set_val(param, &b_val, sizeof(b_val));
}

EXPORT void gs_shader_set_float(gs_sparam_t *param, float val)
{
	COUNT(gs_shader_set_float);
	shader_set_val(param, &val, sizeof(val));
}

EXPORT void gs_shader_set_int(gs_sparam_t *param, int val)
{
	COUNT(gs_shader_set_int);
	shader_set_val(param, &val, sizeof(val));
}

EXPORT void gs_shader_set_matrix3(gs_sparam_t *param, const struct matrix3 *val)
{
	COUNT(gs_shader_set_matrix3);
	struct matrix4 mat;
	matrix4_from_matrix3(&mat, val);
	shader_set_val(param, &mat, sizeof(mat));
}

EXPORT void gs_shader_set_matrix4(gs_sparam_t *param, const struct matrix4 *val)
{
	COUNT(gs_shader_set_matrix4);
	shader_set_val(param, val, sizeof(*val));
}

EXPORT void gs_shader_set_vec2(gs_sparam_t *param, const struct vec2 *val)
{
	COUNT(gs_shader_set_vec2);
	shader_set_val(param, val, sizeof(*val));
}

EXPORT void gs_shader_set_vec3(gs_sparam_t *param, const struct vec3 *val)
{
	COUNT(gs_shader_set_vec3);
	/* struct vec3 is padded to 16 bytes; only the three floats are a value */
	shader_set_val(param, val, sizeof(float) * 3);
}

EXPORT void gs_shader_set_vec4(gs_sparam_t *param, const struct vec4 *val)
{
	COUNT(gs_shader_set_vec4);
	shader_set_val(param, val, sizeof(*val));
}

EXPORT void gs_shader_set_texture(gs_sparam_t *param, gs_texture_t *val)
{
	COUNT(gs_shader_set_texture);
	if (param) {
		param->texture = val;
		param->srgb = false;
	}
}

EXPORT void gs_shader_set_val(gs_sparam_t *param, const void *val, size_t size)
{
	COUNT(gs_shader_set_val);
	shader_set_val(param, val, size);
}

EXPORT void gs_shader_set_default(gs_sparam_t *param)
{
	COUNT(gs_shader_set_default);
	if (param && param->def_value.num)
		shader_set_val(param, param->def_value.array, param->def_value.num);
}

EXPORT void gs_shader_set_next_sampler(gs_sparam_t *param, gs_samplerstate_t *sampler)
{
	COUNT(gs_shader_set_next_sampler);
	if (param)
		param->next_sampler = sampler;
}

/* ------------------------------------------------------------------------- */
/* Pipeline state                                                            */

EXPORT void device_load_vertexbuffer(gs_device_t *device, gs_vertbuffer_t *vertbuffer)
{
	COUNT(device_load_vertexbuffer);
	device->cur_vertex_buffer = vertbuffer;
}

EXPORT void device_load_indexbuffer(gs_device_t *device, gs_indexbuffer_t *indexbuffer)
{
	COUNT(device_load_indexbuffer);
	device->cur_index_buffer = indexbuffer;
}

EXPORT void device_load_texture(gs_device_t *device, gs_texture_t *tex, int unit)
{
	COUNT(device_load_texture);
	if (unit >= 0 && unit < GS_MAX_TEXTURES)
		device->cur_textures[unit] = tex;
}

EXPORT void device_load_samplerstate(gs_device_t *device, gs_samplerstate_t *samplerstate, int unit)
{
	COUNT(device_load_samplerstate);
	if (unit >= 0 && unit < GS_MAX_TEXTURES)
		device->cur_samplers[unit] = samplerstate;
}

EXPORT void device_load_vertexshader(gs_device_t *device, gs_shader_t *vertshader)
{
	COUNT(device_load_vertexshader);
	if (vertshader && vertshader->type != GS_SHADER_VERTEX) {
		blog(LOG_ERROR, "null: device_load_vertexshader: not a vertex shader");
		return;
	}
	device->cur_vertex_shader = vertshader;
}

EXPORT void device_load_pixelshader(gs_device_t *device, gs_shader_t *pixelshader)
{
	COUNT(device_load_pixelshader);
	if (pixelshader && pixelshader->type != GS_SHADER_PIXEL) {
		blog(LOG_ERROR, "null: device_load_pixelshader: not a pixel shader");
		return;
	}
	device->cur_pixel_shader = pixelshader;
}

EXPORT void device_load_default_samplerstate(gs_device_t *device, bool b_3d, int unit)
{
	UNUSED_PARAMETER(b_3d);
	COUNT(device_load_default_samplerstate);
	if (unit >= 0 && unit < GS_MAX_TEXTURES)
		device->cur_samplers[unit] = NULL;
}

EXPORT gs_shader_t *device_get_vertex_shader(const gs_device_t *device)
{
	COUNT(device_get_vertex_shader);
	return device->cur_vertex_shader;
}

EXPORT gs_shader_t *device_get_pixel_shader(const gs_device_t *device)
{
	COUNT(device_get_pixel_shader);
	return device->cur_pixel_shader;
}

EXPORT gs_texture_t *device_get_render_target(const gs_device_t *device)
{
	COUNT(device_get_render_target);
	return device->cur_render_target;
}

EXPORT gs_zstencil_t *device_get_zstencil_target(const gs_device_t *device)
{
	COUNT(device_get_zstencil_target);
	return device->cur_zstencil;
}

static void set_render_target(gs_device_t *device, gs_texture_t *tex, int side, gs_zstencil_t *zstencil,
			      enum gs_color_space space)
{
	if (tex && !(tex->flags & GS_RENDER_TARGET)) {
		blog(LOG_ERROR, "null: texture is not a render target");
		return;
	}

	device->cur_render_target = tex;
	device->cur_render_side = side;
	device->cur_zstencil = zstencil;
	device->cur_color_space = space;
}

EXPORT void device_set_render_target(gs_device_t *device, gs_texture_t *tex, gs_zstencil_t *zstencil)
{
	COUNT(device_set_render_target);
	set_render_target(device, tex, 0, zstencil, GS_CS_SRGB);
}

EXPORT void device_set_render_target_with_color_space(gs_device_t *device, gs_texture_t *tex, gs_zstencil_t *zstencil,
						      enum gs_color_space space)
{
	COUNT(device_set_render_target_with_color_space);
	set_render_target(device, tex, 0, zstencil, space);
}

EXPORT void device_set_cube_render_target(gs_device_t *device, gs_texture_t *cubetex, int side, gs_zstencil_t *zstencil)
{
	COUNT(device_set_cube_render_target);
	set_render_target(device, cubetex, side, zstencil, GS_CS_SRGB);
}

EXPORT void device_enable_framebuffer_srgb(gs_device_t *device, bool enable)
{
	COUNT(device_enable_framebuffer_srgb);
	device->srgb = enable;
}

EXPORT bool device_framebuffer_srgb_enabled(gs_device_t *device)
{
	COUNT(device_framebuffer_srgb_enabled);
	return device->srgb;
}

/* ------------------------------------------------------------------------- */
/* Copies and readback                                                       */

static bool same_desc(const gs_texture_t *a, uint32_t width, uint32_t height, enum gs_color_format format)
{
	return a->width == width && a->height == height && a->format == format;
}

EXPORT void device_copy_texture_region(gs_device_t *device, gs_texture_t *dst, uint32_t dst_x, uint32_t dst_y,
				       gs_texture_t *src, uint32_t src_x, uint32_t src_y, uint32_t src_w,
				       uint32_t src_h)
{
	UNUSED_PARAMETER(device);
	COUNT(device_copy_texture_region);

	if (!dst || !src || dst->type != GS_TEXTURE_2D || src->type != GS_TEXTURE_2D || dst->format != src->format) {
		blog(LOG_ERROR, "null: device_copy_texture_region: incompatible textures");
		return;
	}

	/* 0 means "to the edge of the source", as in the real backends */
	uint32_t w = src_w ? src_w : src->width - src_x;
	uint32_t h = src_h ? src_h : src->height - src_y;
	if (src_x + w > src->width || src_y + h > src->height || dst_x + w > dst->width || dst_y + h > dst->height) {
		blog(LOG_ERROR, "null: device_copy_texture_region: region out of bounds");
		return;
	}

	uint32_t bytes_pp = gs_get_format_bpp(src->format) / 8;
	for (uint32_t y = 0; y < h; y++) {
		memmove(dst->data + (size_t)(dst_y + y) * dst->linesize + (size_t)dst_x * bytes_pp,
			src->data + (size_t)(src_y + y) * src->linesize + (size_t)src_x * bytes_pp,
			(size_t)w * bytes_pp);
	}
}

EXPORT void device_copy_texture(gs_device_t *device, gs_texture_t *dst, gs_texture_t *src)
{
	UNUSED_PARAMETER(device);
	COUNT(device_copy_texture);

	if (!dst || !src || dst->type != src->type || !same_desc(dst, src->width, src->height, src->format) ||
	    dst->depth != src->depth) {
		blog(LOG_ERROR, "null: device_copy_texture: incompatible textures");
		return;
	}

	size_t faces = src->type == GS_TEXTURE_CUBE ? 6 : src->depth;
	memcpy(dst->data, src->data, src->face_size * faces);
}

EXPORT void device_stage_texture(gs_device_t *device, gs_stagesurf_t *dst, gs_texture_t *src)
{
	UNUSED_PARAMETER(device);
	COUNT(device_stage_texture);

	if (!dst || !src || src->type != GS_TEXTURE_2D || !same_desc(src, dst->width, dst->height, dst->format)) {
		blog(LOG_ERROR, "null: device_stage_texture: incompatible source and destination");
		return;
	}

	memcpy(dst->data, src->data, (size_t)dst->linesize * dst->height);
}

/* ------------------------------------------------------------------------- */
/* Frames and drawing                                                        */

EXPORT void device_begin_frame(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	COUNT(device_begin_frame);
}

EXPORT void device_begin_scene(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	COUNT(device_begin_scene);
}

EXPORT void device_draw(gs_device_t *device, enum gs_draw_mode draw_mode, uint32_t start_vert, uint32_t num_verts)
{
	UNUSED_PARAMETER(draw_mode);
	UNUSED_PARAMETER(start_vert);
	UNUSED_PARAMETER(num_verts);
	COUNT(device_draw);

	if (!device->cur_vertex_shader || !device->cur_pixel_shader)
		blog(LOG_ERROR, "null: device_draw: no shader loaded");
}

EXPORT void device_end_scene(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	COUNT(device_end_scene);
}

EXPORT void device_load_swapchain(gs_device_t *device, gs_swapchain_t *swapchain)
{
	COUNT(device_load_swapchain);
	device->cur_swap = swapchain;
}

EXPORT void gs_swapchain_destroy(gs_swapchain_t *swapchain)
{
	COUNT(gs_swapchain_destroy);
	if (!swapchain)
		return;

	if (swapchain->device->cur_swap == swapchain)
		swapchain->device->cur_swap = NULL;
	obj_free(swapchain);
}

static inline uint8_t unorm8(float v)
{
	v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
	return (uint8_t)(v * 255.0f + 0.5f);
}

/* Fills 8-bit RGBA/BGRA render targets so cleared contents read back as expected; other formats are left as-is. */
EXPORT void device_clear(gs_device_t *device, uint32_t clear_flags, const struct vec4 *color, float depth,
			 uint8_t stencil)
{
	UNUSED_PARAMETER(depth);
	UNUSED_PARAMETER(stencil);
	COUNT(device_clear);

	gs_texture_t *rt = device->cur_render_target;
	if (!(clear_flags & GS_CLEAR_COLOR) || !rt || !color)
		return;

	uint8_t px[4];
	switch (rt->format) {
	case GS_RGBA:
	case GS_RGBA_UNORM:
		px[0] = unorm8(color->x);
		px[1] = unorm8(color->y);
		px[2] = unorm8(color->z);
		px[3] = unorm8(color->w);
		break;
	case GS_BGRA:
	case GS_BGRA_UNORM:
	case GS_BGRX:
	case GS_BGRX_UNORM:
		px[0] = unorm8(color->z);
		px[1] = unorm8(color->y);
		px[2] = unorm8(color->x);
		px[3] = unorm8(color->w);
		break;
	default:
		return;
	}

	uint8_t *face = rt->data + (rt->type == GS_TEXTURE_CUBE ? rt->face_size * device->cur_render_side : 0);
	for (size_t i = 0; i < rt->face_size; i += 4)
		memcpy(face + i, px, 4);
}

EXPORT bool device_is_present_ready(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	COUNT(device_is_present_ready);
	return true;
}

EXPORT void device_present(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	COUNT(device_present);
}

EXPORT void device_flush(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	COUNT(device_flush);
}

EXPORT void device_set_cull_mode(gs_device_t *device, enum gs_cull_mode mode)
{
	COUNT(device_set_cull_mode);
	device->cull_mode = mode;
}

EXPORT enum gs_cull_mode device_get_cull_mode(const gs_device_t *device)
{
	COUNT(device_get_cull_mode);
	return device->cull_mode;
}

/* Blend, depth and stencil state is tracked by libobs itself (graphics->cur_blend_state etc.) */

EXPORT void device_enable_blending(gs_device_t *device, bool enable)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(enable);
	COUNT(device_enable_blending);
}

EXPORT void device_enable_depth_test(gs_device_t *device, bool enable)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(enable);
	COUNT(device_enable_depth_test);
}

EXPORT void device_enable_stencil_test(gs_device_t *device, bool enable)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(enable);
	COUNT(device_enable_stencil_test);
}

EXPORT void device_enable_stencil_write(gs_device_t *device, bool enable)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(enable);
	COUNT(device_enable_stencil_write);
}

EXPORT void device_enable_color(gs_device_t *device, bool red, bool green, bool blue, bool alpha)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(red);
	UNUSED_PARAMETER(green);
	UNUSED_PARAMETER(blue);
	UNUSED_PARAMETER(alpha);
	COUNT(device_enable_color);
}

EXPORT void device_blend_function(gs_device_t *device, enum gs_blend_type src, enum gs_blend_type dest)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(src);
	UNUSED_PARAMETER(dest);
	COUNT(device_blend_function);
}

EXPORT void device_blend_function_separate(gs_device_t *device, enum gs_blend_type src_c, enum gs_blend_type dest_c,
					   enum gs_blend_type src_a, enum gs_blend_type dest_a)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(src_c);
	UNUSED_PARAMETER(dest_c);
	UNUSED_PARAMETER(src_a);
	UNUSED_PARAMETER(dest_a);
	COUNT(device_blend_function_separate);
}

EXPORT void device_blend_op(gs_device_t *device, enum gs_blend_op_type op)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(op);
	COUNT(device_blend_op);
}

EXPORT void device_depth_function(gs_device_t *device, enum gs_depth_test test)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(test);
	COUNT(device_depth_function);
}

EXPORT void device_stencil_function(gs_device_t *device, enum gs_stencil_side side, enum gs_depth_test test)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(side);
	UNUSED_PARAMETER(test);
	COUNT(device_stencil_function);
}

EXPORT void device_stencil_op(gs_device_t *device, enum gs_stencil_side side, enum gs_stencil_op_type fail,
			      enum gs_stencil_op_type zfail, enum gs_stencil_op_type zpass)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(side);
	UNUSED_PARAMETER(fail);
	UNUSED_PARAMETER(zfail);
	UNUSED_PARAMETER(zpass);
	COUNT(device_stencil_op);
}

EXPORT void device_set_viewport(gs_device_t *device, int x, int y, int width, int height)
{
	COUNT(device_set_viewport);
	device->viewport.x = x;
	device->viewport.y = y;
	device->viewport.cx = width;
	device->viewport.cy = height;
}

EXPORT void device_get_viewport(const gs_device_t *device, struct gs_rect *rect)
{
	COUNT(device_get_viewport);
	*rect = device->viewport;
}

EXPORT void device_set_scissor_rect(gs_device_t *device, const struct gs_rect *rect)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(rect);
	COUNT(device_set_scissor_rect);
}

static void set_projection(gs_device_t *device, bool ortho, float left, float right, float top, float bottom,
			   float znear, float zfar)
{
	struct projection *proj = &device->projection;
	proj->ortho = ortho;
	proj->left = left;
	proj->right = right;
	proj->top = top;
	proj->bottom = bottom;
	proj->znear = znear;
	proj->zfar = zfar;
}

EXPORT void device_ortho(gs_device_t *device, float left, float right, float top, float bottom, float znear, float zfar)
{
	COUNT(device_ortho);
	set_projection(device, true, left, right, top, bottom, znear, zfar);
}

EXPORT void device_frustum(gs_device_t *device, float left, float right, float top, float bottom, float znear,
			   float zfar)
{
	COUNT(device_frustum);
	set_projection(device, false, left, right, top, bottom, znear, zfar);
}

EXPORT void device_projection_push(gs_device_t *device)
{
	COUNT(device_projection_push);
	da_push_back(device->projection_stack, &device->projection);
}

EXPORT void device_projection_pop(gs_device_t *device)
{
	COUNT(device_projection_pop);
	if (!device->projection_stack.num)
		return;

	device->projection = device->projection_stack.array[device->projection_stack.num - 1];
	da_pop_back(device->projection_stack);
}

EXPORT bool device_is_monitor_hdr(gs_device_t *device, void *monitor)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(monitor);
	COUNT(device_is_monitor_hdr);
	return false;
}

EXPORT void device_debug_marker_begin(gs_device_t *device, const char *markername, const float color[4])
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(markername);
	UNUSED_PARAMETER(color);
	COUNT(device_debug_marker_begin);
}

EXPORT void device_debug_marker_end(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	COUNT(device_debug_marker_end);
}

/* ------------------------------------------------------------------------- */
/* Windows-specific exports: none of these features exist without a GPU      */

#ifdef _WIN32
EXPORT bool device_gdi_texture_available(void)
{
	COUNT(device_gdi_texture_available);
	return false;
}

EXPORT bool device_shared_texture_available(void)
{
	COUNT(device_shared_texture_available);
	return false;
}

EXPORT bool device_get_duplicator_monitor_info(gs_device_t *device, int monitor_idx,
					       struct gs_monitor_info *monitor_info)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(monitor_idx);
	UNUSED_PARAMETER(monitor_info);
	COUNT(device_get_duplicator_monitor_info);
	return false;
}

EXPORT int device_duplicator_get_monitor_index(gs_device_t *device, void *monitor)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(monitor);
	COUNT(device_duplicator_get_monitor_index);
	return -1;
}

EXPORT gs_duplicator_t *device_duplicator_create(gs_device_t *device, int monitor_idx)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(monitor_idx);
	COUNT(device_duplicator_create);
	FAIL_IF_ASKED(device_duplicator_create, NULL);
	return NULL;
}

EXPORT void gs_duplicator_destroy(gs_duplicator_t *duplicator)
{
	UNUSED_PARAMETER(duplicator);
	COUNT(gs_duplicator_destroy);
}

EXPORT bool gs_duplicator_update_frame(gs_duplicator_t *duplicator)
{
	UNUSED_PARAMETER(duplicator);
	COUNT(gs_duplicator_update_frame);
	return false;
}

EXPORT gs_texture_t *gs_duplicator_get_texture(gs_duplicator_t *duplicator)
{
	UNUSED_PARAMETER(duplicator);
	COUNT(gs_duplicator_get_texture);
	return NULL;
}

EXPORT enum gs_color_space gs_duplicator_get_color_space(gs_duplicator_t *duplicator)
{
	UNUSED_PARAMETER(duplicator);
	COUNT(gs_duplicator_get_color_space);
	return GS_CS_SRGB;
}

EXPORT float gs_duplicator_get_sdr_white_level(gs_duplicator_t *duplicator)
{
	UNUSED_PARAMETER(duplicator);
	COUNT(gs_duplicator_get_sdr_white_level);
	return 80.0f;
}

EXPORT bool device_can_adapter_fast_clear(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	COUNT(device_can_adapter_fast_clear);
	return false;
}

EXPORT gs_texture_t *device_texture_create_gdi(gs_device_t *device, uint32_t width, uint32_t height)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	COUNT(device_texture_create_gdi);
	FAIL_IF_ASKED(device_texture_create_gdi, NULL);
	return NULL;
}

EXPORT void *gs_texture_get_dc(gs_texture_t *gdi_tex)
{
	UNUSED_PARAMETER(gdi_tex);
	COUNT(gs_texture_get_dc);
	return NULL;
}

EXPORT void gs_texture_release_dc(gs_texture_t *gdi_tex)
{
	UNUSED_PARAMETER(gdi_tex);
	COUNT(gs_texture_release_dc);
}

EXPORT gs_texture_t *device_texture_open_shared(gs_device_t *device, uint32_t handle)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(handle);
	COUNT(device_texture_open_shared);
	return NULL;
}

EXPORT gs_texture_t *device_texture_open_nt_shared(gs_device_t *device, uint32_t handle)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(handle);
	COUNT(device_texture_open_nt_shared);
	return NULL;
}

EXPORT uint32_t device_texture_get_shared_handle(gs_texture_t *tex)
{
	UNUSED_PARAMETER(tex);
	COUNT(device_texture_get_shared_handle);
	return GS_INVALID_HANDLE;
}

EXPORT gs_texture_t *device_texture_wrap_obj(gs_device_t *device, void *obj)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(obj);
	COUNT(device_texture_wrap_obj);
	return NULL;
}

EXPORT int device_texture_acquire_sync(gs_texture_t *tex, uint64_t key, uint32_t ms)
{
	UNUSED_PARAMETER(tex);
	UNUSED_PARAMETER(key);
	UNUSED_PARAMETER(ms);
	COUNT(device_texture_acquire_sync);
	return -1;
}

EXPORT int device_texture_release_sync(gs_texture_t *tex, uint64_t key)
{
	UNUSED_PARAMETER(tex);
	UNUSED_PARAMETER(key);
	COUNT(device_texture_release_sync);
	return -1;
}

EXPORT void device_register_loss_callbacks(gs_device_t *device, const struct gs_device_loss *callbacks)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(callbacks);
	COUNT(device_register_loss_callbacks);
}

EXPORT void device_unregister_loss_callbacks(gs_device_t *device, void *data)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(data);
	COUNT(device_unregister_loss_callbacks);
}
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__DragonFly__)
/* Linux-specific exports: no dmabuf, pixmap or sync object support without a GPU */

EXPORT gs_texture_t *device_texture_create_from_dmabuf(gs_device_t *device, unsigned int width, unsigned int height,
							uint32_t drm_format, enum gs_color_format color_format,
							uint32_t n_planes, const int *fds, const uint32_t *strides,
							const uint32_t *offsets, const uint64_t *modifiers)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(drm_format);
	UNUSED_PARAMETER(color_format);
	UNUSED_PARAMETER(n_planes);
	UNUSED_PARAMETER(fds);
	UNUSED_PARAMETER(strides);
	UNUSED_PARAMETER(offsets);
	UNUSED_PARAMETER(modifiers);
	COUNT(device_texture_create_from_dmabuf);
	return NULL;
}

EXPORT bool device_query_dmabuf_capabilities(gs_device_t *device, enum gs_dmabuf_flags *dmabuf_flags,
					     uint32_t **drm_formats, size_t *n_formats)
{
	UNUSED_PARAMETER(device);
	COUNT(device_query_dmabuf_capabilities);
	if (dmabuf_flags)
		*dmabuf_flags = GS_DMABUF_FLAG_NONE;
	if (drm_formats)
		*drm_formats = NULL;
	if (n_formats)
		*n_formats = 0;
	return false;
}

EXPORT bool device_query_dmabuf_modifiers_for_format(gs_device_t *device, uint32_t drm_format, uint64_t **modifiers,
						     size_t *n_modifiers)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(drm_format);
	COUNT(device_query_dmabuf_modifiers_for_format);
	if (modifiers)
		*modifiers = NULL;
	if (n_modifiers)
		*n_modifiers = 0;
	return false;
}

EXPORT gs_texture_t *device_texture_create_from_pixmap(gs_device_t *device, uint32_t width, uint32_t height,
							enum gs_color_format color_format, uint32_t target,
							void *pixmap)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	UNUSED_PARAMETER(color_format);
	UNUSED_PARAMETER(target);
	UNUSED_PARAMETER(pixmap);
	COUNT(device_texture_create_from_pixmap);
	return NULL;
}

EXPORT bool device_query_sync_capabilities(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	COUNT(device_query_sync_capabilities);
	return false;
}

EXPORT gs_sync_t *device_sync_create(gs_device_t *device)
{
	UNUSED_PARAMETER(device);
	COUNT(device_sync_create);
	return NULL;
}

EXPORT gs_sync_t *device_sync_create_from_syncobj_timeline_point(gs_device_t *device, int syncobj_fd,
								 uint64_t timeline_point)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(syncobj_fd);
	UNUSED_PARAMETER(timeline_point);
	COUNT(device_sync_create_from_syncobj_timeline_point);
	return NULL;
}

EXPORT void device_sync_destroy(gs_device_t *device, gs_sync_t *sync)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(sync);
	COUNT(device_sync_destroy);
}

EXPORT bool device_sync_export_syncobj_timeline_point(gs_device_t *device, gs_sync_t *sync, int syncobj_fd,
						      uint64_t timeline_point)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(sync);
	UNUSED_PARAMETER(syncobj_fd);
	UNUSED_PARAMETER(timeline_point);
	COUNT(device_sync_export_syncobj_timeline_point);
	return false;
}

EXPORT bool device_sync_signal_syncobj_timeline_point(gs_device_t *device, int syncobj_fd, uint64_t timeline_point)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(syncobj_fd);
	UNUSED_PARAMETER(timeline_point);
	COUNT(device_sync_signal_syncobj_timeline_point);
	return false;
}

EXPORT bool device_sync_wait(gs_device_t *device, gs_sync_t *sync)
{
	UNUSED_PARAMETER(device);
	UNUSED_PARAMETER(sync);
	COUNT(device_sync_wait);
	return false;
}
#endif
