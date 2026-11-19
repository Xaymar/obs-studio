// 2D and volume textures and stage surfaces.
#include "vulkan-subsystem.hpp"

#include <util/base.h>

#include <algorithm>
#include <string.h>
#include <vector>

VkFormat vk_format(enum gs_color_format format)
{
	// GS_RGBA/GS_BGRA/GS_BGRX are the sRGB-capable variants (typeless in D3D11): UNORM images with an extra sRGB view.
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
		return VK_FORMAT_B8G8R8A8_UNORM; // Vulkan has no BGRX; the views swizzle alpha to one
	case GS_R10G10B10A2:
		return VK_FORMAT_A2B10G10R10_UNORM_PACK32; // same bit layout as DXGI R10G10B10A2
	case GS_RGBA16F:
		return VK_FORMAT_R16G16B16A16_SFLOAT;
	case GS_RGBA32F:
		return VK_FORMAT_R32G32B32A32_SFLOAT;
	case GS_A8:
		return VK_FORMAT_R8_UNORM; // sampled views swizzle it to alpha
	case GS_R8G8:
		return VK_FORMAT_R8G8_UNORM; // NV12 chroma plane
	case GS_R16:
		return VK_FORMAT_R16_UNORM; // P010 luma plane
	case GS_RG16:
		return VK_FORMAT_R16G16_UNORM; // P010 chroma plane
	case GS_RGBA16:
		return VK_FORMAT_R16G16B16A16_UNORM;
	case GS_R16F:
		return VK_FORMAT_R16_SFLOAT;
	case GS_RG16F:
		return VK_FORMAT_R16G16_SFLOAT;
	case GS_R32F:
		return VK_FORMAT_R32_SFLOAT;
	case GS_RG32F:
		return VK_FORMAT_R32G32_SFLOAT;
	default:
		// shortcut: GS_DXT1/3/5 (BC1-3) need block-sized uploads; only DDS images use them.
		return VK_FORMAT_UNDEFINED;
	}
}

bool vk_create_host_buffer(gs_device *d, VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocationCreateFlags access,
			   VkBuffer &buffer, VmaAllocation &alloc, void *&mapped)
{
	VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bi.size = size;
	bi.usage = usage;

	VmaAllocationCreateInfo ai = {};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = access | VMA_ALLOCATION_CREATE_MAPPED_BIT;

	VmaAllocationInfo info;
	if (vmaCreateBuffer(d->vma, &bi, &ai, &buffer, &alloc, &info) != VK_SUCCESS) {
		return false;
	}
	mapped = info.pMappedData;
	return true;
}

// Sampled views cover all mips and force BGRX's alpha to one; render target views (rt) are mip 0, identity swizzle.
static VkImageView create_view(gs_device *d, const gs_texture *tex, VkFormat fmt, bool rt = false)
{
	const bool no_alpha = !rt && (tex->format == GS_BGRX || tex->format == GS_BGRX_UNORM);
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = tex->image;
	vi.viewType = tex->type == GS_TEXTURE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
	vi.format = fmt;
	vi.components.a = no_alpha ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_IDENTITY;
	if (tex->format == GS_A8 && !rt) { // as DXGI A8_UNORM: (0, 0, 0, a)
		vi.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
				 VK_COMPONENT_SWIZZLE_R};
	}
	vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, rt ? 1 : tex->levels, 0, 1};
	VkImageView view = VK_NULL_HANDLE;
	vkCreateImageView(d->dev, &vi, nullptr, &view);
	return view;
}

static uint32_t mip_size(uint32_t size, uint32_t level)
{
	return std::max(size >> level, 1u);
}

