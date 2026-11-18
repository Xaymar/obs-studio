#pragma once

#include <graphics/graphics.h>
#include <graphics/matrix4.h>

#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "vulkan-frame.hpp"
#include "vulkan-tracker.hpp"

// Fixed-function state the device_* setters change; defaults as D3D11. Only uint8_t members, so it has no padding
// and is compared and hashed bytewise.
struct vk_state {
	uint8_t blend = 1;
	uint8_t src_c = GS_BLEND_SRCALPHA, dst_c = GS_BLEND_INVSRCALPHA;
	uint8_t src_a = GS_BLEND_ONE, dst_a = GS_BLEND_INVSRCALPHA;
	uint8_t op = GS_BLEND_OP_ADD;
	uint8_t write_mask = 0xF; // VkColorComponentFlags
	// Depth/stencil; the draw resets it to these defaults in the key when there is no depth (or stencil) attachment.
	uint8_t depth = 1, depth_write = 1, depth_func = GS_LESS;
	uint8_t stencil = 0, stencil_write = 1;
	uint8_t front[4] = {GS_ALWAYS, GS_KEEP, GS_KEEP, GS_KEEP}; // test, fail, zfail, zpass
	uint8_t back[4] = {GS_ALWAYS, GS_KEEP, GS_KEEP, GS_KEEP};
	uint8_t cull = GS_BACK;
};

constexpr uint32_t vk_max_inputs = 16; // vertex shader input locations a pipeline key describes

// Everything a VkPipeline depends on. Scissor enable is not part of it: Vulkan always scissors, a disabled scissor
// is a full-target rectangle (dynamic state, as is the viewport). libobs has no fill mode setter (always solid).
struct vk_pipeline_key {
	gs_shader *vs = nullptr, *ps = nullptr;
	VkFormat color = VK_FORMAT_UNDEFINED, depth = VK_FORMAT_UNDEFINED;
	vk_state state;
	uint8_t topology = 0;                   // VkPrimitiveTopology
	uint8_t components[vk_max_inputs] = {}; // per vertex input location: components of the bound attribute

	bool operator==(const vk_pipeline_key &o) const
	{
		return vs == o.vs && ps == o.ps && color == o.color && depth == o.depth && topology == o.topology &&
		       !memcmp(&state, &o.state, sizeof(state)) &&
		       !memcmp(components, o.components, sizeof(components));
	}
};

template<> struct std::hash<vk_pipeline_key> {
	size_t operator()(const vk_pipeline_key &k) const noexcept
	{
		uint64_t h = 14695981039346656037ull; // FNV-1a
		auto mix = [&h](const void *p, size_t n) {
			for (size_t i = 0; i < n; ++i) {
				h = (h ^ ((const uint8_t *)p)[i]) * 1099511628211ull;
			}
		};
		mix(&k.vs, sizeof(k.vs));
		mix(&k.ps, sizeof(k.ps));
		mix(&k.color, sizeof(k.color));
		mix(&k.depth, sizeof(k.depth));
		mix(&k.state, sizeof(k.state));
		mix(&k.topology, 1);
		mix(k.components, sizeof(k.components));
		return (size_t)h;
	}
};

// Pipelines by key, each created once on first use; a failed creation is cached as VK_NULL_HANDLE (logged once, the
// draw is a no-op) so it is never retried. Layouts are per shader pair: set 0 = vertex shader, set 1 = pixel shader.
struct vk_pipeline_cache {
	gs_device *device = nullptr;
	VkPipelineCache cache = VK_NULL_HANDLE; // in memory only
	struct entry {
		VkPipeline pipeline;
		VkPipelineLayout layout;
	};
	std::unordered_map<vk_pipeline_key, entry> pipelines;
	struct layout_entry {
		gs_shader *vs, *ps;
		VkPipelineLayout layout;
	};
	std::vector<layout_entry> layouts; // searched only when a pipeline is created

	// O(1) lookup; creates the pipeline on the key's first use. `layout` (optional) receives its pipeline layout.
	VkPipeline get(const vk_pipeline_key &key, VkPipelineLayout *layout = nullptr);
	VkPipelineLayout layout(gs_shader *vs, gs_shader *ps);
	void forget(gs_shader *shader); // destroys (deferred) everything built from `shader`
};

