#include "vulkan-tracker.hpp"

// shortcut: omits acceleration-structure and video write bits, upgrade when those are used.
static constexpr VkAccessFlags2 write_bits =
	VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
	VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT |
	VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

static bool is_write(const vk_access &a)
{
	return (a.access & write_bits) != 0;
}

template<class T> static uint64_t key(T handle)
{
	return (uint64_t)handle;
}

bool vk_tracker::step(state &s, const vk_access &needed, bool check_layout, source &src)
{
	bool same_layout = !check_layout || s.layout == needed.layout;
	bool write = is_write(needed);
	// Coalesce a read when there is no pending write, or an earlier barrier already made it visible to this use.
	bool covered = s.write_stage == 0 ||
		       ((needed.stage & ~s.visible_stage) == 0 && (needed.access & ~s.visible_access) == 0);
	if (!write && same_layout && covered) {
		s.read_stage |= needed.stage;
		return false;
	}

	// A write waits for the previous write and for every reader since (execution dependency, no access for reads).
	src = {s.write_stage | s.read_stage, s.write_access, s.layout};
	if (write) {
		s.write_stage = needed.stage;
		s.write_access = needed.access & write_bits;
		// The write's own result is visible to nothing yet, not even a read in its own stage.
		s.visible_stage = 0;
		s.visible_access = 0;
		s.read_stage = 0;
	} else {
		// The write stays pending for further readers in other stages.
		if (!same_layout) {
			s.visible_stage = 0;
			s.visible_access = 0;
		}
		s.visible_stage |= needed.stage;
		s.visible_access |= needed.access;
		s.read_stage |= needed.stage;
	}
	s.layout = needed.layout;
	return true;
}

void vk_tracker::use_image(VkImage image, uint32_t, const vk_access &needed, vk_barriers &out)
{
	source src;
	if (!step(images_[key(image)], needed, true, src))
		return;

	VkImageMemoryBarrier2 b = {};
	b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	b.srcStageMask = src.stage ? src.stage : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
	b.srcAccessMask = src.access;
	b.dstStageMask = needed.stage;
	b.dstAccessMask = needed.access;
	b.oldLayout = src.old_layout;
	b.newLayout = needed.layout;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {needed.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
	out.images.push_back(b);
}

void vk_tracker::use_buffer(VkBuffer buffer, uint32_t, const vk_access &needed, vk_barriers &out)
{
	bool first = buffers_.find(key(buffer)) == buffers_.end();
	source src;
	bool barrier = step(buffers_[key(buffer)], needed, false, src);
	if (first || !barrier) // first use is not a hazard
		return;

	VkBufferMemoryBarrier2 b = {};
	b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
	b.srcStageMask = src.stage;
	b.srcAccessMask = src.access;
	b.dstStageMask = needed.stage;
	b.dstAccessMask = needed.access;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.buffer = buffer;
	b.offset = 0;
	b.size = VK_WHOLE_SIZE;
	out.buffers.push_back(b);
}

void vk_tracker::forget(VkImage image)
{
	images_.erase(key(image));
}

void vk_tracker::forget(VkBuffer buffer)
{
	buffers_.erase(key(buffer));
}
