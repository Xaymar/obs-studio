// Spike G: VK_KHR_wayland_surface swapchain against a headless compositor. No compositor is installed, so this
// program contains a minimal one (wl_compositor, wl_surface, zwp_linux_dmabuf_v1 v4 with feedback) running on a
// thread. Every committed wl_buffer is imported (dmabuf) into the compositor's own VkDevice and read back, so the
// test checks what the client actually presented. Logs every global the driver's WSI binds.
// With --syncobj the compositor also offers wp_linux_drm_syncobj_manager_v1 (explicit sync): it waits for the
// acquire point before reading and signals the release point after, and the client no longer idles between frames.
#include "dmabuf.hpp"

#include <drm_fourcc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <vulkan/vulkan_wayland.h>

#include "linux-dmabuf-v1-server.h"
#include "linux-drm-syncobj-v1-server.h"

#include <fcntl.h>
#include <xf86drm.h>

#include <atomic>
#include <mutex>
#include <thread>

static const uint32_t W = 256, H = 128, FRAMES = 6;

// ---------------------------------------------------------------- compositor
struct Plane {
	int fd = -1;
	uint32_t offset = 0, stride = 0;
	uint64_t mod = 0;
};
struct Buffer {
	wl_resource *res;
	int32_t w, h;
	uint32_t format;
	std::vector<Plane> planes;
};
struct Params {
	std::vector<Plane> planes = std::vector<Plane>(4);
	bool used = false;
};
struct Point {
	uint32_t handle = 0;
	uint64_t value = 0;
	bool set = false;
};
struct Surface {
	wl_resource *res;
	wl_resource *pending = nullptr, *current = nullptr;
	std::vector<wl_resource *> frame_cbs;
	Point acquire, release; // pending explicit-sync points for the next commit
};

struct Comp {
	wl_display *dpy = nullptr;
	Ctx vk;
	std::vector<std::pair<uint32_t, uint64_t>> formats; // (fourcc, modifier)
	int table_fd = -1;
	size_t table_size = 0;
	dev_t render_dev = 0;
	std::mutex m;
	std::vector<std::string> binds;
	std::vector<uint32_t> seen_colors; // packed BGRA of each committed buffer (0xFFFFFFFF = not uniform)
	std::vector<uint64_t> seen_modifiers;
	int commits = 0, import_failures = 0;
	int drm = -1;
	bool syncobj = false;
	int acquire_waits = 0, acquire_wait_failures = 0, releases = 0, already_signalled = 0;
	std::atomic<bool> stop{false};
} comp;

static void log_bind(const char *iface, uint32_t v)
{
	std::lock_guard<std::mutex> l(comp.m);
	comp.binds.push_back(std::string(iface) + " v" + std::to_string(v));
}

static uint32_t fourcc_to_vk_ok(uint32_t f) { return f == DRM_FORMAT_ARGB8888 || f == DRM_FORMAT_XRGB8888; }

