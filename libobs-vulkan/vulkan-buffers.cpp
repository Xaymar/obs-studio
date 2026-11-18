// Vertex and index buffers, sampler states, and the device_load_* calls that bind them.
#include "vulkan-subsystem.hpp"

#include <graphics/vec3.h>
#include <util/base.h>
#include <util/bmem.h>

#include <algorithm>

// shortcut: static buffers are host-visible (VMA may pick device-local ReBAR memory); OBS meshes are tiny. Upgrade to
// a staging copy into device-local memory if large static meshes appear.
static bool create_static(gs_device *d, const void *data, VkDeviceSize size, VkBufferUsageFlags usage, uint32_t stride,
			  vk_buffer &b)
{
	void *mapped;
	if (!vk_create_host_buffer(d, size, usage, VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT, b.buffer,
				   b.alloc, mapped)) {
		return false;
	}
	// Written by the host before any submission that reads it: visible without a barrier.
	memcpy(mapped, data, (size_t)size);
	vmaFlushAllocation(d->vma, b.alloc, 0, VK_WHOLE_SIZE);
	b.stride = stride;
	return true;
}

static void destroy_buffer(gs_device *d, vk_buffer &b)
{
	if (b.alloc) {
		d->frames.defer_destroy(
			[vma = d->vma, buffer = b.buffer, alloc = b.alloc] { vmaDestroyBuffer(vma, buffer, alloc); });
	}
	b = {};
}

struct vb_array {
	const void *data;
	uint32_t stride;
};

// Attribute arrays in gs_vertex_buffer::attrs order.
static std::vector<vb_array> vb_arrays(const gs_vb_data *v)
{
	std::vector<vb_array> a = {{v->points, sizeof(vec3)},
				   {v->normals, sizeof(vec3)},
				   {v->tangents, sizeof(vec3)},
				   {v->colors, sizeof(uint32_t)}};
	for (size_t i = 0; i < v->num_tex; ++i) {
		a.push_back({v->tvarray[i].array, (uint32_t)(v->tvarray[i].width * sizeof(float))});
	}
	return a;
}

bool vk_upload_dynamic(gs_device *d, gs_vertex_buffer *vb)
{
	if (!vb->dynamic || (!vb->dirty && vb->upload_value == d->graphics.next_value)) {
		return true;
	}
	std::vector<vb_array> a = vb_arrays(vb->data);
	for (size_t i = 0; i < a.size() && i < vb->attrs.size(); ++i) {
		if (a[i].data &&
		    !vk_arena_upload(d, a[i].data, (VkDeviceSize)a[i].stride * vb->data->num, 16, vb->attrs[i])) {
			return false;
		}
	}
	vb->dirty = false;
	vb->upload_value = d->graphics.next_value;
	return true;
}

bool vk_upload_dynamic(gs_device *d, gs_index_buffer *ib)
{
	if (!ib->dynamic || (!ib->dirty && ib->upload_value == d->graphics.next_value)) {
		return true;
	}
	const size_t size = ib->type == GS_UNSIGNED_SHORT ? 2 : 4;
	if (!vk_arena_upload(d, ib->indices, ib->num * size, 4, ib->buf)) {
		return false;
	}
	ib->dirty = false;
	ib->upload_value = d->graphics.next_value;
	return true;
}

static VkFilter filter(bool linear)
{
	return linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}

static VkSamplerAddressMode address(gs_device *d, enum gs_address_mode mode)
{
	switch (mode) {
	case GS_ADDRESS_WRAP:
		return VK_SAMPLER_ADDRESS_MODE_REPEAT;
	case GS_ADDRESS_MIRROR:
		return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	case GS_ADDRESS_BORDER:
		return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	case GS_ADDRESS_MIRRORONCE:
		return d->mirror_clamp ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE
				       : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
	default:
		return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	}
}

