#pragma once

#include <graphics/graphics.h>

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
}
