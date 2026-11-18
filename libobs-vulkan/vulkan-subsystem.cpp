// Graphics exports. Device lifetime is real; the rest are still stubs until later tasks.
#include "vulkan-subsystem.hpp"

#include <vector>

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
	*device = d;
	return GS_SUCCESS;
}

EXPORT void device_destroy(gs_device_t *device)
{
	if (!device)
		return;
	vk_device_free(device);
	delete device;
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
		if (!callback(param, props.deviceName, i))
			break;
	}
	return true;
}

EXPORT void *device_get_device_obj(gs_device_t *device)
{
	return {};
}

EXPORT gs_swapchain_t *device_swapchain_create(gs_device_t *device, const struct gs_init_data *data)
{
	return {};
}

EXPORT void device_resize(gs_device_t *device, uint32_t x, uint32_t y) {}

EXPORT enum gs_color_space device_get_color_space(gs_device_t *device)
{
	return {};
}

EXPORT void device_update_color_space(gs_device_t *device) {}

EXPORT void device_get_size(const gs_device_t *device, uint32_t *x, uint32_t *y) {}

EXPORT uint32_t device_get_width(const gs_device_t *device)
{
	return {};
}

EXPORT uint32_t device_get_height(const gs_device_t *device)
{
	return {};
}

EXPORT gs_texture_t *device_texture_create(gs_device_t *device, uint32_t width, uint32_t height,
					   enum gs_color_format color_format, uint32_t levels, const uint8_t **data,
					   uint32_t flags)
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

EXPORT gs_stagesurf_t *device_stagesurface_create(gs_device_t *device, uint32_t width, uint32_t height,
						  enum gs_color_format color_format)
{
	return {};
}

EXPORT gs_samplerstate_t *device_samplerstate_create(gs_device_t *device, const struct gs_sampler_info *info)
{
	return {};
}

EXPORT gs_shader_t *device_vertexshader_create(gs_device_t *device, const char *shader, const char *file,
					       char **error_string)
{
	return {};
}

EXPORT gs_shader_t *device_pixelshader_create(gs_device_t *device, const char *shader, const char *file,
					      char **error_string)
{
	return {};
}

EXPORT gs_vertbuffer_t *device_vertexbuffer_create(gs_device_t *device, struct gs_vb_data *data, uint32_t flags)
{
	return {};
}

EXPORT gs_indexbuffer_t *device_indexbuffer_create(gs_device_t *device, enum gs_index_type type, void *indices,
						   size_t num, uint32_t flags)
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

EXPORT enum gs_texture_type device_get_texture_type(const gs_texture_t *texture)
{
	return {};
}

EXPORT void device_load_vertexbuffer(gs_device_t *device, gs_vertbuffer_t *vertbuffer) {}

EXPORT void device_load_indexbuffer(gs_device_t *device, gs_indexbuffer_t *indexbuffer) {}

EXPORT void device_load_texture(gs_device_t *device, gs_texture_t *tex, int unit) {}

EXPORT void device_load_samplerstate(gs_device_t *device, gs_samplerstate_t *samplerstate, int unit) {}

EXPORT void device_load_vertexshader(gs_device_t *device, gs_shader_t *vertshader) {}

EXPORT void device_load_pixelshader(gs_device_t *device, gs_shader_t *pixelshader) {}

EXPORT void device_load_default_samplerstate(gs_device_t *device, bool b_3d, int unit) {}

EXPORT gs_shader_t *device_get_vertex_shader(const gs_device_t *device)
{
	return {};
}

EXPORT gs_shader_t *device_get_pixel_shader(const gs_device_t *device)
{
	return {};
}

EXPORT gs_texture_t *device_get_render_target(const gs_device_t *device)
{
	return {};
}

EXPORT gs_zstencil_t *device_get_zstencil_target(const gs_device_t *device)
{
	return {};
}

EXPORT void device_set_render_target(gs_device_t *device, gs_texture_t *tex, gs_zstencil_t *zstencil) {}

EXPORT void device_set_render_target_with_color_space(gs_device_t *device, gs_texture_t *tex, gs_zstencil_t *zstencil,
						      enum gs_color_space space)
{
}

EXPORT void device_set_cube_render_target(gs_device_t *device, gs_texture_t *cubetex, int side, gs_zstencil_t *zstencil)
{
}

