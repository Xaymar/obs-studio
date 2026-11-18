#pragma once

#include <graphics/graphics.h>

#include <string>
#include <vector>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "vulkan-frame.hpp"
#include "vulkan-tracker.hpp"

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
	enum gs_color_space space = GS_CS_SRGB;
	bool can_present = false; // VK_KHR_surface, the platform surface and VK_KHR_swapchain are enabled
	gs_swap_chain *swap = nullptr;
	VkSemaphore acquire_wait = VK_NULL_HANDLE; // signalled by the last acquire; the next submit waits on it
	std::vector<VkSemaphore> spare_sems;       // unsignalled binary semaphores for acquires
};

struct gs_texture {
	gs_device *device;
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;      // UNORM (or the format itself)
	VkImageView srgb_view = VK_NULL_HANDLE; // only for gs_is_srgb_format formats
	uint32_t width, height, levels;
	gs_color_format format;
	// GS_DYNAMIC only: persistently mapped staging buffer for gs_texture_map.
	VkBuffer staging = VK_NULL_HANDLE;
	VmaAllocation staging_alloc = VK_NULL_HANDLE;
	void *staging_mapped = nullptr;
	uint64_t upload_value = 0; // graphics timeline value of the submission holding the last unmap copy; 0: none
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
	std::vector<VkSemaphore> present_sems; // per image: reusable once that image is acquired again
	gs_texture target;                     // `image` is the acquired image, null when none is
	uint32_t image = UINT32_MAX;           // acquired index
	bool recreate = false;                 // out of date, suboptimal or after a failed submit
	enum gs_color_space space = GS_CS_SRGB;
	bool test_not_ready = false; // test hook: acquire behaves as VK_NOT_READY
};

struct gs_shader_param {
	gs_shader *shader;
	std::string name;
	enum gs_shader_param_type type;
	int array_count;               // 0: not an array
	std::vector<uint8_t> def;      // default value as the effect parser stored it
	uint32_t offset = UINT32_MAX;  // std140 offset in the uniform block; UINT32_MAX for textures
	uint32_t stride = 0;           // array element stride in the block
	uint32_t binding = 0;          // textures: set 0 binding (combined image sampler)
	size_t sampler = SIZE_MAX;     // textures: index into gs_shader::samplers it is sampled with, if any
	gs_texture *texture = nullptr; // textures: bound by gs_shader_set_texture / set_val
	bool srgb = false;
	gs_samplerstate_t *next_sampler = nullptr;
	bool warned = false; // unsupported type already logged once
};

struct gs_shader {
	gs_device *device;
	enum gs_shader_type type;
	std::vector<uint32_t> spirv;
	std::vector<gs_shader_param> params;   // never resized after creation; gs_sparam_t points into it
	std::vector<uint8_t> uniforms;         // CPU shadow of the set 0 binding 0 std140 block; empty when none
	bool dirty = true;                     // uniforms changed since the last upload (Task 7 clears it)
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

// Command buffer of the frame being recorded; begins one if none is.
VkCommandBuffer vk_cmd(gs_device *d);
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