// Imports the buffer into the compositor's VkDevice, copies it out and returns the colour if uniform.
static void inspect(Buffer *b)
{
	DmaImage img;
	std::string err;
	std::vector<PlaneLayout> pl;
	for (auto &p : b->planes)
		if (p.fd >= 0)
			pl.push_back({p.offset, 0, p.stride});
	bool ok = fourcc_to_vk_ok(b->format) &&
		  import_dmabuf(comp.vk, img, b->planes[0].fd, VK_FORMAT_B8G8R8A8_UNORM, b->w, b->h, b->planes[0].mod, pl,
				VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &err);
	std::lock_guard<std::mutex> l(comp.m);
	comp.commits++;
	comp.seen_modifiers.push_back(b->planes[0].mod);
	if (!ok) {
		printf("  [compositor] import of %s %dx%d mod 0x%llx failed: %s\n", fourcc_str(b->format), b->w, b->h,
		       (unsigned long long)b->planes[0].mod, err.c_str());
		comp.import_failures++;
		return;
	}
	Ctx &c = comp.vk;
	Ctx::Buffer rb = c.host_buffer((VkDeviceSize)b->w * b->h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
	VkCommandBuffer cb = c.begin();
	image_barrier(cb, img.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
		      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
		      VK_ACCESS_2_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_FOREIGN_EXT, c.qfam);
	VkBufferImageCopy r{};
	r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	r.imageExtent = {(uint32_t)b->w, (uint32_t)b->h, 1};
	vkCmdCopyImageToBuffer(cb, img.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &r);
	image_barrier(cb, img.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		      VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_NONE, 0, c.qfam,
		      VK_QUEUE_FAMILY_FOREIGN_EXT);
	c.submit(cb);
	const uint32_t *px = (const uint32_t *)rb.map;
	uint32_t first = px[0];
	bool uniform = true;
	for (int i = 0; i < b->w * b->h; i++)
		uniform &= px[i] == first;
	comp.seen_colors.push_back(uniform ? first : 0xFFFFFFFFu);
	c.free_buffer(rb);
	destroy_dma_image(c, img);
}

// wl_buffer
static void buffer_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wl_buffer_interface buffer_impl = {buffer_destroy};
static void buffer_free(wl_resource *r)
{
	Buffer *b = (Buffer *)wl_resource_get_user_data(r);
	for (auto &p : b->planes)
		if (p.fd >= 0)
			close(p.fd);
	delete b;
}

// zwp_linux_buffer_params_v1
static void params_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void params_add(wl_client *, wl_resource *r, int32_t fd, uint32_t idx, uint32_t offset, uint32_t stride,
		       uint32_t hi, uint32_t lo)
{
	Params *p = (Params *)wl_resource_get_user_data(r);
	if (idx < 4) {
		p->planes[idx] = {fd, offset, stride, ((uint64_t)hi << 32) | lo};
	} else {
		close(fd);
	}
}
static wl_resource *make_buffer(wl_client *client, Params *p, uint32_t id, int32_t w, int32_t h, uint32_t format)
{
	Buffer *b = new Buffer{nullptr, w, h, format, p->planes};
	for (auto &pl : p->planes)
		pl.fd = -1; // moved
	b->res = wl_resource_create(client, &wl_buffer_interface, 1, id);
	wl_resource_set_implementation(b->res, &buffer_impl, b, buffer_free);
	return b->res;
}
static void params_create(wl_client *client, wl_resource *r, int32_t w, int32_t h, uint32_t format, uint32_t)
{
	Params *p = (Params *)wl_resource_get_user_data(r);
	wl_resource *buf = make_buffer(client, p, 0, w, h, format);
	zwp_linux_buffer_params_v1_send_created(r, buf);
}
static void params_create_immed(wl_client *client, wl_resource *r, uint32_t id, int32_t w, int32_t h, uint32_t format,
				uint32_t)
{
	make_buffer(client, (Params *)wl_resource_get_user_data(r), id, w, h, format);
}
static const struct zwp_linux_buffer_params_v1_interface params_impl = {params_destroy, params_add, params_create,
									params_create_immed};
static void params_free(wl_resource *r)
{
	Params *p = (Params *)wl_resource_get_user_data(r);
	for (auto &pl : p->planes)
		if (pl.fd >= 0)
			close(pl.fd);
	delete p;
}

// zwp_linux_dmabuf_feedback_v1
static void fb_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct zwp_linux_dmabuf_feedback_v1_interface fb_impl = {fb_destroy};
static void send_feedback(wl_resource *fb)
{
	wl_array dev;
	wl_array_init(&dev);
	*(dev_t *)wl_array_add(&dev, sizeof(dev_t)) = comp.render_dev;
	zwp_linux_dmabuf_feedback_v1_send_format_table(fb, comp.table_fd, (uint32_t)comp.table_size);
	zwp_linux_dmabuf_feedback_v1_send_main_device(fb, &dev);
	zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(fb, &dev);
	wl_array idx;
	wl_array_init(&idx);
	for (size_t i = 0; i < comp.formats.size(); i++)
		*(uint16_t *)wl_array_add(&idx, sizeof(uint16_t)) = (uint16_t)i;
	zwp_linux_dmabuf_feedback_v1_send_tranche_formats(fb, &idx);
	zwp_linux_dmabuf_feedback_v1_send_tranche_flags(fb, 0);
	zwp_linux_dmabuf_feedback_v1_send_tranche_done(fb);
	zwp_linux_dmabuf_feedback_v1_send_done(fb);
	wl_array_release(&dev);
	wl_array_release(&idx);
}
static void new_feedback(wl_client *client, wl_resource *r, uint32_t id)
{
	wl_resource *fb = wl_resource_create(client, &zwp_linux_dmabuf_feedback_v1_interface, wl_resource_get_version(r), id);
	wl_resource_set_implementation(fb, &fb_impl, nullptr, nullptr);
	send_feedback(fb);
}

// zwp_linux_dmabuf_v1
static void dmabuf_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void dmabuf_create_params(wl_client *client, wl_resource *r, uint32_t id)
{
	wl_resource *p = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface, wl_resource_get_version(r), id);
	wl_resource_set_implementation(p, &params_impl, new Params, params_free);
}
static void dmabuf_default_feedback(wl_client *client, wl_resource *r, uint32_t id)
{
	log_bind("  zwp_linux_dmabuf_v1.get_default_feedback", 0);
	new_feedback(client, r, id);
}
static void dmabuf_surface_feedback(wl_client *client, wl_resource *r, uint32_t id, wl_resource *)
{
	log_bind("  zwp_linux_dmabuf_v1.get_surface_feedback", 0);
	new_feedback(client, r, id);
}
static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {dmabuf_destroy, dmabuf_create_params,
								  dmabuf_default_feedback, dmabuf_surface_feedback};