VkSampler vk_create_sampler(gs_device *d, const gs_sampler_info &info)
{
	// min, mag, mip linear; as D3D11's ConvertGSFilter.
	bool min = false, mag = false, mip = false;
	switch (info.filter) {
	case GS_FILTER_LINEAR:
	case GS_FILTER_ANISOTROPIC:
		min = mag = mip = true;
		break;
	case GS_FILTER_MIN_MAG_POINT_MIP_LINEAR:
		mip = true;
		break;
	case GS_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT:
		mag = true;
		break;
	case GS_FILTER_MIN_POINT_MAG_MIP_LINEAR:
		mag = mip = true;
		break;
	case GS_FILTER_MIN_LINEAR_MAG_MIP_POINT:
		min = true;
		break;
	case GS_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR:
		min = mip = true;
		break;
	case GS_FILTER_MIN_MAG_LINEAR_MIP_POINT:
		min = mag = true;
		break;
	default:
		break;
	}

	VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	si.magFilter = filter(mag);
	si.minFilter = filter(min);
	si.mipmapMode = mip ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
	si.addressModeU = address(d, info.address_u);
	si.addressModeV = address(d, info.address_v);
	si.addressModeW = address(d, info.address_w);
	si.anisotropyEnable = info.filter == GS_FILTER_ANISOTROPIC && d->anisotropy && info.max_anisotropy > 1;
	si.maxAnisotropy = std::clamp((float)info.max_anisotropy, 1.0f, d->limits.maxSamplerAnisotropy);
	si.maxLod = VK_LOD_CLAMP_NONE;
	// shortcut: Vulkan core has three border colours; others need VK_EXT_custom_border_color. Nearest one is used.
	const uint32_t alpha = info.border_color >> 24, rgb = info.border_color & 0xFFFFFF;
	si.borderColor = alpha == 0        ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
			 : rgb == 0xFFFFFF ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
					   : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
	VkSampler s = VK_NULL_HANDLE;
	if (vkCreateSampler(d->dev, &si, nullptr, &s) != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] vkCreateSampler failed");
	}
	return s;
}