struct gs_device {
	VkInstance instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice dev = VK_NULL_HANDLE;
	vk_queue graphics;
	vk_queue transfer; // q == VK_NULL_HANDLE when the device has no distinct transfer family
	VmaAllocator vma = VK_NULL_HANDLE;
	vk_frame_ring frames;
	vk_tracker tracker;
	gs_texture *render_target = nullptr;
	gs_zstencil_buffer *zstencil = nullptr; // depth attachment of the render target; none when null
	enum gs_color_space space = GS_CS_SRGB;
	bool can_present = false; // VK_KHR_surface, the platform surface and VK_KHR_swapchain are enabled
	gs_swap_chain *swap = nullptr;
	VkSemaphore acquire_wait = VK_NULL_HANDLE; // signalled by the last acquire; the next submit waits on it
	std::vector<VkSemaphore> spare_sems;       // unsignalled binary semaphores for acquires
	VkPhysicalDeviceLimits limits = {};
	bool anisotropy = false;   // samplerAnisotropy enabled
	bool mirror_clamp = false; // samplerMirrorClampToEdge enabled (GS_ADDRESS_MIRRORONCE)

	// Draw state, CPU side until device_draw.
	vk_state state;
	vk_pipeline_cache pipelines;
	gs_shader *cur_vs = nullptr, *cur_ps = nullptr;
	gs_vertex_buffer *cur_vb = nullptr;
	gs_index_buffer *cur_ib = nullptr;
	gs_texture *cur_textures[GS_MAX_TEXTURES] = {};
	gs_sampler_state *cur_samplers[GS_MAX_TEXTURES] = {};
	gs_rect viewport = {};
	gs_rect scissor = {};
	bool scissor_enabled = false;
	bool framebuffer_srgb = false;
	matrix4 proj;
	std::vector<matrix4> proj_stack;
	VkSampler default_sampler = VK_NULL_HANDLE; // for textures whose shader names no sampler_state
	// Colour view of the open dynamic rendering instance (VK_NULL_HANDLE: none). See vk_end_rendering.
	VkImageView rendering_view = VK_NULL_HANDLE;
	VkImageView rendering_depth = VK_NULL_HANDLE; // its depth view (VK_NULL_HANDLE: none)

	gs_device() { matrix4_identity(&proj); }
};

struct gs_texture {
	gs_device *device;
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;      // UNORM (or the format itself)
	VkImageView srgb_view = VK_NULL_HANDLE; // only for gs_is_srgb_format formats
	// GS_RENDER_TARGET only: mip 0, identity swizzle (BGRX's sampled views force A to one, an attachment must not).
	VkImageView rt_view = VK_NULL_HANDLE, rt_srgb_view = VK_NULL_HANDLE;
	VkFormat rt_format = VK_FORMAT_UNDEFINED, rt_srgb_format = VK_FORMAT_UNDEFINED;
	uint32_t width, height, levels;
	uint32_t depth = 1;                        // GS_TEXTURE_3D only
	enum gs_texture_type type = GS_TEXTURE_2D; // 2D or 3D (volume)
	gs_color_format format;
	// GS_DYNAMIC only: persistently mapped staging buffer for gs_texture_map.
	VkBuffer staging = VK_NULL_HANDLE;
	VmaAllocation staging_alloc = VK_NULL_HANDLE;
	void *staging_mapped = nullptr;
	uint64_t upload_value = 0; // graphics timeline value of the submission holding the last unmap copy; 0: none
};

// Depth/stencil buffer: one image and one attachment view, never sampled (as D3D11's DSV-only texture).
struct gs_zstencil_buffer {
	gs_device *device;
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkImageAspectFlags aspect = 0; // DEPTH, plus STENCIL for formats that have it
	uint32_t width, height;
};

