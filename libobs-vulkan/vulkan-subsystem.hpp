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
};

struct gs_texture {
	gs_device *device;
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	uint32_t width, height;
	gs_color_format format;
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

// VK_FORMAT_UNDEFINED for formats the backend does not support.
VkFormat vk_format(enum gs_color_format format);

// Command buffer of the frame being recorded; begins one if none is.
VkCommandBuffer vk_cmd(gs_device *d);
// Submits the frame being recorded, if any. False when the submission failed.
bool vk_flush(gs_device *d);

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
}