// Records the copy of data[0..n) into levels 0..n) through a staging buffer freed when the frame finishes.
static bool upload(gs_device *d, gs_texture *tex, const uint8_t *const *data, uint32_t n)
{
	const uint32_t bpp = gs_get_format_bpp(tex->format) / 8;
	std::vector<VkBufferImageCopy> regions;
	VkDeviceSize size = 0;
	for (uint32_t i = 0; i < n; ++i) {
		if (!data[i]) {
			continue;
		}
		VkBufferImageCopy r = {};
		r.bufferOffset = size;
		r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
		r.imageExtent = {mip_size(tex->width, i), mip_size(tex->height, i), mip_size(tex->depth, i)};
		regions.push_back(r);
		size += (VkDeviceSize)r.imageExtent.width * r.imageExtent.height * r.imageExtent.depth * bpp;
	}
	if (regions.empty()) {
		return true;
	}

	VkBuffer buffer;
	VmaAllocation alloc;
	void *mapped;
	if (!vk_create_host_buffer(d, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
				   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT, buffer, alloc, mapped)) {
		return false;
	}
	for (const VkBufferImageCopy &r : regions) {
		memcpy((uint8_t *)mapped + r.bufferOffset, data[r.imageSubresource.mipLevel],
		       (size_t)r.imageExtent.width * r.imageExtent.height * r.imageExtent.depth * bpp);
	}
	vmaFlushAllocation(d->vma, alloc, 0, VK_WHOLE_SIZE);

	// Fresh buffer written by the host before submission: only the image needs a barrier.
	VkCommandBuffer cmd = vk_cmd(d);
	vk_barriers b;
	d->tracker.use_image(tex->image, d->graphics.family,
			     {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL},
			     b);
	vk_record_barriers(cmd, b);
	vkCmdCopyBufferToImage(cmd, buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regions.size(),
			       regions.data());
	d->frames.defer_destroy([vma = d->vma, buffer, alloc] { vmaDestroyBuffer(vma, buffer, alloc); });
	return true;
}

// Fills levels 1.. from level 0, each blit reading the level the previous one wrote.
static void generate_mips(gs_device *d, gs_texture *tex)
{
	VkCommandBuffer cmd = vk_cmd(d);
	for (uint32_t i = 1; i < tex->levels; ++i) {
		// GENERAL: the image is blit source and destination; the tracker's whole-image barrier orders the levels.
		vk_barriers b;
		d->tracker.use_image(tex->image, d->graphics.family,
				     {VK_PIPELINE_STAGE_2_BLIT_BIT,
				      VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
				      VK_IMAGE_LAYOUT_GENERAL},
				     b);
		vk_record_barriers(cmd, b);

		VkImageBlit blit = {};
		blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
		blit.srcOffsets[1] = {(int32_t)mip_size(tex->width, i - 1), (int32_t)mip_size(tex->height, i - 1), 1};
		blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
		blit.dstOffsets[1] = {(int32_t)mip_size(tex->width, i), (int32_t)mip_size(tex->height, i), 1};
		vkCmdBlitImage(cmd, tex->image, VK_IMAGE_LAYOUT_GENERAL, tex->image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
			       VK_FILTER_LINEAR);
	}
}

static void destroy_texture_objects(gs_device *d, const gs_texture *tex)
{
	d->tracker.forget(tex->image);
	if (tex->map_ptr) { // mapped, never unmapped: arena space, nothing to free; the slot may reuse it again
		--d->frames.frames[tex->map_slot].maps;
	}
	d->frames.defer_destroy([dev = d->dev, vma = d->vma, t = *tex] {
		vkDestroyImageView(dev, t.view, nullptr);
		vkDestroyImageView(dev, t.srgb_view, nullptr);
		vkDestroyImageView(dev, t.rt_view, nullptr);
		vkDestroyImageView(dev, t.rt_srgb_view, nullptr);
		vmaDestroyImage(vma, t.image, t.alloc);
	});
}