EXPORT void device_enable_framebuffer_srgb(gs_device_t *device, bool enable) {}

EXPORT bool device_framebuffer_srgb_enabled(gs_device_t *device)
{
	return {};
}

EXPORT void device_copy_texture(gs_device_t *device, gs_texture_t *dst, gs_texture_t *src) {}

EXPORT void device_copy_texture_region(gs_device_t *device, gs_texture_t *dst, uint32_t dst_x, uint32_t dst_y,
				       gs_texture_t *src, uint32_t src_x, uint32_t src_y, uint32_t src_w,
				       uint32_t src_h)
{
}

EXPORT void device_stage_texture(gs_device_t *device, gs_stagesurf_t *dst, gs_texture_t *src) {}

EXPORT void device_begin_frame(gs_device_t *device) {}

EXPORT void device_begin_scene(gs_device_t *device) {}

EXPORT void device_draw(gs_device_t *device, enum gs_draw_mode draw_mode, uint32_t start_vert, uint32_t num_verts) {}

EXPORT void device_end_scene(gs_device_t *device) {}

EXPORT void device_load_swapchain(gs_device_t *device, gs_swapchain_t *swaphchain) {}

EXPORT void device_clear(gs_device_t *device, uint32_t clear_flags, const struct vec4 *color, float depth,
			 uint8_t stencil)
{
}

EXPORT bool device_is_present_ready(gs_device_t *device)
{
	return {};
}

EXPORT void device_present(gs_device_t *device) {}

EXPORT void device_flush(gs_device_t *device) {}

EXPORT void device_set_cull_mode(gs_device_t *device, enum gs_cull_mode mode) {}

EXPORT enum gs_cull_mode device_get_cull_mode(const gs_device_t *device)
{
	return {};
}

EXPORT void device_enable_blending(gs_device_t *device, bool enable) {}

EXPORT void device_enable_depth_test(gs_device_t *device, bool enable) {}

EXPORT void device_enable_stencil_test(gs_device_t *device, bool enable) {}

EXPORT void device_enable_stencil_write(gs_device_t *device, bool enable) {}

EXPORT void device_enable_color(gs_device_t *device, bool red, bool green, bool blue, bool alpha) {}

EXPORT void device_blend_function(gs_device_t *device, enum gs_blend_type src, enum gs_blend_type dest) {}

EXPORT void device_blend_function_separate(gs_device_t *device, enum gs_blend_type src_c, enum gs_blend_type dest_c,
					   enum gs_blend_type src_a, enum gs_blend_type dest_a)
{
}

EXPORT void device_blend_op(gs_device_t *device, enum gs_blend_op_type op) {}

EXPORT void device_depth_function(gs_device_t *device, enum gs_depth_test test) {}

EXPORT void device_stencil_function(gs_device_t *device, enum gs_stencil_side side, enum gs_depth_test test) {}

EXPORT void device_stencil_op(gs_device_t *device, enum gs_stencil_side side, enum gs_stencil_op_type fail,
			      enum gs_stencil_op_type zfail, enum gs_stencil_op_type zpass)
{
}

EXPORT void device_set_viewport(gs_device_t *device, int x, int y, int width, int height) {}

EXPORT void device_get_viewport(const gs_device_t *device, struct gs_rect *rect) {}

EXPORT void device_set_scissor_rect(gs_device_t *device, const struct gs_rect *rect) {}

EXPORT void device_ortho(gs_device_t *device, float left, float right, float top, float bottom, float znear, float zfar)
{
}

EXPORT void device_frustum(gs_device_t *device, float left, float right, float top, float bottom, float znear,
			   float zfar)
{
}

EXPORT void device_projection_push(gs_device_t *device) {}

EXPORT void device_projection_pop(gs_device_t *device) {}

EXPORT void gs_swapchain_destroy(gs_swapchain_t *swapchain) {}

EXPORT void gs_texture_destroy(gs_texture_t *tex) {}

EXPORT uint32_t gs_texture_get_width(const gs_texture_t *tex)
{
	return {};
}

EXPORT uint32_t gs_texture_get_height(const gs_texture_t *tex)
{
	return {};
}

EXPORT enum gs_color_format gs_texture_get_color_format(const gs_texture_t *tex)
{
	return {};
}

EXPORT bool gs_texture_map(gs_texture_t *tex, uint8_t **ptr, uint32_t *linesize)
{
	return {};
}