static void dmabuf_bind(wl_client *client, void *, uint32_t version, uint32_t id)
{
	log_bind("zwp_linux_dmabuf_v1", version);
	wl_resource *r = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface, version, id);
	wl_resource_set_implementation(r, &dmabuf_impl, nullptr, nullptr);
	if (version < 4)
		for (auto &f : comp.formats) {
			if (version >= 3)
				zwp_linux_dmabuf_v1_send_modifier(r, f.first, f.second >> 32, f.second & 0xffffffff);
			else
				zwp_linux_dmabuf_v1_send_format(r, f.first);
		}
}

// wl_callback / wl_region / wl_surface / wl_compositor
static void region_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void region_op(wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {}
static const struct wl_region_interface region_impl = {region_destroy, region_op, region_op};

static void surf_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static void surf_attach(wl_client *, wl_resource *r, wl_resource *buf, int32_t, int32_t)
{
	((Surface *)wl_resource_get_user_data(r))->pending = buf;
}
static void surf_damage(wl_client *, wl_resource *, int32_t, int32_t, int32_t, int32_t) {}
static void surf_frame(wl_client *client, wl_resource *r, uint32_t id)
{
	wl_resource *cb = wl_resource_create(client, &wl_callback_interface, 1, id);
	wl_resource_set_implementation(cb, nullptr, nullptr, nullptr);
	((Surface *)wl_resource_get_user_data(r))->frame_cbs.push_back(cb);
}
static void surf_region(wl_client *, wl_resource *, wl_resource *) {}
static void surf_commit(wl_client *, wl_resource *r)
{
	Surface *s = (Surface *)wl_resource_get_user_data(r);
	if (s->pending && s->acquire.set) {
		// Explicit sync: the buffer may only be read once the acquire point has signalled.
		uint64_t now = 0;
		drmSyncobjQuery(comp.drm, &s->acquire.handle, &now, 1);
		if (now >= s->acquire.value)
			comp.already_signalled++;
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		int64_t abs = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec + 2000000000ll;
		int e = drmSyncobjTimelineWait(comp.drm, &s->acquire.handle, &s->acquire.value, 1, abs,
					       DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, nullptr);
		comp.acquire_waits++;
		if (e)
			comp.acquire_wait_failures++;
	}
	if (s->pending) {
		inspect((Buffer *)wl_resource_get_user_data(s->pending));
		if (s->release.set) {
			drmSyncobjTimelineSignal(comp.drm, &s->release.handle, &s->release.value, 1);
			comp.releases++;
		}
		s->acquire.set = s->release.set = false;
		// "Display" it, then release the previous one: the GPU copy in inspect() already finished.
		if (s->current && s->current != s->pending)
			wl_buffer_send_release(s->current);
		s->current = s->pending;
		s->pending = nullptr;
	}
	for (auto cb : s->frame_cbs) {
		wl_callback_send_done(cb, (uint32_t)(comp.commits * 16));
		wl_resource_destroy(cb);
	}
	s->frame_cbs.clear();
}
static void surf_transform(wl_client *, wl_resource *, int32_t) {}
static void surf_offset(wl_client *, wl_resource *, int32_t, int32_t) {}
static const struct wl_surface_interface surf_impl = {surf_destroy, surf_attach,  surf_damage, surf_frame,
						      surf_region,  surf_region,  surf_commit, surf_transform,
						      surf_transform, surf_damage, surf_offset};
static void surf_free(wl_resource *r) { delete (Surface *)wl_resource_get_user_data(r); }

static void comp_create_surface(wl_client *client, wl_resource *r, uint32_t id)
{
	Surface *s = new Surface;
	s->res = wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(r), id);
	wl_resource_set_implementation(s->res, &surf_impl, s, surf_free);
}
static void comp_create_region(wl_client *client, wl_resource *r, uint32_t id)
{
	wl_resource *g = wl_resource_create(client, &wl_region_interface, wl_resource_get_version(r), id);
	wl_resource_set_implementation(g, &region_impl, nullptr, nullptr);
}
static const struct wl_compositor_interface comp_impl = {comp_create_surface, comp_create_region};
static void comp_bind(wl_client *client, void *, uint32_t version, uint32_t id)
{
	log_bind("wl_compositor", version);
	wl_resource *r = wl_resource_create(client, &wl_compositor_interface, version, id);
	wl_resource_set_implementation(r, &comp_impl, nullptr, nullptr);
}