// 2D textures, or volume textures (type GS_TEXTURE_3D) with data per level holding all its slices.
static gs_texture *create_texture(gs_device *device, enum gs_texture_type type, uint32_t width, uint32_t height,
				  uint32_t depth, enum gs_color_format color_format, uint32_t levels,
				  const uint8_t *const *data, uint32_t flags)
{
	VkFormat fmt = vk_format(color_format);
	if (fmt == VK_FORMAT_UNDEFINED) {
		blog(LOG_ERROR, "[libobs-vulkan] device_texture_create: unsupported format %d", (int)color_format);
		return nullptr;
	}
	const bool vol = type == GS_TEXTURE_3D;
	if (vol && (flags & (GS_RENDER_TARGET | GS_DYNAMIC))) {
		// shortcut: no core effect or plugin renders into or maps a volume texture; D3D11 allows both.
		blog(LOG_ERROR,
		     "[libobs-vulkan] device_voltexture_create: render target and dynamic flags unsupported");
		return nullptr;
	}
	if (vol && (flags & GS_BUILD_MIPMAPS)) {
		// shortcut: generate_mips blits 2D levels only; volume LUTs (color_grade_filter) use one level.
		blog(LOG_WARNING, "[libobs-vulkan] device_voltexture_create: GS_BUILD_MIPMAPS ignored");
		flags &= ~GS_BUILD_MIPMAPS;
		levels = 1;
	}

	// As D3D11: GS_BUILD_MIPMAPS means the full chain whatever `levels` says.
	const uint32_t full = gs_get_total_levels(width, height, depth);
	bool build_mips = (flags & GS_BUILD_MIPMAPS) != 0;
	if (build_mips) {
		VkFormatProperties fp;
		vkGetPhysicalDeviceFormatProperties(device->phys, fmt, &fp);
		const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
						  VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
		if ((fp.optimalTilingFeatures & need) != need) {
			blog(LOG_WARNING, "[libobs-vulkan] device_texture_create: format %d cannot blit mipmaps",
			     (int)color_format);
			build_mips = false;
			levels = 1;
		}
	}
	levels = build_mips ? full : std::clamp(levels, 1u, full);

	VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				  VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (flags & GS_RENDER_TARGET) {
		usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	}

	VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	const bool srgb = gs_is_srgb_format(color_format);
	const VkFormat srgb_fmt = fmt == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_B8G8R8A8_SRGB;
	const VkFormat view_formats[] = {fmt, srgb_fmt};
	VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
	if (srgb) {
		fl.viewFormatCount = 2;
		fl.pViewFormats = view_formats;
		ii.pNext = &fl;
		ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
	}

	VkImageFormatProperties props;
	ii.imageType = vol ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
	if (vkGetPhysicalDeviceImageFormatProperties(device->phys, fmt, ii.imageType, VK_IMAGE_TILING_OPTIMAL, usage,
						     ii.flags, &props) != VK_SUCCESS ||
	    width > props.maxExtent.width || height > props.maxExtent.height || depth > props.maxExtent.depth) {
		blog(LOG_ERROR, "[libobs-vulkan] device_texture_create: format %d unsupported for %ux%u",
		     (int)color_format, width, height);
		return nullptr;
	}

	ii.format = fmt;
	ii.extent = {width, height, depth};
	ii.mipLevels = levels;
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
	tex->depth = depth;
	tex->type = type;
	tex->levels = levels;
	tex->format = color_format;
	if (vmaCreateImage(device->vma, &ii, &ai, &tex->image, &tex->alloc, nullptr) != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] device_texture_create: vmaCreateImage failed");
		delete tex;
		return nullptr;
	}
	tex->view = create_view(device, tex, fmt);
	if (srgb) {
		tex->srgb_view = create_view(device, tex, srgb_fmt);
	}
	if (flags & GS_RENDER_TARGET) {
		tex->rt_format = fmt;
		tex->rt_view = create_view(device, tex, fmt, true);
		if (srgb) {
			tex->rt_srgb_format = srgb_fmt;
			tex->rt_srgb_view = create_view(device, tex, srgb_fmt, true);
		}
	}
	bool ok = tex->view && (!srgb || tex->srgb_view) && (!(flags & GS_RENDER_TARGET) || tex->rt_view) &&
		  (!(flags & GS_RENDER_TARGET) || !srgb || tex->rt_srgb_view);
	tex->dynamic = (flags & GS_DYNAMIC) != 0;
	if (ok && data) {
		ok = upload(device, tex, data, build_mips ? 1 : levels);
		if (ok && build_mips) {
			generate_mips(device, tex);
		}
	}
	if (!ok) {
		blog(LOG_ERROR, "[libobs-vulkan] device_texture_create: view or upload failed");
		destroy_texture_objects(device, tex);
		delete tex;
		return nullptr;
	}
	return tex;
}

