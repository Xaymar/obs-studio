#pragma once

#include <vulkan/vulkan.h>

#include <functional>
#include <vector>

struct vk_queue {
	VkQueue q = VK_NULL_HANDLE;
	uint32_t family = 0;
	VkSemaphore timeline = VK_NULL_HANDLE;
	uint64_t next_value = 1;
	std::vector<uint64_t> failed_values; // timeline values whose submission failed (signalled from the host)
	float priority = 0.5f;               // granted: 0.25 low, 0.5 medium (default), 0.75 high, 1.0 realtime
};

struct vk_frame {
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkSemaphore done_sem = VK_NULL_HANDLE; // timeline the last submit signals
	uint64_t done_value = 0;               // 0 when not in flight
	std::vector<std::function<void()>> deferred;

	// Runs fn once the GPU has finished this frame's submission.
	void defer_destroy(std::function<void()> fn) { deferred.push_back(std::move(fn)); }
};

struct vk_frame_ring {
	static constexpr uint32_t count = 3;

	VkDevice dev = VK_NULL_HANDLE;
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