// wp_linux_drm_syncobj_v1
static void so_destroy(wl_client *, wl_resource *r) { wl_resource_destroy(r); }
static const struct wp_linux_drm_syncobj_timeline_v1_interface tl_impl = {so_destroy};
static void tl_free(wl_resource *r)
{
	drmSyncobjDestroy(comp.drm, (uint32_t)(uintptr_t)wl_resource_get_user_data(r));
}
static void sos_point(wl_resource *r, wl_resource *tl, uint32_t hi, uint32_t lo, bool acquire)
{
	Surface *s = (Surface *)wl_resource_get_user_data(r);
	Point &p = acquire ? s->acquire : s->release;
	p.handle = (uint32_t)(uintptr_t)wl_resource_get_user_data(tl);
	p.value = ((uint64_t)hi << 32) | lo;
	p.set = true;
}
static void sos_acquire(wl_client *, wl_resource *r, wl_resource *tl, uint32_t hi, uint32_t lo)
{
	sos_point(r, tl, hi, lo, true);
}
static void sos_release(wl_client *, wl_resource *r, wl_resource *tl, uint32_t hi, uint32_t lo)
{
	sos_point(r, tl, hi, lo, false);
}
static const struct wp_linux_drm_syncobj_surface_v1_interface sos_impl = {so_destroy, sos_acquire, sos_release};
static void som_get_surface(wl_client *client, wl_resource *r, uint32_t id, wl_resource *surface)
{
	log_bind("  wp_linux_drm_syncobj_manager_v1.get_surface", 0);
	wl_resource *s = wl_resource_create(client, &wp_linux_drm_syncobj_surface_v1_interface, 1, id);
	wl_resource_set_implementation(s, &sos_impl, wl_resource_get_user_data(surface), nullptr);
}
static void som_import(wl_client *client, wl_resource *r, uint32_t id, int32_t fd)
{
	uint32_t h = 0;
	int e = drmSyncobjFDToHandle(comp.drm, fd, &h);
	close(fd);
	log_bind(e ? "  wp_linux_drm_syncobj_manager_v1.import_timeline FAILED" : "  wp_linux_drm_syncobj_manager_v1.import_timeline", 0);
	if (e) {
		wl_resource_post_error(r, WP_LINUX_DRM_SYNCOBJ_MANAGER_V1_ERROR_INVALID_TIMELINE, "bad syncobj fd");
		return;
	}
	wl_resource *t = wl_resource_create(client, &wp_linux_drm_syncobj_timeline_v1_interface, 1, id);
	wl_resource_set_implementation(t, &tl_impl, (void *)(uintptr_t)h, tl_free);
}
static const struct wp_linux_drm_syncobj_manager_v1_interface som_impl = {so_destroy, som_get_surface, som_import};
static void som_bind(wl_client *client, void *, uint32_t version, uint32_t id)
{
	log_bind("wp_linux_drm_syncobj_manager_v1", version);
	wl_resource *r = wl_resource_create(client, &wp_linux_drm_syncobj_manager_v1_interface, version, id);
	wl_resource_set_implementation(r, &som_impl, nullptr, nullptr);
}