EXPORT void gs_texture_unmap(gs_texture_t *tex) {}

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

EXPORT void gs_stagesurface_destroy(gs_stagesurf_t *stagesurf) {}

EXPORT uint32_t gs_stagesurface_get_width(const gs_stagesurf_t *stagesurf)
{
	return {};
}

EXPORT uint32_t gs_stagesurface_get_height(const gs_stagesurf_t *stagesurf)
{
	return {};
}

EXPORT enum gs_color_format gs_stagesurface_get_color_format(const gs_stagesurf_t *stagesurf)
{
	return {};
}

EXPORT bool gs_stagesurface_map(gs_stagesurf_t *stagesurf, uint8_t **data, uint32_t *linesize)
{
	return {};
}

EXPORT void gs_stagesurface_unmap(gs_stagesurf_t *stagesurf) {}

EXPORT void gs_zstencil_destroy(gs_zstencil_t *zstencil) {}

EXPORT void gs_samplerstate_destroy(gs_samplerstate_t *samplerstate) {}

EXPORT void gs_vertexbuffer_destroy(gs_vertbuffer_t *vertbuffer) {}

EXPORT void gs_vertexbuffer_flush(gs_vertbuffer_t *vertbuffer) {}

EXPORT void gs_vertexbuffer_flush_direct(gs_vertbuffer_t *vertbuffer, const struct gs_vb_data *data) {}

EXPORT struct gs_vb_data *gs_vertexbuffer_get_data(const gs_vertbuffer_t *vertbuffer)
{
	return {};
}

EXPORT void gs_indexbuffer_destroy(gs_indexbuffer_t *indexbuffer) {}

EXPORT void gs_indexbuffer_flush(gs_indexbuffer_t *indexbuffer) {}

EXPORT void gs_indexbuffer_flush_direct(gs_indexbuffer_t *indexbuffer, const void *data) {}

EXPORT void *gs_indexbuffer_get_data(const gs_indexbuffer_t *indexbuffer)
{
	return {};
}

EXPORT size_t gs_indexbuffer_get_num_indices(const gs_indexbuffer_t *indexbuffer)
{
	return {};
}

EXPORT enum gs_index_type gs_indexbuffer_get_type(const gs_indexbuffer_t *indexbuffer)
{
	return {};
}

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

EXPORT void gs_shader_destroy(gs_shader_t *shader) {}

EXPORT int gs_shader_get_num_params(const gs_shader_t *shader)
{
	return {};
}

EXPORT gs_sparam_t *gs_shader_get_param_by_idx(gs_shader_t *shader, uint32_t param)
{
	return {};
}

EXPORT gs_sparam_t *gs_shader_get_param_by_name(gs_shader_t *shader, const char *name)
{
	return {};
}

EXPORT gs_sparam_t *gs_shader_get_viewproj_matrix(const gs_shader_t *shader)
{
	return {};
}

EXPORT gs_sparam_t *gs_shader_get_world_matrix(const gs_shader_t *shader)
{
	return {};
}

EXPORT void gs_shader_get_param_info(const gs_sparam_t *param, struct gs_shader_param_info *info) {}

EXPORT void gs_shader_set_bool(gs_sparam_t *param, bool val) {}

EXPORT void gs_shader_set_float(gs_sparam_t *param, float val) {}

EXPORT void gs_shader_set_int(gs_sparam_t *param, int val) {}

EXPORT void gs_shader_set_matrix3(gs_sparam_t *param, const struct matrix3 *val) {}

EXPORT void gs_shader_set_matrix4(gs_sparam_t *param, const struct matrix4 *val) {}

EXPORT void gs_shader_set_vec2(gs_sparam_t *param, const struct vec2 *val) {}

EXPORT void gs_shader_set_vec3(gs_sparam_t *param, const struct vec3 *val) {}

EXPORT void gs_shader_set_vec4(gs_sparam_t *param, const struct vec4 *val) {}

EXPORT void gs_shader_set_texture(gs_sparam_t *param, gs_texture_t *val) {}

EXPORT void gs_shader_set_val(gs_sparam_t *param, const void *val, size_t size) {}

EXPORT void gs_shader_set_default(gs_sparam_t *param) {}

EXPORT void gs_shader_set_next_sampler(gs_sparam_t *param, gs_samplerstate_t *sampler) {}

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
