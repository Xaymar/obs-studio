#pragma once

#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

#include <functional>
#include <vector>

struct vk_queue {
	VkQueue q = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkSemaphore timeline = VK_NULL_HANDLE;
	uint64_t next_value = 1;
	std::vector<uint64_t> failed_values; // timeline values whose submission failed (signalled from the host)
	float priority = 0.5f;               // granted: 0.25 low, 0.5 medium (default), 0.75 high, 1.0 realtime
	// VK_KHR_global_priority level asked for, and the one vkCreateDevice accepted (0: none, the driver default).
	VkQueueGlobalPriorityKHR requested = (VkQueueGlobalPriorityKHR)0;
	VkQueueGlobalPriorityKHR global_priority = (VkQueueGlobalPriorityKHR)0;
};

// One host-visible, persistently mapped block of a frame's upload arena.
struct vk_arena_chunk {
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation alloc = VK_NULL_HANDLE;
	uint8_t *mapped = nullptr;
	VkDeviceSize size = 0, used = 0;
};

struct vk_frame {
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkSemaphore done_sem = VK_NULL_HANDLE; // timeline the last submit signals
	uint64_t done_value = 0;               // 0 when not in flight
	std::vector<std::function<void()>> deferred;
	// Uniform and dynamic vertex/index data, and descriptor sets, of draws recorded into this frame; both are
	// reset (not freed) when the slot is reused, i.e. once the GPU has finished with them.
	std::vector<vk_arena_chunk> arena;
	size_t arena_chunk = 0; // chunk new allocations come from
	std::vector<VkDescriptorPool> desc_pools;
	size_t desc_pool = 0; // desc_pools entry new sets come from

	// Runs fn once the GPU has finished this frame's submission.
	void defer_destroy(std::function<void()> fn) { deferred.push_back(std::move(fn)); }
};

struct vk_frame_ring {
	static constexpr uint32_t count = 3;

	VkDevice dev = VK_NULL_HANDLE;
	VmaAllocator vma = VK_NULL_HANDLE; // frees the arenas in destroy()
	uint32_t family = 0;
	vk_frame frames[count];
	uint32_t index = 0;
	bool recording = false; // frames[index] is between begin() and submit()

	bool init(VkDevice dev, uint32_t family);
	void destroy();

	// Waits only if the slot's previous submission is still in flight; returns it with `cmd` recording.
	vk_frame &begin();
	// Ends and submits the current frame on q (same family as init). wait/signal are optional binary semaphores.
	// On failure the queue is drained, the timeline value is signalled from the host and added to q.failed_values,
	// then the frame's deferred work runs.
	bool submit(vk_queue &q, VkSemaphore *wait, VkSemaphore *signal);
	// Runs fn once every submission so far, and the frame being recorded, has finished on the GPU.
	void defer_destroy(std::function<void()> fn);
};
