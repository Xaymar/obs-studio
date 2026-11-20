// Spike C: dmabuf export from Vulkan and import into a second VkDevice; GBM-allocated dmabuf imported into Vulkan;
// single plane (BGRA) and multi plane (NV12) with explicit modifiers. Lists the modifiers the driver offers.
#include "dmabuf.hpp"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <xf86drm.h>

static const char *modname(uint64_t m)
{
	static char buf[128];
	char *n = drmGetFormatModifierName(m);
	snprintf(buf, sizeof buf, "0x%016llx %s", (unsigned long long)m, n ? n : "?");
	free(n);
	return buf;
}

struct Fmt {
	const char *name;
	VkFormat vk;
	uint32_t fourcc;
	VkImageUsageFlags usage;
};

static std::vector<uint64_t> list_mods(Ctx &c, const Fmt &f, uint32_t w, uint32_t h)
{
	printf("--- modifiers for %s (usage 0x%x, %ux%u)\n", f.name, f.usage, w, h);
	std::vector<uint64_t> usable;
	for (auto &m : query_modifiers(c, f.vk, f.usage, w, h)) {
		printf("  %-56s planes %u  attach %s sampled %s xfer %s/%s  import %s export %s\n", modname(m.mod),
		       m.planes, (m.features & VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT) ? "y" : "n",
		       (m.features & VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT) ? "y" : "n",
		       (m.features & VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT) ? "y" : "n",
		       (m.features & VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT) ? "y" : "n", m.importable ? "y" : "n",
		       m.exportable ? "y" : "n");
		if (m.importable && m.exportable)
			usable.push_back(m.mod);
	}
	return usable;
}

