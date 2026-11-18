// Graphics exports. Device lifetime, render target, clear, staging and flush are real; the rest are still stubs.
// Textures and stage surfaces live in vulkan-texture.cpp, swapchains and present in vulkan-swapchain.cpp, buffers and
// samplers in vulkan-buffers.cpp, state, pipelines and draw in vulkan-pipeline.cpp.
#include "vulkan-subsystem.hpp"

#include <graphics/vec4.h>
#include <util/base.h>

#include <algorithm>
#include <vector>

VkCommandBuffer vk_frame_cmd(gs_device *d)
{
	if (!d->frames.recording) {
		d->frames.begin();
	}
	return d->frames.frames[d->frames.index].cmd;
}

VkCommandBuffer vk_cmd(gs_device *d)
{
	vk_end_rendering(d);
	return vk_frame_cmd(d);
}

bool vk_flush(gs_device *d, VkSemaphore *signal)
{
	if (!d->frames.recording) {
		return true;
	}
	vk_end_rendering(d);
	VkSemaphore wait = d->acquire_wait;
	d->acquire_wait = VK_NULL_HANDLE;
	if (d->frames.submit(d->graphics, wait ? &wait : nullptr, signal)) {
		if (wait) {
			// Unsignalled again once this submission (just made the ring's latest) has run.
			d->frames.defer_destroy([d, wait] { d->spare_sems.push_back(wait); });
		}
		return true;
	}
	// The tracker recorded layouts the failed command buffer never reached; restart from UNDEFINED (contents lost).
	d->tracker = vk_tracker{};
	if (wait) {
		vk_consume_semaphore(d, wait);
	}
	return false;
}

void vk_record_barriers(VkCommandBuffer cmd, const vk_barriers &b)
{
	if (b.images.empty() && b.buffers.empty()) {
		return;
	}
	VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	di.imageMemoryBarrierCount = (uint32_t)b.images.size();
	di.pImageMemoryBarriers = b.images.data();
	di.bufferMemoryBarrierCount = (uint32_t)b.buffers.size();
	di.pBufferMemoryBarriers = b.buffers.data();
	vkCmdPipelineBarrier2(cmd, &di);
}