extern "C" {

EXPORT gs_texture_t *device_texture_create(gs_device_t *device, uint32_t width, uint32_t height,
					   enum gs_color_format color_format, uint32_t levels, const uint8_t **data,
					   uint32_t flags)
{
	return create_texture(device, GS_TEXTURE_2D, width, height, 1, color_format, levels, data, flags);
}

EXPORT gs_texture_t *device_voltexture_create(gs_device_t *device, uint32_t width, uint32_t height, uint32_t depth,
					      enum gs_color_format color_format, uint32_t levels,
					      const uint8_t *const *data, uint32_t flags)
{
	return create_texture(device, GS_TEXTURE_3D, width, height, depth, color_format, levels, data, flags);
}

EXPORT void gs_voltexture_destroy(gs_texture_t *voltex)
{
	gs_texture_destroy(voltex);
}

EXPORT uint32_t gs_voltexture_get_width(const gs_texture_t *voltex)
{
	return voltex->width;
}

EXPORT uint32_t gs_voltexture_get_height(const gs_texture_t *voltex)
{
	return voltex->height;
}

EXPORT uint32_t gs_voltexture_get_depth(const gs_texture_t *voltex)
{
	return voltex->depth;
}

EXPORT enum gs_color_format gs_voltexture_get_color_format(const gs_texture_t *voltex)
{
	return voltex->format;
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
	for (gs_texture *&t : d->cur_textures) {
		if (t == tex) {
			t = nullptr;
		}
	}
	destroy_texture_objects(d, tex);
	delete tex;
}

static VkDeviceSize map_size(const gs_texture *tex)
{
	return (VkDeviceSize)tex->width * tex->height * (gs_get_format_bpp(tex->format) / 8);
}

// Never waits on the GPU (as D3D11's WRITE_DISCARD): the space comes from the recording frame's upload arena, which
// the GPU stops reading before the slot is reused. Mip 0 only, tightly packed. As D3D11, a mapped texture cannot be
// mapped again before its unmap.
EXPORT bool gs_texture_map(gs_texture_t *tex, uint8_t **ptr, uint32_t *linesize)
{
	if (!tex->dynamic || tex->map_ptr) {
		return false;
	}
	gs_device *d = tex->device;
	// 16: a multiple of 4 and of every format's texel size, as vkCmdCopyBufferToImage's bufferOffset requires.
	uint8_t *p = vk_arena_alloc(d, map_size(tex), 16, tex->map_buf, tex->map_alloc);
	if (!p) {
		return false;
	}
	tex->map_slot = d->frames.index;
	++d->frames.frames[tex->map_slot].maps;
	tex->map_ptr = p;
	*ptr = p;
	*linesize = tex->width * gs_get_format_bpp(tex->format) / 8;
	return true;
}

// Records the copy into the frame being recorded. The mapped space is never reused while mapped (vk_frame::maps).
// When a submit came between map and unmap (gs_flush, a present) it belongs to another slot's arena, which this
// frame's copy must not outlive, so the data is first copied into this frame's arena. When it is this slot's (the
// usual case, or the slot was begun again after a failed submit or a full turn of the ring) it is read in place.
EXPORT void gs_texture_unmap(gs_texture_t *tex)
{
	uint8_t *src = tex->map_ptr;
	if (!src) {
		return;
	}
	tex->map_ptr = nullptr;
	gs_device *d = tex->device;
	const VkDeviceSize size = map_size(tex);
	VkCommandBuffer cmd = vk_cmd(d);
	vk_buffer buf = tex->map_buf;
	bool ok = true;
	if (tex->map_slot == d->frames.index) {
		// Host writes before the submission are visible to it without a barrier.
		vmaFlushAllocation(d->vma, tex->map_alloc, buf.offset, size);
	} else {
		ok = vk_arena_upload(d, src, size, 16, buf);
	}
	--d->frames.frames[tex->map_slot].maps; // after the copy above: the slot may reset its arena from now on
	if (!ok) {
		return;
	}

	vk_barriers b;
	d->tracker.use_image(tex->image, d->graphics.family,
			     {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
			      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL},
			     b);
	vk_record_barriers(cmd, b);

	VkBufferImageCopy region = {};
	region.bufferOffset = buf.offset;
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	region.imageExtent = {tex->width, tex->height, 1};
	vkCmdCopyBufferToImage(cmd, buf.buffer, tex->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

EXPORT enum gs_texture_type device_get_texture_type(const gs_texture_t *texture)
{
	return texture->type;
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

// As D3D11's DXGI formats where the device has them; D24S8 and D32S8 stand in for each other (Vulkan guarantees one).
static VkFormat zstencil_format(gs_device *d, enum gs_zstencil_format format)
{
	VkFormat want[2] = {};
	switch (format) {
	case GS_Z16:
		want[0] = VK_FORMAT_D16_UNORM;
		break;
	case GS_Z24_S8:
		want[0] = VK_FORMAT_D24_UNORM_S8_UINT;
		want[1] = VK_FORMAT_D32_SFLOAT_S8_UINT;
		break;
	case GS_Z32F:
		want[0] = VK_FORMAT_D32_SFLOAT;
		break;
	case GS_Z32F_S8X24:
		want[0] = VK_FORMAT_D32_SFLOAT_S8_UINT;
		want[1] = VK_FORMAT_D24_UNORM_S8_UINT;
		break;
	default:
		return VK_FORMAT_UNDEFINED;
	}
	const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
					  VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
	for (VkFormat f : want) {
		VkFormatProperties fp;
		if (f) {
			vkGetPhysicalDeviceFormatProperties(d->phys, f, &fp);
			if ((fp.optimalTilingFeatures & need) == need) {
				return f;
			}
		}
	}
	return VK_FORMAT_UNDEFINED;
}

EXPORT gs_zstencil_t *device_zstencil_create(gs_device_t *device, uint32_t width, uint32_t height,
					     enum gs_zstencil_format format)
{
	const VkFormat fmt = zstencil_format(device, format);
	const VkImageUsageFlags usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	VkImageFormatProperties props;
	if (!fmt ||
	    vkGetPhysicalDeviceImageFormatProperties(device->phys, fmt, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
						     usage, 0, &props) != VK_SUCCESS ||
	    !width || !height || width > props.maxExtent.width || height > props.maxExtent.height) {
		blog(LOG_ERROR, "[libobs-vulkan] device_zstencil_create: format %d unsupported for %ux%u", (int)format,
		     width, height);
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

	auto zs = new gs_zstencil_buffer{device};
	zs->format = fmt;
	zs->aspect = VK_IMAGE_ASPECT_DEPTH_BIT |
		     (fmt == VK_FORMAT_D16_UNORM || fmt == VK_FORMAT_D32_SFLOAT ? 0 : VK_IMAGE_ASPECT_STENCIL_BIT);
	zs->width = width;
	zs->height = height;
	if (vmaCreateImage(device->vma, &ii, &ai, &zs->image, &zs->alloc, nullptr) != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] device_zstencil_create: vmaCreateImage failed");
		delete zs;
		return nullptr;
	}
	VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vi.image = zs->image;
	vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vi.format = fmt;
	vi.subresourceRange = {zs->aspect, 0, 1, 0, 1};
	if (vkCreateImageView(device->dev, &vi, nullptr, &zs->view) != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] device_zstencil_create: vkCreateImageView failed");
		vmaDestroyImage(device->vma, zs->image, zs->alloc); // never used by a command
		delete zs;
		return nullptr;
	}
	return zs;
}

EXPORT void gs_zstencil_destroy(gs_zstencil_t *zstencil)
{
	if (!zstencil) {
		return;
	}
	gs_device *d = zstencil->device;
	if (d->zstencil == zstencil) {
		d->zstencil = nullptr;
	}
	d->tracker.forget(zstencil->image);
	d->frames.defer_destroy(
		[dev = d->dev, vma = d->vma, view = zstencil->view, image = zstencil->image, alloc = zstencil->alloc] {
			vkDestroyImageView(dev, view, nullptr);
			vmaDestroyImage(vma, image, alloc);
		});
	delete zstencil;
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

	if (!vk_create_host_buffer(device, (VkDeviceSize)s->linesize * height, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
				   VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT, s->buffer, s->alloc, s->mapped)) {
		blog(LOG_ERROR, "[libobs-vulkan] device_stagesurface_create: vmaCreateBuffer failed");
		delete s;
		return nullptr;
	}
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