// Vulkan device A exports, device B imports with the explicit modifier/layout A reports.
static void vk_to_vk(Ctx &a, Ctx &b, const Fmt &f, const std::vector<uint64_t> &mods, uint32_t w, uint32_t h,
		     const char *what, const std::vector<PlaneLayout> *explicit_planes = nullptr)
{
	if (mods.empty()) {
		check(false, "%s %s: no exportable+importable modifier", f.name, what);
		return;
	}
	DmaImage src = create_exportable(a, f.vk, w, h, f.usage, mods, explicit_planes);
	printf("  exported %s %ux%u: modifier %s, %zu memory plane(s):", f.name, w, h, modname(src.modifier),
	       src.planes.size());
	for (auto &p : src.planes)
		printf(" [off %llu pitch %llu size %llu]", (unsigned long long)p.offset, (unsigned long long)p.pitch,
		       (unsigned long long)p.size);
	printf("\n");
	upload_and_release(a, src, 1);
	int fd = export_fd(a, src.mem, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
	if (src.modifier == DRM_FORMAT_MOD_LINEAR) {
		// Independent CPU check: mmap the dmabuf itself (bypasses Vulkan on the reading side).
		size_t len = (size_t)lseek(fd, 0, SEEK_END);
		void *p = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
		if (p == MAP_FAILED) {
			printf("  mmap(dmabuf, %zu bytes): %s - CPU cross-check skipped\n", len, strerror(errno));
		} else {
			auto want = make_pattern(f.vk, w, h, 1);
			uint32_t bpp = f.vk == VK_FORMAT_B8G8R8A8_UNORM ? 4 : 1;
			size_t bad = 0;
			for (uint32_t y = 0; y < h; y++)
				bad += memcmp((uint8_t *)p + src.planes[0].offset + y * src.planes[0].pitch,
					      &want[y * w * bpp], w * bpp) != 0;
			if (src.planes.size() > 1)
				for (uint32_t y = 0; y < h / 2; y++)
					bad += memcmp((uint8_t *)p + src.planes[1].offset + y * src.planes[1].pitch,
						      &want[w * h + y * w], w) != 0;
			if (bad && getenv("SPIKE_DUMP")) {
				printf("  dmabuf size %zu; first bytes:", len);
				for (int i = 0; i < 16; i++)
					printf(" %02x", ((uint8_t *)p)[i]);
				printf("\n  expected:      ");
				for (int i = 0; i < 16; i++)
					printf(" %02x", want[i]);
				printf("\n");
				// look for the start of row 0 and row 1 of the expected Y plane
				for (size_t off = 0; off + 16 <= len; off++) {
					if (!memcmp((uint8_t *)p + off, &want[0], 16))
						printf("  row0 pattern found at offset %zu\n", off);
					if (!memcmp((uint8_t *)p + off, &want[w], 16))
						printf("  row1 pattern found at offset %zu\n", off);
				}
			}
			check(bad == 0, "%s %s: CPU mmap of the exported dmabuf matches (%zu bad rows)", f.name, what,
			      bad);
			munmap(p, len);
		}
	}
	DmaImage dst;
	std::string err;
	bool ok = import_dmabuf(b, dst, fd, f.vk, w, h, src.modifier, src.planes, f.usage, &err);
	check(ok, "%s %s: import into second VkDevice (%s)", f.name, what, ok ? "ok" : err.c_str());
	if (ok) {
		size_t bad = acquire_and_verify(b, dst, 1);
		check(bad == 0, "%s %s: pixels match after export -> import (%zu bad bytes)", f.name, what, bad);
		destroy_dma_image(b, dst);
	}
	close(fd);
	destroy_dma_image(a, src);
}

// GBM allocates; Vulkan A imports and writes the pattern; Vulkan B imports the same fd and reads it back.
static void gbm_to_vk(gbm_device *gbm, Ctx &a, Ctx &b, const Fmt &f, const std::vector<uint64_t> &vkmods,
		      uint32_t w, uint32_t h)
{
	printf("--- GBM %s %ux%u\n", f.name, w, h);
	bool fmt_ok = gbm_device_is_format_supported(gbm, f.fourcc, GBM_BO_USE_RENDERING);
	printf("  gbm_device_is_format_supported(%s, RENDERING): %s\n", fourcc_str(f.fourcc), fmt_ok ? "yes" : "no");
	gbm_bo *bo = nullptr;
	if (!vkmods.empty())
		bo = gbm_bo_create_with_modifiers2(gbm, w, h, f.fourcc, vkmods.data(), (unsigned)vkmods.size(),
						   GBM_BO_USE_RENDERING);
	const char *how = "with Vulkan's modifier list";
	if (!bo) {
		printf("  gbm_bo_create_with_modifiers2(RENDERING): %s\n", strerror(errno));
		struct {
			uint32_t flags;
			const char *name;
		} tries[] = {{GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR, "RENDERING|LINEAR"},
			     {GBM_BO_USE_LINEAR, "LINEAR"},
			     {GBM_BO_USE_SCANOUT, "SCANOUT"},
			     {GBM_BO_USE_SCANOUT | GBM_BO_USE_LINEAR, "SCANOUT|LINEAR"},
			     {0, "no flags"}};
		for (auto &t : tries) {
			bo = gbm_bo_create(gbm, w, h, f.fourcc, t.flags);
			printf("  gbm_bo_create(%s): %s\n", t.name, bo ? "ok" : strerror(errno));
			if (bo) {
				how = t.name;
				break;
			}
		}
	}
	check(bo != nullptr, "%s: GBM allocation %s", f.name, how);
	if (!bo)
		return;
	uint64_t mod = gbm_bo_get_modifier(bo);
	int np = gbm_bo_get_plane_count(bo);
	std::vector<PlaneLayout> planes;
	int fd = gbm_bo_get_fd(bo);
	bool same_fd = true;
	for (int i = 0; i < np; i++) {
		planes.push_back({gbm_bo_get_offset(bo, i), 0, gbm_bo_get_stride_for_plane(bo, i)});
		int pfd = gbm_bo_get_fd_for_plane(bo, i);
		struct stat s1, s2;
		fstat(fd, &s1);
		fstat(pfd, &s2);
		same_fd &= s1.st_ino == s2.st_ino;
		close(pfd);
	}
	printf("  bo: modifier %s, %d plane(s), same dmabuf for all planes: %s:", modname(mod), np,
	       same_fd ? "yes" : "no");
	for (auto &p : planes)
		printf(" [off %llu pitch %llu]", (unsigned long long)p.offset, (unsigned long long)p.pitch);
	printf("\n");
	bool in_vk_list = false;
	for (auto m : vkmods)
		in_vk_list |= m == mod;
	if (!in_vk_list)
		printf("  note: GBM modifier is not in Vulkan's importable list for this usage\n");

	DmaImage ia, ib;
	std::string err;
	bool ok = import_dmabuf(a, ia, fd, f.vk, w, h, mod, planes, f.usage, &err);
	check(ok, "%s: GBM bo imported into VkDevice A (%s)", f.name, ok ? "ok" : err.c_str());
	if (ok) {
		upload_and_release(a, ia, 7);
		ok = import_dmabuf(b, ib, fd, f.vk, w, h, mod, planes, f.usage, &err);
		check(ok, "%s: same GBM bo imported into VkDevice B (%s)", f.name, ok ? "ok" : err.c_str());
		if (ok) {
			size_t bad = acquire_and_verify(b, ib, 7);
			check(bad == 0, "%s: GBM bo pixels written by A, read by B (%zu bad bytes)", f.name, bad);
			destroy_dma_image(b, ib);
		}
		// CPU view through GBM (linear only makes sense to compare)
		uint32_t stride = 0;
		void *map_data = nullptr;
		void *p = gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_READ, &stride, &map_data);
		if (p) {
			auto want = make_pattern(f.vk, w, h, 7);
			size_t bad = 0;
			uint32_t bpp = f.vk == VK_FORMAT_B8G8R8A8_UNORM ? 4 : 1;
			for (uint32_t y = 0; y < h; y++)
				bad += memcmp((uint8_t *)p + y * stride, &want[y * w * bpp], w * bpp) != 0;
			check(bad == 0, "%s: gbm_bo_map readback of plane 0 matches (%zu bad rows, stride %u)", f.name,
			      bad, stride);
			gbm_bo_unmap(bo, map_data);
		} else {
			printf("  gbm_bo_map: failed (%s)\n", strerror(errno));
		}
		if (mod == DRM_FORMAT_MOD_LINEAR) {
			size_t len = (size_t)lseek(fd, 0, SEEK_END);
			void *m = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
			if (m == MAP_FAILED) {
				printf("  mmap(GBM dmabuf): %s\n", strerror(errno));
			} else {
				auto want = make_pattern(f.vk, w, h, 7);
				uint32_t bpp = f.vk == VK_FORMAT_B8G8R8A8_UNORM ? 4 : 1;
				size_t bad = 0;
				for (uint32_t y = 0; y < h; y++)
					bad += memcmp((uint8_t *)m + planes[0].offset + y * planes[0].pitch,
						      &want[y * w * bpp], w * bpp) != 0;
				if (planes.size() > 1)
					for (uint32_t y = 0; y < h / 2; y++)
						bad += memcmp((uint8_t *)m + planes[1].offset + y * planes[1].pitch,
							      &want[w * h + y * w], w) != 0;
				check(bad == 0, "%s: CPU mmap of the GBM LINEAR dmabuf matches what Vulkan wrote (%zu bad rows)",
				      f.name, bad);
				munmap(m, len);
			}
		}
		destroy_dma_image(a, ia);
	}
	close(fd);
	gbm_bo_destroy(bo);
}