struct gs_stage_surface {
	gs_device *device;
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	void *mapped = nullptr; // persistently mapped
	uint32_t width, height, linesize;
	gs_color_format format;
	uint64_t copy_value = 0; // graphics timeline value of the submission holding the last copy; 0: never staged
};

struct gs_swap_chain {
	gs_device *device;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE; // null while the window is 0x0 or minimized
	VkSurfaceFormatKHR surface_format = {};
	uint32_t cx = 0, cy = 0; // requested size
	std::vector<VkImage> images;
	std::vector<VkImageView> views;        // per image; the target's rt_view while it is acquired
	std::vector<VkSemaphore> present_sems; // per image: reusable once that image is acquired again
	gs_texture target;                     // `image` is the acquired image, null when none is
	uint32_t image = UINT32_MAX;           // acquired index
	bool recreate = false;                 // out of date, suboptimal or after a failed submit
	enum gs_color_space space = GS_CS_SRGB;
	bool test_not_ready = false; // test hook: acquire behaves as VK_NOT_READY
};

struct gs_sampler_state {
	gs_device *device;
	VkSampler sampler = VK_NULL_HANDLE;
	gs_sampler_info info;
};

struct vk_buffer {
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE; // null for arena space
	VkDeviceSize offset = 0;
	uint32_t stride = 0;
};

// Static data lives in host-visible buffers written once at creation. GS_DYNAMIC data is copied into the recording
// frame's upload arena on its first draw in each frame and after each flush, so a flush never waits for the GPU.
struct gs_vertex_buffer {
	gs_device *device;
	gs_vb_data *data; // owned
	bool dynamic;
	bool dirty = true;
	// points, normals, tangents, colors, then one per tvarray entry; buffer is null when the data has none.
	std::vector<vk_buffer> attrs;
	uint64_t upload_value = 0; // dynamic: graphics.next_value of the frame attrs point into
};

struct gs_index_buffer {
	gs_device *device;
	enum gs_index_type type;
	void *indices; // owned (bfree)
	size_t num;
	bool dynamic;
	bool dirty = true;
	vk_buffer buf;
	uint64_t upload_value = 0;
};

struct gs_shader_param {
	gs_shader *shader;
	std::string name;
	enum gs_shader_param_type type;
	int array_count;               // 0: not an array
	std::vector<uint8_t> def;      // default value as the effect parser stored it
	uint32_t offset = UINT32_MAX;  // std140 offset in the uniform block; UINT32_MAX for textures
	uint32_t stride = 0;           // array element stride in the block
	uint32_t binding = 0;          // textures: binding (combined image sampler) in the shader's set
	size_t sampler = SIZE_MAX;     // textures: index into gs_shader::samplers it is sampled with, if any
	gs_texture *texture = nullptr; // textures: bound by gs_shader_set_texture / set_val
	bool srgb = false;
	gs_samplerstate_t *next_sampler = nullptr;
	bool warned = false;          // unsupported type, or (textures) unset at a draw, already logged once
	bool feedback_warned = false; // textures: sampled while bound as the render target, already logged once
};

struct gs_shader {
	gs_device *device;
	enum gs_shader_type type;
	std::vector<uint32_t> spirv;
	std::vector<gs_shader_param> params; // never resized after creation; gs_sparam_t points into it
	uint32_t set = 0;                    // descriptor set: 0 vertex, 1 pixel
	std::vector<uint8_t> uniforms;       // CPU shadow of the std140 block at binding 0 of `set`; empty when none
	bool dirty = true;                   // uniforms changed since the last upload (device_draw clears it)
	int max_varying = -1;                // highest varying location the shader declares; -1: none
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE; // created with the first pipeline using the shader
	std::vector<VkSampler> vk_samplers;                // per `samplers` entry, created by the first draw
	// Last upload of `uniforms` into an upload arena, reused while not dirty in the same frame.
	VkBuffer ubo = VK_NULL_HANDLE;
	VkDeviceSize ubo_offset = 0;
	uint64_t ubo_value = 0;                // graphics.next_value of the frame it was written into
	std::vector<std::string> inputs;       // vertex shaders: semantic per input location
	std::vector<gs_sampler_info> samplers; // the shader's sampler_state blocks
	gs_shader_param *viewproj = nullptr, *world = nullptr;
};

