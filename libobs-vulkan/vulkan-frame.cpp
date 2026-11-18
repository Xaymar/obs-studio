#include "vulkan-frame.hpp"

#include <util/base.h>

static void reset_transient(VkDevice dev, vk_frame &f)
{
	// shortcut: a mapping outstanding in this slot (held across vk_frame_ring::count submissions, or a failed one)
	// keeps the whole arena from being reused until the next begin(); it grows meanwhile. No libobs caller holds one.
	if (!f.maps) {
		for (vk_arena_chunk &c : f.arena) {
			c.used = 0;
		}
		f.arena_chunk = 0;
	}
	for (VkDescriptorPool p : f.desc_pools) {
		vkResetDescriptorPool(dev, p, 0);
	}
	f.desc_pool = 0;
}

static void run_deferred(vk_frame &f)
{
	for (auto &fn : f.deferred) {
		fn();
	}
	f.deferred.clear();
}

bool vk_frame_ring::init(VkDevice device, uint32_t queue_family)
{
	dev = device;
	family = queue_family;
	for (auto &f : frames) {
		VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
		pi.queueFamilyIndex = family;
		if (vkCreateCommandPool(dev, &pi, nullptr, &f.pool) != VK_SUCCESS) {
			return false;
		}

		VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ai.commandPool = f.pool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(dev, &ai, &f.cmd) != VK_SUCCESS) {
			return false;
		}
	}
	return true;
}

void vk_frame_ring::destroy()
{
	if (!dev) {
		return;
	}
	vkDeviceWaitIdle(dev);
	for (auto &f : frames) {
		run_deferred(f);
		if (f.pool) {
			vkDestroyCommandPool(dev, f.pool, nullptr);
		}
		for (vk_arena_chunk &c : f.arena) {
			vmaDestroyBuffer(vma, c.buffer, c.alloc);
		}
		for (VkDescriptorPool p : f.desc_pools) {
			vkDestroyDescriptorPool(dev, p, nullptr);
		}
		f = {};
	}
	dev = VK_NULL_HANDLE;
	index = 0;
	recording = false;
}

vk_frame &vk_frame_ring::begin()
{
	vk_frame &f = frames[index];
	if (recording) {
		blog(LOG_ERROR, "[libobs-vulkan] begin() while a frame is recording");
		return f;
	}

	if (f.done_value) {
		VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
		wi.semaphoreCount = 1;
		wi.pSemaphores = &f.done_sem;
		wi.pValues = &f.done_value;
		vkWaitSemaphores(dev, &wi, UINT64_MAX);
		f.done_value = 0;
	}
	run_deferred(f);
	reset_transient(dev, f);
	vkResetCommandPool(dev, f.pool, 0);

	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(f.cmd, &bi);
	recording = true;
	return f;
}

void vk_frame_ring::defer_destroy(std::function<void()> fn)
{
	if (recording) {
		frames[index].defer_destroy(std::move(fn));
		return;
	}
	// The most recent submission is the previous slot's; its begin() waits for it before running fn.
	vk_frame &last = frames[(index + count - 1) % count];
	if (last.done_value) {
		last.defer_destroy(std::move(fn));
	} else {
		fn(); // nothing in flight
	}
}

bool vk_frame_ring::submit(vk_queue &q, VkSemaphore *wait, VkSemaphore *signal)
{
	vk_frame &f = frames[index];
	if (!recording) {
		blog(LOG_ERROR, "[libobs-vulkan] submit() without begin()");
		return false;
	}
	recording = false;
	vkEndCommandBuffer(f.cmd);

	const uint64_t value = q.next_value;

	VkSemaphoreSubmitInfo waits[1] = {{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
	VkSemaphoreSubmitInfo signals[2] = {{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO},
					    {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
	uint32_t nsignal = 0;

	signals[nsignal].semaphore = q.timeline;
	signals[nsignal].value = value;
	signals[nsignal++].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	if (signal) {
		signals[nsignal].semaphore = *signal;
		signals[nsignal++].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	}
	if (wait) {
		waits[0].semaphore = *wait;
		waits[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	}

	VkCommandBufferSubmitInfo ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
	ci.commandBuffer = f.cmd;

	VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
	si.waitSemaphoreInfoCount = wait ? 1 : 0;
	si.pWaitSemaphoreInfos = waits;
	si.commandBufferInfoCount = 1;
	si.pCommandBufferInfos = &ci;
	si.signalSemaphoreInfoCount = nsignal;
	si.pSignalSemaphoreInfos = signals;

	VkResult res = test_fail_submit ? VK_ERROR_OUT_OF_HOST_MEMORY : vkQueueSubmit2(q.q, 1, &si, VK_NULL_HANDLE);
	if (res != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] vkQueueSubmit2 failed (%d)", (int)res);
		// Resources deferred here may still be used by earlier submissions; the slot itself has none in flight.
		vkQueueWaitIdle(q.q);
		// Signal the value from the host so no waiter hangs, and record that its work never ran.
		VkSemaphoreSignalInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
		sig.semaphore = q.timeline;
		sig.value = value;
		vkSignalSemaphore(dev, &sig);
		q.next_value = value + 1;
		q.failed_values.push_back(value);
		run_deferred(f);
		return false;
	}
	q.next_value = value + 1;
	f.done_sem = q.timeline;
	f.done_value = value;
	index = (index + 1) % count;
	return true;
}
