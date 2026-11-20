// dmabuf export/import helpers (VK_EXT_external_memory_dma_buf + VK_EXT_image_drm_format_modifier).
#pragma once
#include "vkutil.hpp"

#include <unistd.h>

inline std::vector<const char *> dmabuf_exts()
{
	return {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
		VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
}

struct ModInfo {
	uint64_t mod;
	uint32_t planes;
	VkFormatFeatureFlags2 features;
	bool importable, exportable; // with the usage passed to query_modifiers
};

inline bool is_multiplane(VkFormat f)
{
	return f == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM || f == VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
}

inline std::vector<ModInfo> query_modifiers(Ctx &c, VkFormat fmt, VkImageUsageFlags usage, uint32_t w, uint32_t h)
{
	VkDrmFormatModifierPropertiesList2EXT l{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT};
	VkFormatProperties2 fp{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
	fp.pNext = &l;
	vkGetPhysicalDeviceFormatProperties2(c.phys, fmt, &fp);
	std::vector<VkDrmFormatModifierProperties2EXT> props(l.drmFormatModifierCount);
	l.pDrmFormatModifierProperties = props.data();
	vkGetPhysicalDeviceFormatProperties2(c.phys, fmt, &fp);
	std::vector<ModInfo> out;
	for (auto &p : props) {
		ModInfo m{p.drmFormatModifier, p.drmFormatModifierPlaneCount, p.drmFormatModifierTilingFeatures, false,
			  false};
		VkPhysicalDeviceImageDrmFormatModifierInfoEXT mi{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
		mi.drmFormatModifier = p.drmFormatModifier;
		mi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		VkPhysicalDeviceExternalImageFormatInfo ei{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
		ei.pNext = &mi;
		ei.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		VkPhysicalDeviceImageFormatInfo2 fi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
		fi.pNext = &ei;
		fi.format = fmt;
		fi.type = VK_IMAGE_TYPE_2D;
		fi.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
		fi.usage = usage;
		VkExternalImageFormatProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
		VkImageFormatProperties2 ip{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
		ip.pNext = &ep;
		if (vkGetPhysicalDeviceImageFormatProperties2(c.phys, &fi, &ip) == VK_SUCCESS &&
		    ip.imageFormatProperties.maxExtent.width >= w && ip.imageFormatProperties.maxExtent.height >= h) {
			auto f = ep.externalMemoryProperties.externalMemoryFeatures;
			m.importable = f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
			m.exportable = f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT;
		}
		out.push_back(m);
	}
	return out;
}

struct PlaneLayout {
	uint64_t offset, size, pitch;
};

struct DmaImage {
	VkImage img = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	VkFormat fmt = VK_FORMAT_UNDEFINED;
	uint32_t w = 0, h = 0;
	uint64_t modifier = 0;
	std::vector<PlaneLayout> planes;
};

inline void destroy_dma_image(Ctx &c, DmaImage &d)
{
	vkDestroyImage(c.dev, d.img, nullptr);
	vkFreeMemory(c.dev, d.mem, nullptr);
	d = {};
}

// Exportable image; the driver picks one of `mods`.
// With `explicit_planes`, mods[0] is forced with those plane layouts (ExplicitCreateInfo) instead of a list.
inline DmaImage create_exportable(Ctx &c, VkFormat fmt, uint32_t w, uint32_t h, VkImageUsageFlags usage,
				  const std::vector<uint64_t> &mods,
				  const std::vector<PlaneLayout> *explicit_planes = nullptr)
{
	DmaImage d;
	d.fmt = fmt;
	d.w = w;
	d.h = h;
	VkImageDrmFormatModifierListCreateInfoEXT ml{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
	ml.drmFormatModifierCount = (uint32_t)mods.size();
	ml.pDrmFormatModifiers = mods.data();
	std::vector<VkSubresourceLayout> sl;
	VkImageDrmFormatModifierExplicitCreateInfoEXT me{
		VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
	if (explicit_planes) {
		for (auto &p : *explicit_planes)
			sl.push_back({p.offset, 0, p.pitch, 0, 0});
		me.drmFormatModifier = mods[0];
		me.drmFormatModifierPlaneCount = (uint32_t)sl.size();
		me.pPlaneLayouts = sl.data();
	}
	VkExternalMemoryImageCreateInfo ei{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	ei.pNext = explicit_planes ? (void *)&me : (void *)&ml;
	ei.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.pNext = &ei;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = fmt;
	ici.extent = {w, h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	ici.usage = usage;
	VK_CHECK(vkCreateImage(c.dev, &ici, nullptr, &d.img));
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(c.dev, d.img, &mr);
	VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.image = d.img;
	VkExportMemoryAllocateInfo ex{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
	ex.pNext = &ded;
	ex.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = &ex;
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = c.mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(vkAllocateMemory(c.dev, &mai, nullptr, &d.mem));
	VK_CHECK(vkBindImageMemory(c.dev, d.img, d.mem, 0));
	VkImageDrmFormatModifierPropertiesEXT mp{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
	VK_CHECK(c.dfn<PFN_vkGetImageDrmFormatModifierPropertiesEXT>("vkGetImageDrmFormatModifierPropertiesEXT")(
		c.dev, d.img, &mp));
	d.modifier = mp.drmFormatModifier;
	uint32_t np = 1;
	for (auto &m : query_modifiers(c, fmt, usage, w, h))
		if (m.mod == d.modifier)
			np = m.planes;
	for (uint32_t i = 0; i < np; i++) {
		VkImageSubresource sr{(VkImageAspectFlags)(VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT << i), 0, 0};
		VkSubresourceLayout l;
		vkGetImageSubresourceLayout(c.dev, d.img, &sr, &l);
		d.planes.push_back({l.offset, l.size, l.rowPitch});
	}
	return d;
}

inline int export_fd(Ctx &c, VkDeviceMemory mem, VkExternalMemoryHandleTypeFlagBits type)
{
	VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	gi.memory = mem;
	gi.handleType = type;
	int fd = -1;
	VK_CHECK(c.dfn<PFN_vkGetMemoryFdKHR>("vkGetMemoryFdKHR")(c.dev, &gi, &fd));
	return fd;
}

// Imports a single-fd dmabuf with an explicit modifier and plane layouts. Takes ownership of a dup of `fd`.
inline bool import_dmabuf(Ctx &c, DmaImage &d, int fd, VkFormat fmt, uint32_t w, uint32_t h, uint64_t modifier,
			  const std::vector<PlaneLayout> &planes, VkImageUsageFlags usage, std::string *err = nullptr)
{
	d = {};
	d.fmt = fmt;
	d.w = w;
	d.h = h;
	d.modifier = modifier;
	d.planes = planes;
	std::vector<VkSubresourceLayout> sl;
	for (auto &p : planes)
		sl.push_back({p.offset, 0, p.pitch, 0, 0});
	VkImageDrmFormatModifierExplicitCreateInfoEXT me{
		VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
	me.drmFormatModifier = modifier;
	me.drmFormatModifierPlaneCount = (uint32_t)sl.size();
	me.pPlaneLayouts = sl.data();
	VkExternalMemoryImageCreateInfo ei{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	ei.pNext = &me;
	ei.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.pNext = &ei;
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = fmt;
	ici.extent = {w, h, 1};
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
	ici.usage = usage;
	VkResult r = vkCreateImage(c.dev, &ici, nullptr, &d.img);
	if (r != VK_SUCCESS) {
		if (err)
			*err = std::string("vkCreateImage: ") + vk_result_str(r);
		return false;
	}
	VkMemoryFdPropertiesKHR fp{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
	r = c.dfn<PFN_vkGetMemoryFdPropertiesKHR>("vkGetMemoryFdPropertiesKHR")(
		c.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp);
	if (r != VK_SUCCESS) {
		if (err)
			*err = std::string("vkGetMemoryFdPropertiesKHR: ") + vk_result_str(r);
		vkDestroyImage(c.dev, d.img, nullptr);
		return false;
	}
	VkMemoryRequirements mr;
	vkGetImageMemoryRequirements(c.dev, d.img, &mr);
	VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.image = d.img;
	VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
	imp.pNext = &ded;
	imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	imp.fd = dup(fd);
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = &imp;
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = c.mem_type(mr.memoryTypeBits & fp.memoryTypeBits, 0);
	r = vkAllocateMemory(c.dev, &mai, nullptr, &d.mem);
	if (r != VK_SUCCESS) {
		close(imp.fd);
		if (err)
			*err = std::string("vkAllocateMemory(import): ") + vk_result_str(r);
		vkDestroyImage(c.dev, d.img, nullptr);
		return false;
	}
	VK_CHECK(vkBindImageMemory(c.dev, d.img, d.mem, 0));
	return true;
}

// Test pattern, per plane, in bytes. BGRA: B=x, G=y, R=x^y, A=255. NV12: Y and interleaved UV.
inline uint8_t pat_bgra(uint32_t x, uint32_t y, int ch, uint32_t seed)
{
	switch (ch) {
	case 0:
		return (uint8_t)(x + seed);
	case 1:
		return (uint8_t)(y * 3 + seed);
	case 2:
		return (uint8_t)((x ^ y) + seed);
	default:
		return 255;
	}
}
inline uint8_t pat_y(uint32_t x, uint32_t y, uint32_t seed) { return (uint8_t)(x * 7 + y * 13 + seed); }
inline uint8_t pat_uv(uint32_t x, uint32_t y, int ch, uint32_t seed)
{
	return (uint8_t)(ch ? x * 3 + y * 5 + 100 + seed : x * 11 + y + 50 + seed);
}

// Tightly packed CPU image (BGRA, or NV12 Y then UV).
inline std::vector<uint8_t> make_pattern(VkFormat fmt, uint32_t w, uint32_t h, uint32_t seed)
{
	std::vector<uint8_t> v;
	if (fmt == VK_FORMAT_B8G8R8A8_UNORM) {
		v.resize(w * h * 4);
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++)
				for (int c = 0; c < 4; c++)
					v[(y * w + x) * 4 + c] = pat_bgra(x, y, c, seed);
	} else {
		v.resize(w * h + w * h / 2);
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++)
				v[y * w + x] = pat_y(x, y, seed);
		for (uint32_t y = 0; y < h / 2; y++)
			for (uint32_t x = 0; x < w / 2; x++)
				for (int c = 0; c < 2; c++)
					v[w * h + (y * (w / 2) + x) * 2 + c] = pat_uv(x, y, c, seed);
	}
	return v;
}

inline std::vector<VkBufferImageCopy> plane_copies(VkFormat fmt, uint32_t w, uint32_t h)
{
	std::vector<VkBufferImageCopy> r;
	if (is_multiplane(fmt)) {
		VkBufferImageCopy a{}, b{};
		a.imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
		a.imageExtent = {w, h, 1};
		b.bufferOffset = w * h;
		b.imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
		b.imageExtent = {w / 2, h / 2, 1};
		r = {a, b};
	} else {
		VkBufferImageCopy a{};
		a.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		a.imageExtent = {w, h, 1};
		r = {a};
	}
	return r;
}

// Uploads the pattern and releases the image to VK_QUEUE_FAMILY_FOREIGN_EXT in GENERAL layout.
// `signal` (optional) is signalled by the submit; with it, the call does not wait for the GPU.
inline void upload_and_release(Ctx &c, DmaImage &d, uint32_t seed, VkSemaphoreSubmitInfo *signal = nullptr,
			       uint32_t dst_q = VK_QUEUE_FAMILY_FOREIGN_EXT)
{
	auto data = make_pattern(d.fmt, d.w, d.h, seed);
	static Ctx::Buffer staging; // leaked on purpose: lives until the submit is done in the no-wait case
	staging = c.host_buffer(data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	memcpy(staging.map, data.data(), data.size());
	VkCommandBuffer cb = c.begin();
	image_barrier(cb, d.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
		      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
		      VK_ACCESS_2_TRANSFER_WRITE_BIT);
	auto regs = plane_copies(d.fmt, d.w, d.h);
	vkCmdCopyBufferToImage(cb, staging.buf, d.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regs.size(),
			       regs.data());
	image_barrier(cb, d.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		      VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
		      VK_PIPELINE_STAGE_2_NONE, 0, c.qfam, dst_q);
	if (signal)
		c.submit(cb, {}, {*signal}, false);
	else {
		c.submit(cb);
		c.free_buffer(staging);
	}
}

// Acquires from FOREIGN (GENERAL layout), copies to a buffer, compares with the pattern. Returns bad byte count.
inline size_t acquire_and_verify(Ctx &c, DmaImage &d, uint32_t seed, VkSemaphoreSubmitInfo *wait = nullptr,
				 uint32_t src_q = VK_QUEUE_FAMILY_FOREIGN_EXT)
{
	auto want = make_pattern(d.fmt, d.w, d.h, seed);
	Ctx::Buffer rb = c.host_buffer(want.size(), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	memset(rb.map, 0xEE, want.size());
	VkCommandBuffer cb = c.begin();
	image_barrier(cb, d.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
		      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
		      VK_ACCESS_2_TRANSFER_READ_BIT, src_q, c.qfam);
	auto regs = plane_copies(d.fmt, d.w, d.h);
	vkCmdCopyImageToBuffer(cb, d.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, (uint32_t)regs.size(),
			       regs.data());
	if (wait)
		c.submit(cb, {*wait}, {});
	else
		c.submit(cb);
	size_t bad = 0;
	const uint8_t *got = (const uint8_t *)rb.map;
	for (size_t i = 0; i < want.size(); i++)
		if (got[i] != want[i] && bad++ < 3)
			printf("  byte %zu: got %u want %u\n", i, got[i], want[i]);
	c.free_buffer(rb);
	return bad;
}

inline const char *fourcc_str(uint32_t f)
{
	static char s[5];
	memcpy(s, &f, 4);
	s[4] = 0;
	return s;
}
