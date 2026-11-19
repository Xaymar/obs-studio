// Spike 1: render into NV12 / P010 planes through per-plane single-plane views.
//
// For each GPU: create VK_FORMAT_G8_B8R8_2PLANE_420_UNORM and
// VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 images with MUTABLE_FORMAT + EXTENDED_USAGE,
// create R8/R8G8 (resp. R16/R16G16) views of PLANE_0/PLANE_1, render a known pattern into each plane
// view as a colour attachment (both via loadOp=CLEAR + vkCmdClearAttachments and via real draws),
// copy every plane back to a buffer and verify every texel.
#include "../common/rect_pipeline.hpp"

using namespace spike;

namespace {

constexpr uint32_t W = 256, H = 128;

struct rect {
	int x0, y0, x1, y1;
	uint16_t c[2];
};

struct plane_desc {
	VkImageAspectFlagBits aspect;
	VkFormat view_format;
	uint32_t w, h, comps, bpc; // bytes per component
	uint16_t bg[2];
	rect rects[3];
};

struct format_case {
	const char *name;
	VkFormat format;
	plane_desc planes[2];
};

// Values in the storage unit of the plane (8-bit for NV12, 16-bit word with 10 MSBs for P010).
format_case make_nv12()
{
	format_case f{"NV12 (G8_B8R8_2PLANE_420)", VK_FORMAT_G8_B8R8_2PLANE_420_UNORM};
	f.planes[0] = {VK_IMAGE_ASPECT_PLANE_0_BIT, VK_FORMAT_R8_UNORM, W, H, 1, 1, {16, 0},
		       {{8, 8, 72, 40, {235, 0}}, {100, 20, 180, 100, {128, 0}}, {200, 64, 256, 128, {81, 0}}}};
	f.planes[1] = {VK_IMAGE_ASPECT_PLANE_1_BIT, VK_FORMAT_R8G8_UNORM, W / 2, H / 2, 2, 1, {128, 128},
		       {{4, 4, 36, 20, {90, 240}}, {50, 10, 90, 50, {54, 34}}, {100, 32, 128, 64, {240, 110}}}};
	return f;
}

format_case make_p010()
{
	format_case f{"P010 (G10X6_B10X6R10X6_2PLANE_420_3PACK16)", VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16};
	auto s = [](int v10) { return (uint16_t)(v10 << 6); };
	f.planes[0] = {VK_IMAGE_ASPECT_PLANE_0_BIT, VK_FORMAT_R16_UNORM, W, H, 1, 2, {s(64), 0},
		       {{8, 8, 72, 40, {s(940), 0}}, {100, 20, 180, 100, {s(512), 0}}, {200, 64, 256, 128, {s(324), 0}}}};
	f.planes[1] = {VK_IMAGE_ASPECT_PLANE_1_BIT, VK_FORMAT_R16G16_UNORM, W / 2, H / 2, 2, 2, {s(512), s(512)},
		       {{4, 4, 36, 20, {s(360), s(960)}}, {50, 10, 90, 50, {s(216), s(136)}},
			{100, 32, 128, 64, {s(960), s(440)}}}};
	return f;
}

float to_float(uint16_t v, uint32_t bpc)
{
	return bpc == 1 ? v / 255.0f : v / 65535.0f;
}

uint16_t expected(const plane_desc &p, int x, int y, int c)
{
	uint16_t v = p.bg[c];
	for (auto &r : p.rects)
		if (x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1)
			v = r.c[c];
	return v;
}

enum class method { clear, draw };

struct run_result {
	bool ok = false;
	std::string note;
};

run_result run_case(device &d, const format_case &fc, method m, bool use_format_list)
{
	run_result rr;
	VkImage img = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	VkImageView views[2] = {};
	rect_pipeline pipes[2];
	buffer rb;
	try {
		VkFormat list[3] = {fc.format, fc.planes[0].view_format, fc.planes[1].view_format};
		VkImageFormatListCreateInfo fl{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
		fl.viewFormatCount = 3;
		fl.pViewFormats = list;

		VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.pNext = use_format_list ? &fl : nullptr;
		ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = fc.format;
		ii.extent = {W, H, 1};
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
			   VK_IMAGE_USAGE_SAMPLED_BIT;
		ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VKC(d.t.vkCreateImage(d.dev, &ii, nullptr, &img));
		VkMemoryRequirements mr;
		d.t.vkGetImageMemoryRequirements(d.dev, img, &mr);
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = d.find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VKC(d.t.vkAllocateMemory(d.dev, &ai, nullptr, &mem));
		VKC(d.t.vkBindImageMemory(d.dev, img, mem, 0));

		for (int p = 0; p < 2; p++) {
			const plane_desc &pd = fc.planes[p];
			VkImageViewUsageCreateInfo vu{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
			vu.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			vi.pNext = &vu;
			vi.image = img;
			vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
			vi.format = pd.view_format;
			vi.subresourceRange = {(VkImageAspectFlags)pd.aspect, 0, 1, 0, 1};
			VKC(d.t.vkCreateImageView(d.dev, &vi, nullptr, &views[p]));
			if (m == method::draw)
				pipes[p].create(d, pd.view_format);
		}

		VkDeviceSize sz0 = (VkDeviceSize)fc.planes[0].w * fc.planes[0].h * fc.planes[0].comps * fc.planes[0].bpc;
		VkDeviceSize sz1 = (VkDeviceSize)fc.planes[1].w * fc.planes[1].h * fc.planes[1].comps * fc.planes[1].bpc;
		rb.create_host(d, sz0 + sz1, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
		memset(rb.map, 0xCD, (size_t)rb.size);

		VkCommandBuffer cb = d.begin();
		image_barrier(d, cb, img, VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT,
			      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			      VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
			      VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
		for (int p = 0; p < 2; p++) {
			const plane_desc &pd = fc.planes[p];
			VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
			att.imageView = views[p];
			att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
			att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
			att.clearValue.color.float32[0] = to_float(pd.bg[0], pd.bpc);
			att.clearValue.color.float32[1] = to_float(pd.bg[1], pd.bpc);
			att.clearValue.color.float32[2] = 0;
			att.clearValue.color.float32[3] = 1;
			VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
			ri.renderArea = {{0, 0}, {pd.w, pd.h}};
			ri.layerCount = 1;
			ri.colorAttachmentCount = 1;
			ri.pColorAttachments = &att;
			d.t.vkCmdBeginRendering(cb, &ri);
			for (auto &r : pd.rects) {
				float col[4] = {to_float(r.c[0], pd.bpc), to_float(r.c[1], pd.bpc), 0, 1};
				if (m == method::clear) {
					VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0};
					memcpy(ca.clearValue.color.float32, col, sizeof col);
					VkClearRect cr{{{r.x0, r.y0}, {(uint32_t)(r.x1 - r.x0), (uint32_t)(r.y1 - r.y0)}}, 0, 1};
					d.t.vkCmdClearAttachments(cb, 1, &ca, 1, &cr);
				} else {
					pipes[p].draw(d, cb, pd.w, pd.h, r.x0, r.y0, r.x1, r.y1, col);
				}
			}
			d.t.vkCmdEndRendering(cb);
		}
		image_barrier(d, cb, img, VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT,
			      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			      VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
		VkBufferImageCopy cp[2]{};
		cp[0].bufferOffset = 0;
		cp[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
		cp[0].imageExtent = {fc.planes[0].w, fc.planes[0].h, 1};
		cp[1].bufferOffset = sz0;
		cp[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
		cp[1].imageExtent = {fc.planes[1].w, fc.planes[1].h, 1};
		d.t.vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 2, cp);
		VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
		bb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
		bb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		bb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
		bb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
		bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		bb.buffer = rb.buf;
		bb.size = VK_WHOLE_SIZE;
		VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.bufferMemoryBarrierCount = 1;
		di.pBufferMemoryBarriers = &bb;
		d.t.vkCmdPipelineBarrier2(cb, &di);
		d.submit_wait(cb);

		// Verify.
		int bad = 0, bad_lowbits_only = 0;
		std::string first_bad;
		const uint8_t *base = (const uint8_t *)rb.map;
		for (int p = 0; p < 2; p++) {
			const plane_desc &pd = fc.planes[p];
			const uint8_t *pl = base + (p ? sz0 : 0);
			for (uint32_t y = 0; y < pd.h; y++)
				for (uint32_t x = 0; x < pd.w; x++)
					for (uint32_t c = 0; c < pd.comps; c++) {
						size_t idx = ((size_t)y * pd.w + x) * pd.comps + c;
						uint16_t got = pd.bpc == 1 ? pl[idx] : ((const uint16_t *)pl)[idx];
						uint16_t exp = expected(pd, x, y, c);
						if (got != exp) {
							if (pd.bpc == 2 && (got & 0xFFC0) == (exp & 0xFFC0))
								bad_lowbits_only++;
							else {
								if (!bad) {
									char b[160];
									snprintf(b, sizeof b,
										 "first mismatch plane%d (%u,%u) c%u got %u exp %u",
										 p, x, y, c, got, exp);
									first_bad = b;
								}
								bad++;
							}
						}
					}
		}
		rr.ok = bad == 0;
		char b[256];
		snprintf(b, sizeof b, "%d bad texels%s%s; %d differ only in the 6 padding LSBs", bad,
			 bad ? ", " : "", first_bad.c_str(), bad_lowbits_only);
		rr.note = b;
	} catch (check_failed &e) {
		rr.ok = false;
		rr.note = "exception: " + e.what;
	}
	d.t.vkDeviceWaitIdle(d.dev);
	rb.destroy(d);
	for (int p = 0; p < 2; p++) {
		pipes[p].destroy(d);
		if (views[p])
			d.t.vkDestroyImageView(d.dev, views[p], nullptr);
	}
	if (img)
		d.t.vkDestroyImage(d.dev, img, nullptr);
	if (mem)
		d.t.vkFreeMemory(d.dev, mem, nullptr);
	return rr;
}

void report_format_support(VkPhysicalDevice pd, const format_case &fc)
{
	VkFormatProperties3 fp3{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
	VkFormatProperties2 fp{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
	fp.pNext = &fp3;
	vkGetPhysicalDeviceFormatProperties2(pd, fc.format, &fp);
	log("  %s optimalTilingFeatures: %s", fc.name, fmt_features_str(fp.formatProperties.optimalTilingFeatures));
	log("  %s linearTilingFeatures:  %s", fc.name, fmt_features_str(fp.formatProperties.linearTilingFeatures));
	for (auto &p : fc.planes) {
		VkFormatProperties pf;
		vkGetPhysicalDeviceFormatProperties(pd, p.view_format, &pf);
		log("    plane view format %d optimal: %s", (int)p.view_format, fmt_features_str(pf.optimalTilingFeatures));
	}
	VkPhysicalDeviceImageFormatInfo2 ifi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
	ifi.format = fc.format;
	ifi.type = VK_IMAGE_TYPE_2D;
	ifi.tiling = VK_IMAGE_TILING_OPTIMAL;
	ifi.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ifi.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
	VkImageFormatProperties2 ifp{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
	VkResult r = vkGetPhysicalDeviceImageFormatProperties2(pd, &ifi, &ifp);
	log("    vkGetPhysicalDeviceImageFormatProperties2(MUTABLE|EXTENDED_USAGE, COLOR_ATTACHMENT|TRANSFER_SRC|SAMPLED) "
	    "-> %s, maxExtent %ux%u",
	    vkres(r), ifp.imageFormatProperties.maxExtent.width, ifp.imageFormatProperties.maxExtent.height);
}

} // namespace

int main()
{
	results res;
	instance in;
	try {
		in.create("spike1-plane-views");
	} catch (check_failed &) {
		return 2;
	}
	for (VkPhysicalDevice pd : in.physical_devices()) {
		gpu_info g = query_gpu(pd);
		if (g.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU || skip_gpu(g.props.deviceName))
			continue;
		log("\n================================================================");
		print_gpu(g);
		format_case cases[2] = {make_nv12(), make_p010()};
		for (auto &fc : cases)
			report_format_support(pd, fc);
		device d;
		try {
			d.create(pd, {});
		} catch (check_failed &) {
			res.add(g.props.deviceName, "device creation", false, "vkCreateDevice failed");
			continue;
		}
		for (auto &fc : cases)
			for (method m : {method::clear, method::draw})
				for (bool fl : {true, false}) {
					std::string name = std::string(fc.format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM ? "NV12" : "P010") +
							   (m == method::clear ? " clear" : " draw") +
							   (fl ? " +formatlist" : " no-formatlist");
					log("  -- %s", name.c_str());
					vvl_scope vs;
					run_result r = run_case(d, fc, m, fl);
					log("     %s: %s", r.ok ? "PASS" : "FAIL", r.note.c_str());
					res.add(g.props.deviceName, name, r.ok, r.note, vs.errors());
				}
		d.destroy();
	}
	in.destroy();
	res.print();
	return res.fails ? 1 : 0;
}
