// 2D textures and stage surfaces.
#include "vulkan-subsystem.hpp"

#include <util/base.h>

#include <algorithm>

VkFormat vk_format(enum gs_color_format format)
{
	// GS_RGBA/GS_BGRA/GS_BGRX are the sRGB-capable variants (typeless in D3D11); stored as UNORM.
	// shortcut: no sRGB view yet (needs VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT), add with framebuffer sRGB.
	switch (format) {
	case GS_R8:
		return VK_FORMAT_R8_UNORM;
	case GS_RGBA:
	case GS_RGBA_UNORM:
		return VK_FORMAT_R8G8B8A8_UNORM;
	case GS_BGRX:
	case GS_BGRA:
	case GS_BGRX_UNORM:
	case GS_BGRA_UNORM:
		return VK_FORMAT_B8G8R8A8_UNORM; // Vulkan has no BGRX; alpha is ignored by the callers
	case GS_R10G10B10A2:
		return VK_FORMAT_A2B10G10R10_UNORM_PACK32; // same bit layout as DXGI R10G10B10A2
	case GS_RGBA16F:
		return VK_FORMAT_R16G16B16A16_SFLOAT;
	case GS_RGBA32F:
		return VK_FORMAT_R32G32B32A32_SFLOAT;
	default:
		return VK_FORMAT_UNDEFINED;
	}
}

extern "C" {

EXPORT gs_texture_t *device_texture_create(gs_device_t *device, uint32_t width, uint32_t height,
					   enum gs_color_format color_format, uint32_t levels, const uint8_t **data,
					   uint32_t flags)
{
	VkFormat fmt = vk_format(color_format);
	if (fmt == VK_FORMAT_UNDEFINED) {
		blog(LOG_ERROR, "[libobs-vulkan] device_texture_create: unsupported format %d", (int)color_format);
		return nullptr;
	}
	// shortcut: initial data and mip levels are ignored until uploads exist.
	if (data) {
		blog(LOG_WARNING, "[libobs-vulkan] device_texture_create: initial data not uploaded yet");
	}

	VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (flags & GS_RENDER_TARGET) {
		usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	}

	VkImageFormatProperties props;
	if (vkGetPhysicalDeviceImageFormatProperties(device->phys, fmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
						     usage, 0, &props) != VK_SUCCESS ||
	    width > props.maxExtent.width || height > props.maxExtent.height) {
		blog(LOG_ERROR, "[libobs-vulkan] device_texture_create: format %d unsupported for %ux%u",
		     (int)color_format, width, height);
		return nullptr;
	}

	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = fmt;
	ii.extent = {width, height, 1};
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = usage;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;

	auto tex = new gs_texture{device};
	tex->width = width;
	tex->height = height;
	tex->format = color_format;
	if (vmaCreateImage(device->vma, &ii, &ai, &tex->image, &tex->alloc, nullptr) != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] device_texture_create: vmaCreateImage failed");
		delete tex;
		return nullptr;
	}
	(void)levels;
	return tex;
}

EXPORT void gs_texture_destroy(gs_texture_t *tex)
{
	if (!tex) {
		return;
	}
	gs_device *d = tex->device;
	if (d->render_target == tex) {
		d->render_target = nullptr;
	}
	d->tracker.forget(tex->image);
	d->frames.defer_destroy(
		[vma = d->vma, image = tex->image, alloc = tex->alloc] { vmaDestroyImage(vma, image, alloc); });
	delete tex;
}

EXPORT enum gs_texture_type device_get_texture_type(const gs_texture_t *texture)
{
	return GS_TEXTURE_2D;
}

EXPORT uint32_t gs_texture_get_width(const gs_texture_t *tex)
{
	return tex->width;
}

EXPORT uint32_t gs_texture_get_height(const gs_texture_t *tex)
{
	return tex->height;
}

EXPORT enum gs_color_format gs_texture_get_color_format(const gs_texture_t *tex)
{
	return tex->format;
}

EXPORT gs_stagesurf_t *device_stagesurface_create(gs_device_t *device, uint32_t width, uint32_t height,
						  enum gs_color_format color_format)
{
	if (vk_format(color_format) == VK_FORMAT_UNDEFINED) {
		blog(LOG_ERROR, "[libobs-vulkan] device_stagesurface_create: unsupported format %d", (int)color_format);
		return nullptr;
	}

	auto s = new gs_stage_surface{device};
	s->width = width;
	s->height = height;
	s->format = color_format;
	s->linesize = width * gs_get_format_bpp(color_format) / 8;

	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = (VkDeviceSize)s->linesize * height;
	bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;

	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

	VmaAllocationInfo info;
	if (vmaCreateBuffer(device->vma, &bi, &ai, &s->buffer, &s->alloc, &info) != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] device_stagesurface_create: vmaCreateBuffer failed");
		delete s;
		return nullptr;
	}
	s->mapped = info.pMappedData;
	return s;
}

EXPORT void gs_stagesurface_destroy(gs_stagesurf_t *stagesurf)
{
	if (!stagesurf) {
		return;
	}
	gs_device *d = stagesurf->device;
	d->tracker.forget(stagesurf->buffer);
	d->frames.defer_destroy([vma = d->vma, buffer = stagesurf->buffer, alloc = stagesurf->alloc] {
		vmaDestroyBuffer(vma, buffer, alloc);
	});
	delete stagesurf;
}

EXPORT uint32_t gs_stagesurface_get_width(const gs_stagesurf_t *stagesurf)
{
	return stagesurf->width;
}

EXPORT uint32_t gs_stagesurface_get_height(const gs_stagesurf_t *stagesurf)
{
	return stagesurf->height;
}

EXPORT enum gs_color_format gs_stagesurface_get_color_format(const gs_stagesurf_t *stagesurf)
{
	return stagesurf->format;
}

// Sync point: submits the frame holding the copy if it is still being recorded, then waits for that copy only.
EXPORT bool gs_stagesurface_map(gs_stagesurf_t *stagesurf, uint8_t **data, uint32_t *linesize)
{
	gs_device *d = stagesurf->device;
	const uint64_t value = stagesurf->copy_value;
	if (value && value >= d->graphics.next_value && !vk_flush(d)) {
		return false;
	}
	if (value) {
		// Never submitted (would wait forever) or its submission failed (the copy never ran).
		const auto &failed = d->graphics.failed_values;
		if (value >= d->graphics.next_value || std::find(failed.begin(), failed.end(), value) != failed.end()) {
			return false;
		}
		VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
		wi.semaphoreCount = 1;
		wi.pSemaphores = &d->graphics.timeline;
		wi.pValues = &value;
		if (vkWaitSemaphores(d->dev, &wi, UINT64_MAX) != VK_SUCCESS) {
			return false;
		}
	}
	vmaInvalidateAllocation(d->vma, stagesurf->alloc, 0, VK_WHOLE_SIZE);
	*data = (uint8_t *)stagesurf->mapped;
	*linesize = stagesurf->linesize;
	return true;
}

// Persistently mapped, nothing to do.
EXPORT void gs_stagesurface_unmap(gs_stagesurf_t *stagesurf) {}
}
