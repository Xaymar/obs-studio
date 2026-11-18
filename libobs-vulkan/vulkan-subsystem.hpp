#pragma once

#include <graphics/graphics.h>

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include "vulkan-frame.hpp"

struct gs_device {
	VkInstance instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice dev = VK_NULL_HANDLE;
	vk_queue graphics;
	vk_queue transfer; // q == VK_NULL_HANDLE when the device has no distinct transfer family
	VmaAllocator vma = VK_NULL_HANDLE;
	vk_frame_ring frames;
};

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