int main()
{
	const uint32_t W = 256, H = 128;
	Ctx a, b;
	a.init(dmabuf_exts());
	b.init(dmabuf_exts());
	print_driver(a);

	Fmt bgra{"BGRA8 (AR24)", VK_FORMAT_B8G8R8A8_UNORM, DRM_FORMAT_ARGB8888,
		 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT};
	Fmt nv12{"NV12", VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, DRM_FORMAT_NV12,
		 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT};
	auto bm = list_mods(a, bgra, W, H);
	auto nm = list_mods(a, nv12, W, H);

	printf("--- Vulkan export -> Vulkan import\n");
	vk_to_vk(a, b, bgra, bm, W, H, "driver-chosen modifier");
	vk_to_vk(a, b, nv12, nm, W, H, "driver-chosen modifier");
	for (uint64_t m : bm)
		vk_to_vk(a, b, bgra, {m}, W, H, modname(m));
	for (uint64_t m : nm)
		vk_to_vk(a, b, nv12, {m}, W, H, modname(m));
	// Same LINEAR NV12, but the exporter forces a plain pitch layout (Y pitch W at 0, UV pitch W right after).
	std::vector<PlaneLayout> nv12_pitch = {{0, 0, W}, {(uint64_t)W * H, 0, W}};
	vk_to_vk(a, b, nv12, {DRM_FORMAT_MOD_LINEAR}, W, H, "LINEAR, explicit pitch layout on export", &nv12_pitch);

	int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	gbm_device *gbm = drm >= 0 ? gbm_create_device(drm) : nullptr;
	if (!gbm) {
		check(false, "GBM device on /dev/dri/renderD128 (%s)", strerror(errno));
	} else {
		printf("GBM backend: %s\n", gbm_device_get_backend_name(gbm));
		gbm_to_vk(gbm, a, b, bgra, bm, W, H);
		gbm_to_vk(gbm, a, b, nv12, nm, W, H);
		gbm_device_destroy(gbm);
	}
	if (drm >= 0)
		close(drm);

	int rc = finish(a, "spike C");
	b.destroy();
	a.destroy();
	return rc;
}