extern "C" {

EXPORT const char *device_get_name(void)
{
	return "Vulkan";
}

EXPORT int device_get_type(void)
{
	return GS_DEVICE_VULKAN;
}

EXPORT const char *device_preprocessor_name(void)
{
	return "_vulkan";
}

EXPORT int device_create(gs_device_t **device, uint32_t adapter)
{
	*device = nullptr;
	auto d = new gs_device;
	int ret = vk_device_init(d, adapter); // frees everything it created on failure
	if (ret != GS_SUCCESS) {
		delete d;
		return ret;
	}
	d->pipelines.device = d;
	vk_glslang_ref(); // keeps glslang's built-in symbol tables alive between compiles
	*device = d;
	return GS_SUCCESS;
}

EXPORT void device_destroy(gs_device_t *device)
{
	if (!device) {
		return;
	}
	vk_device_free(device);
	delete device;
	vk_glslang_unref();
}

// Vulkan has no current-context state; commands are recorded per frame and submitted explicitly.
EXPORT void device_enter_context(gs_device_t *device) {}

EXPORT void device_leave_context(gs_device_t *device) {}

EXPORT const char *gpu_get_driver_version(void)
{
	return vk_driver_version();
}

EXPORT const char *gpu_get_renderer(void)
{
	return vk_adapter_name();
}

EXPORT bool device_enum_adapters(gs_device_t *device, bool (*callback)(void *, const char *, uint32_t), void *param)
{
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(device->instance, &n, nullptr);
	std::vector<VkPhysicalDevice> phys(n);
	vkEnumeratePhysicalDevices(device->instance, &n, phys.data());
	for (uint32_t i = 0; i < n; ++i) {
		VkPhysicalDeviceProperties props;
		vkGetPhysicalDeviceProperties(phys[i], &props);
		if (!callback(param, props.deviceName, i)) {
			break;
		}
	}
	return true;
}

EXPORT void *device_get_device_obj(gs_device_t *device)
{
	return {};
}

EXPORT gs_texture_t *device_cubetexture_create(gs_device_t *device, uint32_t size, enum gs_color_format color_format,
					       uint32_t levels, const uint8_t **data, uint32_t flags)
{
	return {};
}

EXPORT gs_texture_t *device_voltexture_create(gs_device_t *device, uint32_t width, uint32_t height, uint32_t depth,
					      enum gs_color_format color_format, uint32_t levels,
					      const uint8_t *const *data, uint32_t flags)
{
	return {};
}

EXPORT gs_zstencil_t *device_zstencil_create(gs_device_t *device, uint32_t width, uint32_t height,
					     enum gs_zstencil_format format)
{
	return {};
}

EXPORT gs_timer_t *device_timer_create(gs_device_t *device)
{
	return {};
}

EXPORT gs_timer_range_t *device_timer_range_create(gs_device_t *device)
{
	return {};
}

EXPORT gs_texture_t *device_get_render_target(const gs_device_t *device)
{
	return device->render_target;
}

EXPORT gs_zstencil_t *device_get_zstencil_target(const gs_device_t *device)
{
	return {};
}

// shortcut: zstencil ignored until depth targets exist (Task 7).
EXPORT void device_set_render_target_with_color_space(gs_device_t *device, gs_texture_t *tex, gs_zstencil_t *zstencil,
						      enum gs_color_space space)
{
	// As D3D11: no texture means the loaded swapchain's.
	device->render_target = tex ? tex : device->swap ? &device->swap->target : nullptr;
	device->space = space;
}

EXPORT void device_set_render_target(gs_device_t *device, gs_texture_t *tex, gs_zstencil_t *zstencil)
{
	device_set_render_target_with_color_space(device, tex, zstencil, GS_CS_SRGB);
}

EXPORT void device_set_cube_render_target(gs_device_t *device, gs_texture_t *cubetex, int side, gs_zstencil_t *zstencil)
{
}

EXPORT void device_copy_texture(gs_device_t *device, gs_texture_t *dst, gs_texture_t *src) {}

EXPORT void device_copy_texture_region(gs_device_t *device, gs_texture_t *dst, uint32_t dst_x, uint32_t dst_y,
				       gs_texture_t *src, uint32_t src_x, uint32_t src_y, uint32_t src_w,
				       uint32_t src_h)
{
}

EXPORT void obs_vulkan_test_stage_mip(gs_device_t *device, gs_stagesurf_t *dst, gs_texture_t *src, uint32_t level)
{
	if (!dst || !src || level >= src->levels || dst->width != std::max(src->width >> level, 1u) ||
	    dst->height != std::max(src->height >> level, 1u) || dst->format != src->format) {
		blog(LOG_ERROR, "[libobs-vulkan] device_stage_texture: size or format mismatch");
		return;
	}
	VkCommandBuffer cmd = vk_cmd(device);
	const uint32_t fam = device->graphics.family;
	vk_barriers b;
	device->tracker.use_image(
		src->image, fam,
		{VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL}, b);
	device->tracker.use_buffer(dst->buffer, fam, {VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT}, b);
	vk_record_barriers(cmd, b);

	VkBufferImageCopy region = {};
	region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
	region.imageExtent = {dst->width, dst->height, 1};
	vkCmdCopyImageToBuffer(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->buffer, 1, &region);

	// Makes the copy available to the host once the submission's timeline value signals.
	vk_barriers host;
	device->tracker.use_buffer(dst->buffer, fam, {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT}, host);
	vk_record_barriers(cmd, host);
	dst->copy_value = device->graphics.next_value; // the value the frame being recorded will signal
}

EXPORT void device_stage_texture(gs_device_t *device, gs_stagesurf_t *dst, gs_texture_t *src)
{
	obs_vulkan_test_stage_mip(device, dst, src, 0);
}

EXPORT void device_begin_frame(gs_device_t *device) {}

EXPORT void device_end_scene(gs_device_t *device) {}

// shortcut: colour only, as a transfer clear outside rendering (vk_cmd ends an open instance; the next draw's tracker
// barrier orders it before the colour attachment writes). A load op would save the barrier.
EXPORT void device_clear(gs_device_t *device, uint32_t clear_flags, const struct vec4 *color, float depth,
			 uint8_t stencil)
{
	gs_texture *rt = device->render_target;
	if (!(clear_flags & GS_CLEAR_COLOR) || !rt || !rt->image || !color) { // no image: swapchain not acquired
		return;
	}
	VkCommandBuffer cmd = vk_cmd(device);
	vk_barriers b;
	device->tracker.use_image(rt->image, device->graphics.family,
				  {VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
				   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL},
				  b);
	vk_record_barriers(cmd, b);

	VkClearColorValue value = {{color->x, color->y, color->z, color->w}};
	VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
					 VK_REMAINING_ARRAY_LAYERS};
	vkCmdClearColorImage(cmd, rt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
}

EXPORT void device_flush(gs_device_t *device)
{
	vk_flush(device);
}

EXPORT void *gs_texture_get_obj(gs_texture_t *tex)
{
	return {};
}

EXPORT void gs_cubetexture_destroy(gs_texture_t *cubetex) {}

EXPORT uint32_t gs_cubetexture_get_size(const gs_texture_t *cubetex)
{
	return {};
}

EXPORT enum gs_color_format gs_cubetexture_get_color_format(const gs_texture_t *cubetex)
{
	return {};
}

EXPORT void gs_voltexture_destroy(gs_texture_t *voltex) {}

EXPORT uint32_t gs_voltexture_get_width(const gs_texture_t *voltex)
{
	return {};
}

EXPORT uint32_t gs_voltexture_get_height(const gs_texture_t *voltex)
{
	return {};
}

EXPORT uint32_t gs_voltexture_get_depth(const gs_texture_t *voltex)
{
	return {};
}

EXPORT enum gs_color_format gs_voltexture_get_color_format(const gs_texture_t *voltex)
{
	return {};
}

EXPORT void gs_zstencil_destroy(gs_zstencil_t *zstencil) {}

EXPORT void gs_timer_destroy(gs_timer_t *timer) {}

EXPORT void gs_timer_begin(gs_timer_t *timer) {}

EXPORT void gs_timer_end(gs_timer_t *timer) {}

EXPORT bool gs_timer_get_data(gs_timer_t *timer, uint64_t *ticks)
{
	return {};
}

EXPORT void gs_timer_range_destroy(gs_timer_range_t *range) {}

EXPORT void gs_timer_range_begin(gs_timer_range_t *range) {}

EXPORT void gs_timer_range_end(gs_timer_range_t *range) {}

EXPORT bool gs_timer_range_get_data(gs_timer_range_t *range, bool *disjoint, uint64_t *frequency)
{
	return {};
}

EXPORT bool device_is_monitor_hdr(gs_device_t *device, void *monitor)
{
	return {};
}

EXPORT void device_debug_marker_begin(gs_device_t *device, const char *markername, const float color[4]) {}

EXPORT void device_debug_marker_end(gs_device_t *device) {}

#ifdef __APPLE__

EXPORT gs_texture_t *device_texture_create_from_iosurface(gs_device_t *dev, void *iosurf)
{
	return {};
}

EXPORT gs_texture_t *device_texture_open_shared(gs_device_t *dev, uint32_t handle)
{
	return {};
}

EXPORT bool gs_texture_rebind_iosurface(gs_texture_t *texture, void *iosurf)
{
	return {};
}

EXPORT bool device_shared_texture_available(void)
{
	return {};
}

#elif _WIN32

EXPORT bool device_gdi_texture_available(void)
{
	return {};
}

EXPORT bool device_shared_texture_available(void)
{
	return {};
}

#elif defined(__linux__) || defined(__FreeBSD__) || defined(__DragonFly__)

EXPORT struct gs_texture *device_texture_create_from_dmabuf(gs_device_t *device, unsigned int width,
							    unsigned int height, uint32_t drm_format,
							    enum gs_color_format color_format, uint32_t n_planes,
							    const int *fds, const uint32_t *strides,
							    const uint32_t *offsets, const uint64_t *modifiers)
{
	return {};
}

EXPORT bool device_query_dmabuf_capabilities(gs_device_t *device, enum gs_dmabuf_flags *dmabuf_flags,
					     uint32_t **drm_formats, size_t *n_formats)
{
	return {};
}

EXPORT bool device_query_dmabuf_modifiers_for_format(gs_device_t *device, uint32_t drm_format, uint64_t **modifiers,
						     size_t *n_modifiers)
{
	return {};
}

EXPORT struct gs_texture *device_texture_create_from_pixmap(gs_device_t *device, uint32_t width, uint32_t height,
							    enum gs_color_format color_format, uint32_t target,
							    void *pixmap)
{
	return {};
}

EXPORT bool device_query_sync_capabilities(gs_device_t *device)
{
	return {};
}

EXPORT gs_sync_t *device_sync_create(gs_device_t *device)
{
	return {};
}

EXPORT gs_sync_t *device_sync_create_from_syncobj_timeline_point(gs_device_t *device, int syncobj_fd,
								 uint64_t timeline_point)
{
	return {};
}

EXPORT void device_sync_destroy(gs_device_t *device, gs_sync_t *sync) {}

EXPORT bool device_sync_export_syncobj_timeline_point(gs_device_t *device, gs_sync_t *sync, int syncobj_fd,
						      uint64_t timeline_point)
{
	return {};
}

EXPORT bool device_sync_signal_syncobj_timeline_point(gs_device_t *device, int syncobj_fd, uint64_t timeline_point)
{
	return {};
}

EXPORT bool device_sync_wait(gs_device_t *device, gs_sync_t *sync)
{
	return {};
}

#endif
}