static bool comp_init(const char *socket)
{
	comp.vk.init(dmabuf_exts());
	struct stat st;
	if (stat("/dev/dri/renderD128", &st) == 0)
		comp.render_dev = st.st_rdev;
	for (uint32_t f : {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888})
		for (auto &m : query_modifiers(comp.vk, VK_FORMAT_B8G8R8A8_UNORM, VK_IMAGE_USAGE_TRANSFER_SRC_BIT, W, H))
			if (m.importable)
				comp.formats.push_back({f, m.mod});
	struct Entry {
		uint32_t format, pad;
		uint64_t mod;
	};
	comp.table_size = comp.formats.size() * sizeof(Entry);
	comp.table_fd = memfd_create("dmabuf-format-table", MFD_CLOEXEC);
	ftruncate(comp.table_fd, (off_t)comp.table_size);
	Entry *e = (Entry *)mmap(nullptr, comp.table_size, PROT_WRITE, MAP_SHARED, comp.table_fd, 0);
	for (size_t i = 0; i < comp.formats.size(); i++)
		e[i] = {comp.formats[i].first, 0, comp.formats[i].second};
	munmap(e, comp.table_size);
	comp.dpy = wl_display_create();
	if (wl_display_add_socket(comp.dpy, socket)) {
		perror("wl_display_add_socket");
		return false;
	}
	wl_global_create(comp.dpy, &wl_compositor_interface, 6, nullptr, comp_bind);
	wl_global_create(comp.dpy, &zwp_linux_dmabuf_v1_interface, 4, nullptr, dmabuf_bind);
	if (comp.syncobj) {
		comp.drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
		wl_global_create(comp.dpy, &wp_linux_drm_syncobj_manager_v1_interface, 1, nullptr, som_bind);
	}
	return true;
}

static void comp_run()
{
	wl_event_loop *loop = wl_display_get_event_loop(comp.dpy);
	while (!comp.stop) {
		wl_display_flush_clients(comp.dpy);
		wl_event_loop_dispatch(loop, 10);
	}
}

// ---------------------------------------------------------------- client
static wl_compositor *cl_comp;
static void reg_global(void *, wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
	if (!strcmp(iface, wl_compositor_interface.name))
		cl_comp = (wl_compositor *)wl_registry_bind(reg, name, &wl_compositor_interface, std::min(version, 4u));
}
static void reg_remove(void *, wl_registry *, uint32_t) {}
static const wl_registry_listener reg_listener = {reg_global, reg_remove};

static const float COLORS[FRAMES][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1},
					 {1, 1, 0, 1}, {0, 1, 1, 1}, {1, 0, 1, 1}};
static uint32_t packed_bgra(const float *c)
{
	auto u = [](float f) { return (uint32_t)(f * 255.0f + 0.5f); };
	return u(c[2]) | u(c[1]) << 8 | u(c[0]) << 16 | u(c[3]) << 24;
}