extern "C" {

EXPORT gs_vertbuffer_t *device_vertexbuffer_create(gs_device_t *device, struct gs_vb_data *data, uint32_t flags)
{
	if (!data || !data->num || !data->points) {
		blog(LOG_ERROR, "[libobs-vulkan] device_vertexbuffer_create: no vertices or no points");
		return nullptr;
	}
	for (size_t i = 0; i < data->num_tex; ++i) {
		if (!data->tvarray[i].array || data->tvarray[i].width < 1 || data->tvarray[i].width > 4) {
			blog(LOG_ERROR, "[libobs-vulkan] device_vertexbuffer_create: bad texture vertex array %zu", i);
			return nullptr;
		}
	}
	auto vb = new gs_vertex_buffer{device, data, (flags & GS_DYNAMIC) != 0};
	for (const vb_array &a : vb_arrays(data)) {
		vk_buffer b;
		b.stride = a.stride;
		if (a.data && !vb->dynamic &&
		    !create_static(device, a.data, (VkDeviceSize)a.stride * data->num,
				   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, a.stride, b)) {
			blog(LOG_ERROR, "[libobs-vulkan] device_vertexbuffer_create: buffer creation failed");
			gs_vertexbuffer_destroy(vb);
			return nullptr;
		}
		vb->attrs.push_back(b);
	}
	return vb;
}

EXPORT void gs_vertexbuffer_destroy(gs_vertbuffer_t *vertbuffer)
{
	if (!vertbuffer) {
		return;
	}
	gs_device *d = vertbuffer->device;
	if (d->cur_vb == vertbuffer) {
		d->cur_vb = nullptr;
	}
	for (vk_buffer &b : vertbuffer->attrs) {
		destroy_buffer(d, b);
	}
	gs_vbdata_destroy(vertbuffer->data);
	delete vertbuffer;
}

// The next draw copies the data into its frame's arena; nothing waits.
EXPORT void gs_vertexbuffer_flush(gs_vertbuffer_t *vertbuffer)
{
	if (!vertbuffer->dynamic) {
		blog(LOG_ERROR, "[libobs-vulkan] gs_vertexbuffer_flush: buffer is not dynamic");
		return;
	}
	vertbuffer->dirty = true;
}

// shortcut: copies `data` into the buffer's own arrays (gs_vertexbuffer_get_data then returns it); the backend keeps
// a CPU copy to re-upload each frame. Same vertex count and arrays as at creation.
EXPORT void gs_vertexbuffer_flush_direct(gs_vertbuffer_t *vertbuffer, const struct gs_vb_data *data)
{
	if (!vertbuffer->dynamic) {
		blog(LOG_ERROR, "[libobs-vulkan] gs_vertexbuffer_flush_direct: buffer is not dynamic");
		return;
	}
	std::vector<vb_array> dst = vb_arrays(vertbuffer->data), src = vb_arrays(data);
	const size_t num = std::min(vertbuffer->data->num, data->num);
	for (size_t i = 0; i < dst.size() && i < src.size(); ++i) {
		if (dst[i].data && src[i].data && dst[i].data != src[i].data && dst[i].stride == src[i].stride) {
			memcpy((void *)dst[i].data, src[i].data, num * dst[i].stride);
		}
	}
	vertbuffer->dirty = true;
}

EXPORT struct gs_vb_data *gs_vertexbuffer_get_data(const gs_vertbuffer_t *vertbuffer)
{
	return vertbuffer->data;
}

EXPORT gs_indexbuffer_t *device_indexbuffer_create(gs_device_t *device, enum gs_index_type type, void *indices,
						   size_t num, uint32_t flags)
{
	if (!indices || !num) {
		blog(LOG_ERROR, "[libobs-vulkan] device_indexbuffer_create: no indices");
		return nullptr;
	}
	auto ib = new gs_index_buffer{device, type, indices, num, (flags & GS_DYNAMIC) != 0};
	const size_t size = type == GS_UNSIGNED_SHORT ? 2 : 4;
	if (!ib->dynamic &&
	    !create_static(device, indices, num * size, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, (uint32_t)size, ib->buf)) {
		blog(LOG_ERROR, "[libobs-vulkan] device_indexbuffer_create: buffer creation failed");
		gs_indexbuffer_destroy(ib); // frees indices, as D3D11 does on failure
		return nullptr;
	}
	return ib;
}

EXPORT void gs_indexbuffer_destroy(gs_indexbuffer_t *indexbuffer)
{
	if (!indexbuffer) {
		return;
	}
	gs_device *d = indexbuffer->device;
	if (d->cur_ib == indexbuffer) {
		d->cur_ib = nullptr;
	}
	destroy_buffer(d, indexbuffer->buf);
	bfree(indexbuffer->indices);
	delete indexbuffer;
}

EXPORT void gs_indexbuffer_flush(gs_indexbuffer_t *indexbuffer)
{
	if (!indexbuffer->dynamic) {
		blog(LOG_ERROR, "[libobs-vulkan] gs_indexbuffer_flush: buffer is not dynamic");
		return;
	}
	indexbuffer->dirty = true;
}

// shortcut: as gs_vertexbuffer_flush_direct, copies into the buffer's own indices.
EXPORT void gs_indexbuffer_flush_direct(gs_indexbuffer_t *indexbuffer, const void *data)
{
	if (!indexbuffer->dynamic) {
		blog(LOG_ERROR, "[libobs-vulkan] gs_indexbuffer_flush_direct: buffer is not dynamic");
		return;
	}
	if (data != indexbuffer->indices) {
		memcpy(indexbuffer->indices, data, indexbuffer->num * (indexbuffer->type == GS_UNSIGNED_SHORT ? 2 : 4));
	}
	indexbuffer->dirty = true;
}

EXPORT void *gs_indexbuffer_get_data(const gs_indexbuffer_t *indexbuffer)
{
	return indexbuffer->indices;
}

EXPORT size_t gs_indexbuffer_get_num_indices(const gs_indexbuffer_t *indexbuffer)
{
	return indexbuffer->num;
}

EXPORT enum gs_index_type gs_indexbuffer_get_type(const gs_indexbuffer_t *indexbuffer)
{
	return indexbuffer->type;
}

EXPORT gs_samplerstate_t *device_samplerstate_create(gs_device_t *device, const struct gs_sampler_info *info)
{
	VkSampler s = vk_create_sampler(device, *info);
	return s ? new gs_sampler_state{device, s, *info} : nullptr;
}

EXPORT void gs_samplerstate_destroy(gs_samplerstate_t *samplerstate)
{
	if (!samplerstate) {
		return;
	}
	gs_device *d = samplerstate->device;
	for (gs_sampler_state *&s : d->cur_samplers) {
		if (s == samplerstate) {
			s = nullptr;
		}
	}
	d->frames.defer_destroy([dev = d->dev, s = samplerstate->sampler] { vkDestroySampler(dev, s, nullptr); });
	delete samplerstate;
}

EXPORT void device_load_vertexbuffer(gs_device_t *device, gs_vertbuffer_t *vertbuffer)
{
	device->cur_vb = vertbuffer;
}

EXPORT void device_load_indexbuffer(gs_device_t *device, gs_indexbuffer_t *indexbuffer)
{
	device->cur_ib = indexbuffer;
}

// Pixel shader texture `unit` (the n-th texture parameter), used when the shader's parameter has no texture set.
EXPORT void device_load_texture(gs_device_t *device, gs_texture_t *tex, int unit)
{
	if (unit >= 0 && unit < GS_MAX_TEXTURES) {
		device->cur_textures[unit] = tex;
	}
}

// Pixel shader sampler for texture `unit`; overrides the shader's sampler_state until the next pixel shader load.
EXPORT void device_load_samplerstate(gs_device_t *device, gs_samplerstate_t *samplerstate, int unit)
{
	if (unit >= 0 && unit < GS_MAX_TEXTURES) {
		device->cur_samplers[unit] = samplerstate;
	}
}

// As D3D11: not implemented there either.
EXPORT void device_load_default_samplerstate(gs_device_t *device, bool b_3d, int unit) {}
}