// HLSL (as emitted by the effect parser) -> Vulkan GLSL -> SPIR-V. Empty on failure, with `error` set.
// When `shader` is given, its params, uniform block size, inputs and samplers are filled from the same parse.
// C++ exceptions become errors. Inputs known to crash the shared parser (recursive or very deep #defines, bracket
// nesting over 256) are rejected before it runs; any other crash inside it or glslang is fatal (logged on Windows).
std::vector<uint32_t> hlsl_to_spirv(const char *hlsl, enum gs_shader_type type, std::string &error,
				    gs_shader *shader = nullptr, const char *file = nullptr);
// glslang process lifetime: device_create takes a reference, device_destroy drops it; compiles hold their own.
void vk_glslang_ref();
void vk_glslang_unref();

// VK_FORMAT_UNDEFINED for formats the backend does not support.
VkFormat vk_format(enum gs_color_format format);

// Command buffer of the frame being recorded, outside any rendering (ends an open one); begins a frame if none is.
// Every recording outside a draw goes through this, which is what ends dynamic rendering (see vk_end_rendering).
VkCommandBuffer vk_cmd(gs_device *d);
// The same without ending rendering; only device_draw uses it.
VkCommandBuffer vk_frame_cmd(gs_device *d);
// Ends the open dynamic rendering instance, if any.
void vk_end_rendering(gs_device *d);
// Copies `data` into the recording frame's upload arena (begins a frame if none is); buf.buffer/offset point at it.
// A full arena grows by a new chunk; never waits. False when out of memory.
bool vk_arena_upload(gs_device *d, const void *data, VkDeviceSize size, VkDeviceSize align, vk_buffer &buf);
// GS_DYNAMIC buffers: copies the CPU data into the recording frame's arena unless that is already done and current.
bool vk_upload_dynamic(gs_device *d, gs_vertex_buffer *vb);
bool vk_upload_dynamic(gs_device *d, gs_index_buffer *ib);
// Host-visible, persistently mapped buffer.
bool vk_create_host_buffer(gs_device *d, VkDeviceSize size, VkBufferUsageFlags usage, VmaAllocationCreateFlags access,
			   VkBuffer &buffer, VmaAllocation &alloc, void *&mapped);
// Device teardown (GPU idle): destroys pipelines, layouts and the default sampler.
void vk_pipelines_free(gs_device *d);
VkSampler vk_create_sampler(gs_device *d, const gs_sampler_info &info);
// Submits the frame being recorded, if any, waiting on a pending acquire and signalling `signal` (binary).
// False when the submission failed; `signal` is then not signalled.
bool vk_flush(gs_device *d, VkSemaphore *signal = nullptr);
// Error path: waits on `sem`, an acquire's semaphore whose submission failed, so it can go back to spare_sems.
void vk_consume_semaphore(gs_device *d, VkSemaphore sem);
// Records b as one vkCmdPipelineBarrier2; nothing when empty.
void vk_record_barriers(VkCommandBuffer cmd, const vk_barriers &b);

// Creates the instance, picks adapter `adapter`, creates device, queues and allocator.
// Returns GS_SUCCESS, GS_ERROR_FAIL (bad adapter) or GS_ERROR_NOT_SUPPORTED; leaks nothing on failure.
int vk_device_init(gs_device *d, uint32_t adapter);
void vk_device_free(gs_device *d);

// Name and driver version of the adapter chosen by the last successful vk_device_init.
const char *vk_adapter_name();
const char *vk_driver_version();

extern "C" {
// Test hook: validation messages since the last vk_device_init, or -1 when no validation layer is active.
EXPORT int obs_vulkan_validation_messages(void);
// Test hook: device_stage_texture for mip `level`; dst must have that level's size.
EXPORT void obs_vulkan_test_stage_mip(gs_device_t *device, gs_stagesurf_t *dst, gs_texture_t *src, uint32_t level);
// Test hook: glslang process references currently held (devices plus running compiles).
EXPORT int obs_vulkan_glslang_refs(void);
}
