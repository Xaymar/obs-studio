// Spike B: NV12 (G8_B8R8_2PLANE_420) and P010 (G10X6_B10X6R10X6_2PLANE_420_3PACK16) images with MUTABLE_FORMAT +
// EXTENDED_USAGE, per-plane R8/RG8 (R16/RG16) views as colour attachments: clear + draw a pattern, read back, verify.
#include "pipeline.hpp"
#include "fullscreen_vert.h"
#include "pattern_frag.h"

struct Case {
	const char *name;
	VkFormat fmt, yfmt, uvfmt;
	int mode; // 0 = 8 bit, 1 = 10 bit in 16
};

static bool probe(Ctx &c, const Case &k, VkImageUsageFlags usage)
{
	VkFormat list[] = {k.fmt, k.yfmt, k.uvfmt};
	VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
	fl.viewFormatCount = 3;
	fl.pViewFormats = list;
	VkPhysicalDeviceImageFormatInfo2 fi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
	fi.pNext = &fl;
	fi.format = k.fmt;
	fi.type = VK_IMAGE_TYPE_2D;
	fi.tiling = VK_IMAGE_TILING_OPTIMAL;
	fi.usage = usage;
	fi.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	VkImageFormatProperties2 p{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
	VkResult r = vkGetPhysicalDeviceImageFormatProperties2(c.phys, &fi, &p);
	printf("  %s image format props (usage 0x%x): %s, max %ux%u\n", k.name, usage, vk_result_str(r),
	       p.imageFormatProperties.maxExtent.width, p.imageFormatProperties.maxExtent.height);
	VkFormatProperties3 f3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
	VkFormatProperties2 f2{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
	f2.pNext = &f3;
	for (VkFormat f : {k.yfmt, k.uvfmt}) {
		vkGetPhysicalDeviceFormatProperties2(c.phys, f, &f2);
		printf("  view format %d optimal features: colour attachment %s\n", f,
		       (f3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT) ? "yes" : "NO");
	}
	vkGetPhysicalDeviceFormatProperties2(c.phys, k.fmt, &f2);
	printf("  %s optimal features 0x%llx (sampled %s, ycbcr-linear %s, disjoint %s)\n", k.name,
	       (unsigned long long)f3.optimalTilingFeatures,
	       (f3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) ? "yes" : "no",
	       (f3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_MIDPOINT_CHROMA_SAMPLES_BIT) ? "yes" : "no",
	       (f3.optimalTilingFeatures & VK_FORMAT_FEATURE_2_DISJOINT_BIT) ? "yes" : "no");
	return r == VK_SUCCESS;
}

static void run_case(Ctx &c, const Case &k, int clear_mode)
{
	const uint32_t W = 64, H = 48;
	printf("--- %s %ux%u, clear via %s\n", k.name, W, H, clear_mode == 1 ? "vkCmdClearAttachments" : clear_mode == 2 ? "loadOp CLEAR, no draw" : "loadOp CLEAR");
	VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
				  VK_IMAGE_USAGE_SAMPLED_BIT;
	bool ok = probe(c, k, usage);
	check(ok, "%s: MUTABLE|EXTENDED_USAGE image with colour-attachment usage supported", k.name);
	if (!ok)
		return;

	VkFormat list[] = {k.fmt, k.yfmt, k.uvfmt};
	VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
	fl.viewFormatCount = 3;
	fl.pViewFormats = list;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.pNext = &fl;
	ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = k.fmt;
	ici.extent = {W, H, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = usage;
	ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage img;
	VK_CHECK(vkCreateImage(c.dev, &ici, nullptr, &img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(c.dev, img, &mr);
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = c.mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkDeviceMemory mem;
	VK_CHECK(vkAllocateMemory(c.dev, &mai, nullptr, &mem));
	VK_CHECK(vkBindImageMemory(c.dev, img, mem, 0));

	VkImageView views[2];
	VkFormat vf[2] = {k.yfmt, k.uvfmt};
	VkImageAspectFlags asp[2] = {VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_ASPECT_PLANE_1_BIT};
	for (int i = 0; i < 2; i++) {
		VkImageViewUsageCreateInfo vu{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
		vu.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.pNext = &vu;
		vci.image = img;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = vf[i];
		vci.subresourceRange = {asp[i], 0, 1, 0, 1};
		VK_CHECK(vkCreateImageView(c.dev, &vci, nullptr, &views[i]));
	}
	PatternPipeline py = make_pattern_pipeline(c, k.yfmt, fullscreen_vert, sizeof(fullscreen_vert), pattern_frag,
						   sizeof(pattern_frag));
	PatternPipeline puv = make_pattern_pipeline(c, k.uvfmt, fullscreen_vert, sizeof(fullscreen_vert),
						    pattern_frag, sizeof(pattern_frag));

	const uint32_t bpc = k.mode ? 2 : 1; // bytes per channel
	size_t ysize = W * H * bpc, uvsize = (W / 2) * (H / 2) * 2 * bpc;
	Ctx::Buffer rb = c.host_buffer(ysize + uvsize, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	memset(rb.map, 0xCD, rb.size);

	VkRect2D ry{{8, 8}, {32, 24}}, ruv{{4, 4}, {16, 12}};
	VkClearColorValue cy{}, cuv{};
	// limited-range black, neutral chroma
	cy.float32[0] = k.mode ? (64 << 6) / 65535.0f : 16 / 255.0f;
	cuv.float32[0] = cuv.float32[1] = k.mode ? (512 << 6) / 65535.0f : 128 / 255.0f;

	VkCommandBuffer cb = c.begin();
	image_barrier(cb, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
		      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
		      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
	// SPIKE_UV_FIRST=1 renders the chroma plane first (isolates "first rendering after the transition")
	bool uv_first = getenv("SPIKE_UV_FIRST") != nullptr;
	if (uv_first)
		clear_and_draw(cb, puv, views[1], W / 2, H / 2, cuv, ruv, k.mode, clear_mode);
	clear_and_draw(cb, py, views[0], W, H, cy, ry, k.mode, clear_mode);
	if (!uv_first && !getenv("SPIKE_NO_UV"))
		clear_and_draw(cb, puv, views[1], W / 2, H / 2, cuv, ruv, k.mode, clear_mode);
	image_barrier(cb, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
		      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
		      VK_ACCESS_2_TRANSFER_READ_BIT);
	VkBufferImageCopy reg[2] = {};
	reg[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
	reg[0].imageExtent = {W, H, 1};
	reg[1].bufferOffset = ysize;
	reg[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
	reg[1].imageExtent = {W / 2, H / 2, 1};
	vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 2, reg);
	c.submit(cb);

	auto rd = [&](size_t idx) -> uint32_t {
		return bpc == 1 ? ((uint8_t *)rb.map)[idx] : ((uint16_t *)rb.map)[idx];
	};
	auto expect = [&](uint32_t v) { return k.mode ? ((v & 1023) << 6) : (v & 255); };
	int bad_y = 0, bad_uv = 0;
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) {
			uint32_t want = in_rect(ry, x, y) && clear_mode != 2 ? expect(pat_a(x, y)) : (k.mode ? 64u << 6 : 16u);
			uint32_t got = rd(y * W + x);
			if (got != want && bad_y++ < 4)
				printf("  Y(%u,%u) got %u want %u\n", x, y, got, want);
		}
	size_t uvbase = ysize / bpc;
	for (uint32_t y = 0; y < H / 2; y++)
		for (uint32_t x = 0; x < W / 2; x++) {
			bool in = in_rect(ruv, x, y) && clear_mode != 2;
			uint32_t wu = in ? expect(pat_a(x, y)) : (k.mode ? 512u << 6 : 128u);
			uint32_t wv = in ? expect(pat_b(x, y)) : (k.mode ? 512u << 6 : 128u);
			uint32_t gu = rd(uvbase + (y * (W / 2) + x) * 2), gv = rd(uvbase + (y * (W / 2) + x) * 2 + 1);
			if ((gu != wu || gv != wv) && bad_uv++ < 4)
				printf("  UV(%u,%u) got %u,%u want %u,%u\n", x, y, gu, gv, wu, wv);
		}
	if (bad_y && getenv("SPIKE_DUMP")) {
		for (uint32_t y = 0; y < H; y += 4) {
			for (uint32_t x = 0; x < W; x += 2)
				printf("%c", rd(y * W + x) == 0 ? '.' : (rd(y * W + x) == (k.mode ? 64u << 6 : 16u) ? 'c' : 'p'));
			printf("\n");
		}
	}
	check(bad_y == 0, "%s: luma plane via %s view: clear + pattern exact (%d bad texels)", k.name,
	      k.mode ? "R16" : "R8", bad_y);
	check(bad_uv == 0, "%s: chroma plane via %s view: clear + pattern exact (%d bad texels)", k.name,
	      k.mode ? "RG16" : "RG8", bad_uv);

	c.free_buffer(rb);
	destroy_pipeline(c, py);
	destroy_pipeline(c, puv);
	for (auto v : views)
		vkDestroyImageView(c.dev, v, nullptr);
	vkDestroyImage(c.dev, img, nullptr);
	vkFreeMemory(c.dev, mem, nullptr);
}

int main()
{
	Ctx c;
	c.init({});
	print_driver(c);
	for (int cm = 0; cm < 3; cm++) {
		run_case(c, {"NV12", VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, 0}, cm);
		run_case(c, {"P010", VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16, VK_FORMAT_R16_UNORM,
			     VK_FORMAT_R16G16_UNORM, 1}, cm);
	}
	int rc = finish(c, "spike B");
	c.destroy();
	return rc;
}
