#include "vulkan-frame.hpp"

#include <util/base.h>

static void run_deferred(vk_frame &f)
{
	for (auto &fn : f.deferred)
		fn();
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
		if (vkCreateCommandPool(dev, &pi, nullptr, &f.pool) != VK_SUCCESS)
			return false;

		VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ai.commandPool = f.pool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		if (vkAllocateCommandBuffers(dev, &ai, &f.cmd) != VK_SUCCESS)
			return false;
	}
	return true;
}

void vk_frame_ring::destroy()
{
	if (!dev)
		return;
	vkDeviceWaitIdle(dev);
	for (auto &f : frames) {
		run_deferred(f);
		if (f.pool)
			vkDestroyCommandPool(dev, f.pool, nullptr);
		f = {};
	}
	dev = VK_NULL_HANDLE;
	index = 0;
}

vk_frame &vk_frame_ring::begin()
{
	vk_frame &f = frames[index];

	if (f.done_value) {
		VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
		wi.semaphoreCount = 1;
		wi.pSemaphores = &f.done_sem;
		wi.pValues = &f.done_value;
		vkWaitSemaphores(dev, &wi, UINT64_MAX);
		f.done_value = 0;
	}
	run_deferred(f);
	vkResetCommandPool(dev, f.pool, 0);

	VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(f.cmd, &bi);
	return f;
}

void vk_frame_ring::submit(vk_queue &q, VkSemaphore *wait, VkSemaphore *signal)
{
	vk_frame &f = frames[index];
	vkEndCommandBuffer(f.cmd);

	const uint64_t value = q.next_value++;

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

	if (vkQueueSubmit2(q.q, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] vkQueueSubmit2 failed");
		return;
	}
	f.done_sem = q.timeline;
	f.done_value = value;
	index = (index + 1) % count;
}
