#pragma once

#include <stdint.h>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

// Pure bookkeeping: records each resource's last access and emits the synchronization2 barriers the next use needs.
// Calls no Vulkan function.
struct vk_access {
	VkPipelineStageFlags2 stage;
	VkAccessFlags2 access;
	VkImageLayout layout;                                  // ignored for buffers
	VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT; // images: the barrier's aspectMask (all of the image's)
};

struct vk_barriers {
	std::vector<VkImageMemoryBarrier2> images;
	std::vector<VkBufferMemoryBarrier2> buffers;
};

class vk_tracker {
public:
	void use_image(VkImage image, uint32_t family, const vk_access &needed, vk_barriers &out);
	void use_buffer(VkBuffer buffer, uint32_t family, const vk_access &needed, vk_barriers &out);
	void forget(VkImage image);
	void forget(VkBuffer buffer);

private:
	// shortcut: images are tracked whole (every aspect the access names, all mips/layers); no queue family
	// ownership transfers.
	struct state {
		VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
		VkPipelineStageFlags2 write_stage = 0; // source of the last write not yet superseded
		VkAccessFlags2 write_access = 0;
		VkPipelineStageFlags2 visible_stage = 0; // scope already covered by an emitted barrier
		VkAccessFlags2 visible_access = 0;
		VkPipelineStageFlags2 read_stage = 0; // readers since the last write, a later write waits for them
	};
	struct source {
		VkPipelineStageFlags2 stage;
		VkAccessFlags2 access;
		VkImageLayout old_layout;
	};
	// Updates `s` for `needed`; returns true and the barrier source when a barrier is required.
	static bool step(state &s, const vk_access &needed, bool check_layout, source &src);

	std::unordered_map<uint64_t, state> images_;
	std::unordered_map<uint64_t, state> buffers_;
};