static void client(const char *socket, VkPresentModeKHR want_mode)
{
	wl_display *dpy = wl_display_connect(socket);
	check(dpy != nullptr, "client: wl_display_connect(%s)", socket);
	if (!dpy)
		return;
	wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, nullptr);
	wl_display_roundtrip(dpy);
	wl_surface *surf = wl_compositor_create_surface(cl_comp);

	Ctx c;
	c.init({VK_KHR_SWAPCHAIN_EXTENSION_NAME}, {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME});
	VkBool32 support = vkGetPhysicalDeviceWaylandPresentationSupportKHR(c.phys, c.qfam, dpy);
	check(support, "client: vkGetPhysicalDeviceWaylandPresentationSupportKHR");
	VkWaylandSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
	sci.display = dpy;
	sci.surface = surf;
	VkSurfaceKHR vs;
	VkResult r = vkCreateWaylandSurfaceKHR(c.inst, &sci, nullptr, &vs);
	check(r == VK_SUCCESS, "client: vkCreateWaylandSurfaceKHR (%s)", vk_result_str(r));
	VkBool32 qs = VK_FALSE;
	vkGetPhysicalDeviceSurfaceSupportKHR(c.phys, c.qfam, vs, &qs);
	check(qs, "client: queue family can present to the surface");
	VkSurfaceCapabilitiesKHR caps;
	VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c.phys, vs, &caps));
	uint32_t n = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(c.phys, vs, &n, nullptr);
	std::vector<VkSurfaceFormatKHR> fmts(n);
	vkGetPhysicalDeviceSurfaceFormatsKHR(c.phys, vs, &n, fmts.data());
	printf("client: surface caps: images %u..%u, usage 0x%x; %u formats:", caps.minImageCount, caps.maxImageCount,
	       caps.supportedUsageFlags, n);
	for (auto &f : fmts)
		printf(" %d/%d", f.format, f.colorSpace);
	vkGetPhysicalDeviceSurfacePresentModesKHR(c.phys, vs, &n, nullptr);
	std::vector<VkPresentModeKHR> modes(n);
	vkGetPhysicalDeviceSurfacePresentModesKHR(c.phys, vs, &n, modes.data());
	printf("; present modes:");
	for (auto m : modes)
		printf(" %d", m);
	printf("\n");
	VkSurfaceFormatKHR fmt = fmts[0];
	for (auto &f : fmts)
		if (f.format == VK_FORMAT_B8G8R8A8_UNORM)
			fmt = f;
	VkSwapchainCreateInfoKHR sw{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	sw.surface = vs;
	sw.minImageCount = std::max(caps.minImageCount, 3u);
	if (caps.maxImageCount)
		sw.minImageCount = std::min(sw.minImageCount, caps.maxImageCount);
	sw.imageFormat = fmt.format;
	sw.imageColorSpace = fmt.colorSpace;
	sw.imageExtent = {W, H};
	sw.imageArrayLayers = 1;
	sw.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	sw.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
	sw.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
				    ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
				    : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
	sw.presentMode = want_mode;
	sw.clipped = VK_TRUE;
	VkSwapchainKHR swc;
	r = vkCreateSwapchainKHR(c.dev, &sw, nullptr, &swc);
	check(r == VK_SUCCESS, "client: vkCreateSwapchainKHR(%ux%u, format %d, present mode %d) (%s)", W, H, fmt.format,
	      want_mode, vk_result_str(r));
	if (r != VK_SUCCESS)
		return;
	vkGetSwapchainImagesKHR(c.dev, swc, &n, nullptr);
	std::vector<VkImage> imgs(n);
	vkGetSwapchainImagesKHR(c.dev, swc, &n, imgs.data());
	printf("client: %u swapchain images\n", n);
	VkSemaphore acq[FRAMES], done[FRAMES];
	VkSemaphoreCreateInfo sci2{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence;
	vkCreateFence(c.dev, &fci, nullptr, &fence);
	int presented = 0;
	for (uint32_t f = 0; f < FRAMES; f++) {
		vkCreateSemaphore(c.dev, &sci2, nullptr, &acq[f]);
		vkCreateSemaphore(c.dev, &sci2, nullptr, &done[f]);
		uint32_t idx;
		r = vkAcquireNextImageKHR(c.dev, swc, 2000000000ull, acq[f], VK_NULL_HANDLE, &idx);
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
			check(false, "client: vkAcquireNextImageKHR frame %u (%s)", f, vk_result_str(r));
			break;
		}
		VkCommandBuffer cb = c.begin();
		image_barrier(cb, imgs[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
			      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT, 0,
			      VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		VkClearColorValue cv;
		memcpy(cv.float32, COLORS[f], sizeof cv.float32);
		VkImageSubresourceRange rr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &rr);
		image_barrier(cb, imgs[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_CLEAR_BIT,
			      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
		VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
		w.semaphore = acq[f];
		w.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		VkSemaphoreSubmitInfo s = w;
		s.semaphore = done[f];
		c.submit(cb, {w}, {s}, false);
		VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		pi.waitSemaphoreCount = 1;
		pi.pWaitSemaphores = &done[f];
		pi.swapchainCount = 1;
		pi.pSwapchains = &swc;
		pi.pImageIndices = &idx;
		r = vkQueuePresentKHR(c.q, &pi);
		if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
			check(false, "client: vkQueuePresentKHR frame %u (%s)", f, vk_result_str(r));
			break;
		}
		presented++;
		if (!comp.syncobj)
			vkQueueWaitIdle(c.q); // implicit sync is not tested: make sure rendering finished
		wl_display_roundtrip(dpy); // let the compositor see the commit (and send release/frame done)
	}
	check(presented == (int)FRAMES, "client: presented %d/%u frames", presented, FRAMES);
	vkDeviceWaitIdle(c.dev);
	vkDestroySwapchainKHR(c.dev, swc, nullptr);
	for (uint32_t f = 0; f < FRAMES; f++) {
		vkDestroySemaphore(c.dev, acq[f], nullptr);
		vkDestroySemaphore(c.dev, done[f], nullptr);
	}
	vkDestroyFence(c.dev, fence, nullptr);
	vkDestroySurfaceKHR(c.inst, vs, nullptr);
	c.destroy();
	wl_surface_destroy(surf);
	wl_display_roundtrip(dpy);
	wl_display_disconnect(dpy);
}

int main(int argc, char **argv)
{
	VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--syncobj"))
			comp.syncobj = true;
		else
			mode = (VkPresentModeKHR)atoi(argv[i]);
	}
	if (!getenv("XDG_RUNTIME_DIR")) {
		fprintf(stderr, "XDG_RUNTIME_DIR must be set\n");
		return 2;
	}
	char sock[64];
	snprintf(sock, sizeof sock, "knurbs-spike-%d", getpid());
	if (!comp_init(sock))
		return 2;
	print_driver(comp.vk);
	std::thread ct(comp_run);
	client(sock, mode);
	comp.stop = true;
	ct.join();

	printf("globals/requests used by the client + WSI:\n");
	for (auto &b : comp.binds)
		printf("  %s\n", b.c_str());
	printf("compositor: %d commits, %d import failures; colours:", comp.commits, comp.import_failures);
	for (auto c : comp.seen_colors)
		printf(" %08x", c);
	printf("\n");
	int matched = 0;
	for (uint32_t f = 0; f < FRAMES; f++)
		for (auto c : comp.seen_colors)
			if (c == packed_bgra(COLORS[f])) {
				matched++;
				break;
			}
	check(comp.commits >= (int)FRAMES && comp.import_failures == 0,
	      "compositor received %d commits of dmabuf wl_buffers, all imported", comp.commits);
	check(matched == (int)FRAMES, "compositor saw all %u client colours, uniform, in its own VkDevice (%d)", FRAMES,
	      matched);
	if (comp.syncobj) {
		printf("explicit sync: %d acquire waits (%d already signalled at commit, %d failed), %d release signals\n",
		       comp.acquire_waits, comp.already_signalled, comp.acquire_wait_failures, comp.releases);
		check(comp.acquire_waits >= (int)FRAMES && comp.acquire_wait_failures == 0 && comp.releases >= (int)FRAMES,
		      "WSI used wp_linux_drm_syncobj: acquire point per commit, release point per buffer");
	}
	if (!comp.seen_modifiers.empty())
		printf("client buffer modifier: 0x%llx\n", (unsigned long long)comp.seen_modifiers[0]);
	wl_display_destroy_clients(comp.dpy);
	wl_display_destroy(comp.dpy);
	int rc = finish(comp.vk, "spike G");
	comp.vk.destroy();
	return rc;
}
