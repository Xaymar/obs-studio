// Swapchains, acquire and present.
#include "vulkan-subsystem.hpp"

#include <util/base.h>

#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif

#include <algorithm>

static VkSemaphore take_sem(gs_device *d)
{
	if (!d->spare_sems.empty()) {
		VkSemaphore s = d->spare_sems.back();
		d->spare_sems.pop_back();
		return s;
	}
	VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkSemaphore s = VK_NULL_HANDLE;
	vkCreateSemaphore(d->dev, &si, nullptr, &s);
	return s;
}

void vk_consume_semaphore(gs_device *d, VkSemaphore sem)
{
	VkSemaphoreSubmitInfo w = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
	w.semaphore = sem;
	w.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
	VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
	si.waitSemaphoreInfoCount = 1;
	si.pWaitSemaphoreInfos = &w;
	if (vkQueueSubmit2(d->graphics.q, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS &&
	    vkQueueWaitIdle(d->graphics.q) == VK_SUCCESS) {
		d->spare_sems.push_back(sem);
		return;
	}
	vkDeviceWaitIdle(d->dev); // the device is most likely lost; the semaphore is not reusable
	vkDestroySemaphore(d->dev, sem, nullptr);
}

// Submits recorded work and the pending acquire wait, if any. With nothing recorded, the acquire wait is consumed by a
// batch with no commands: later batches are then ordered after the acquire only by driver behaviour, not by the spec.
// OBS's display order (obs-display.c render_display_begin/render_display: load_swapchain, is_present_ready, draw,
// present per display) never gets here with
// an empty frame; the remaining callers (release_images, destroy) idle the queue right after anyway.
static void submit_pending(gs_device *d)
{
	if (d->acquire_wait || d->frames.recording) {
		vk_cmd(d);
		vk_flush(d);
	}
}

static void release_images(gs_swap_chain *s)
{
	gs_device *d = s->device;
	if (s->image != UINT32_MAX) {
		submit_pending(d); // recorded commands may use the acquired image; it is never presented
		s->image = UINT32_MAX;
		s->target.image = VK_NULL_HANDLE;
		s->target.rt_view = VK_NULL_HANDLE;
	}
	if (s->images.empty()) {
		return;
	}
	// Sync point (rebuild path): no submission or present may still use the images or present semaphores.
	vkQueueWaitIdle(d->graphics.q);
	for (VkImage i : s->images) {
		d->tracker.forget(i);
	}
	for (VkSemaphore p : s->present_sems) {
		vkDestroySemaphore(d->dev, p, nullptr);
	}
	for (VkImageView v : s->views) {
		vkDestroyImageView(d->dev, v, nullptr);
	}
	s->views.clear();
	s->images.clear();
	s->present_sems.clear();
}

// (Re)creates the swapchain for the current window size. No swapchain (and true) while it is 0x0 or minimized.
static bool build(gs_swap_chain *s)
{
	gs_device *d = s->device;
	release_images(s);
	VkSwapchainKHR old = s->swapchain;
	s->swapchain = VK_NULL_HANDLE;
	s->recreate = false;

	VkSurfaceCapabilitiesKHR caps = {}; // read below even when the query fails
	VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d->phys, s->surface, &caps);
	VkExtent2D ext = caps.currentExtent;
	if (r == VK_SUCCESS && ext.width == UINT32_MAX) { // the surface takes the swapchain's size
		ext = {std::clamp(s->cx, caps.minImageExtent.width, caps.maxImageExtent.width),
		       std::clamp(s->cy, caps.minImageExtent.height, caps.maxImageExtent.height)};
	}
	const VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (r != VK_SUCCESS || (caps.supportedUsageFlags & usage) != usage || !s->cx || !s->cy || !ext.width ||
	    !ext.height) {
		vkDestroySwapchainKHR(d->dev, old, nullptr);
		if (r != VK_SUCCESS || (caps.supportedUsageFlags & usage) != usage) {
			blog(LOG_ERROR, "[libobs-vulkan] swapchain: surface unusable (%d)", (int)r);
			return false;
		}
		return true;
	}

	VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	ci.surface = s->surface;
	ci.minImageCount = caps.minImageCount + 1;
	if (caps.maxImageCount) {
		ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
	}
	ci.imageFormat = s->surface_format.format;
	ci.imageColorSpace = s->surface_format.colorSpace;
	ci.imageExtent = ext;
	ci.imageArrayLayers = 1;
	ci.imageUsage = usage;
	ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ci.preTransform = caps.currentTransform;
	const VkCompositeAlphaFlagsKHR alpha = caps.supportedCompositeAlpha;
	ci.compositeAlpha = (alpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
				    ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
				    : (VkCompositeAlphaFlagBitsKHR)(alpha & (~alpha + 1));
	ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; // always supported; vsynced like D3D11's Present(1)
	ci.clipped = VK_TRUE;
	ci.oldSwapchain = old;
	r = vkCreateSwapchainKHR(d->dev, &ci, nullptr, &s->swapchain);
	vkDestroySwapchainKHR(d->dev, old, nullptr);
	if (r != VK_SUCCESS) {
		s->swapchain = VK_NULL_HANDLE;
		blog(LOG_ERROR, "[libobs-vulkan] vkCreateSwapchainKHR failed (%d)", (int)r);
		return false;
	}

	uint32_t n = 0;
	vkGetSwapchainImagesKHR(d->dev, s->swapchain, &n, nullptr);
	s->images.resize(n);
	vkGetSwapchainImagesKHR(d->dev, s->swapchain, &n, s->images.data());
	s->present_sems.resize(n);
	VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	for (VkSemaphore &p : s->present_sems) {
		vkCreateSemaphore(d->dev, &si, nullptr, &p);
	}
	for (VkImage image : s->images) {
		VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vi.image = image;
		vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vi.format = s->surface_format.format;
		vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		VkImageView v = VK_NULL_HANDLE;
		vkCreateImageView(d->dev, &vi, nullptr, &v);
		s->views.push_back(v);
	}
	s->target.rt_format = s->surface_format.format;
	s->target.width = ext.width;
	s->target.height = ext.height;
	++s->builds;
	return true;
}

// VK_SUBOPTIMAL_KHR is still presentable; it needs a rebuild only when the surface size no longer matches (the format
// and the FIFO present mode never change). Rebuilding on every SUBOPTIMAL would idle the queue on every acquire.
static bool extent_changed(gs_swap_chain *s)
{
	VkSurfaceCapabilitiesKHR caps = {};
	if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s->device->phys, s->surface, &caps) != VK_SUCCESS) {
		return true; // the rebuild reports it
	}
	const VkExtent2D e = caps.currentExtent; // UINT32_MAX: the swapchain decides, device_resize rebuilds
	return e.width != UINT32_MAX && (e.width != s->target.width || e.height != s->target.height);
}

static void destroy_swap(gs_swap_chain *s)
{
	gs_device *d = s->device;
	release_images(s);
	vkDestroySwapchainKHR(d->dev, s->swapchain, nullptr);
	vkDestroySurfaceKHR(d->instance, s->surface, nullptr);
	delete s;
}

extern "C" {

EXPORT gs_swapchain_t *device_swapchain_create(gs_device_t *device, const struct gs_init_data *data)
{
	if (!device->can_present) {
		blog(LOG_ERROR, "[libobs-vulkan] device_swapchain_create: surface or swapchain extensions missing");
		return nullptr;
	}

	VkSurfaceKHR surface = VK_NULL_HANDLE;
#ifdef _WIN32
	VkWin32SurfaceCreateInfoKHR wi = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
	wi.hinstance = GetModuleHandleW(nullptr);
	wi.hwnd = (HWND)data->window.hwnd;
	if (vkCreateWin32SurfaceKHR(device->instance, &wi, nullptr, &surface) != VK_SUCCESS) {
		blog(LOG_ERROR, "[libobs-vulkan] device_swapchain_create: vkCreateWin32SurfaceKHR failed");
		return nullptr;
	}
#else
	// TODO(linux): Xlib/xcb (data->window.display, data->window.id) and Wayland surfaces. Not written: their headers
	// and instance extensions cannot be built or tested in this (Windows-only) branch.
	blog(LOG_ERROR, "[libobs-vulkan] device_swapchain_create: no surface support on this platform yet");
	return nullptr;
#endif

	auto s = new gs_swap_chain{device, surface};
	s->cx = data->cx;
	s->cy = data->cy;
	s->target.device = device;
	s->target.levels = 1;
	s->target.format = data->format;

	VkBool32 supported = VK_FALSE;
	vkGetPhysicalDeviceSurfaceSupportKHR(device->phys, device->graphics.family, surface, &supported);

	// shortcut: SDR only, the init format with SRGB_NONLINEAR; HDR needs VK_EXT_swapchain_colorspace.
	const VkFormat want = vk_format(data->format);
	uint32_t n = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(device->phys, surface, &n, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(n);
	vkGetPhysicalDeviceSurfaceFormatsKHR(device->phys, surface, &n, formats.data());
	bool found = false;
	for (const VkSurfaceFormatKHR &f : formats) {
		if (f.format == want && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
			s->surface_format = f;
			found = true;
		}
	}

	if (!supported || !found || !build(s)) {
		blog(LOG_ERROR, "[libobs-vulkan] device_swapchain_create: cannot present (support %d, format %d)",
		     (int)supported, (int)found);
		destroy_swap(s);
		return nullptr;
	}
	return s;
}

EXPORT void device_load_swapchain(gs_device_t *device, gs_swapchain_t *swapchain)
{
	// As D3D11: a render target that was the old swapchain's follows to the new one.
	if (device->swap && device->render_target == &device->swap->target) {
		device->render_target = nullptr;
	}
	device->swap = swapchain;
	if (!device->render_target && swapchain) {
		device->render_target = &swapchain->target;
	}
	device->space = swapchain ? swapchain->space : GS_CS_SRGB;
}

EXPORT void gs_swapchain_destroy(gs_swapchain_t *swapchain)
{
	if (!swapchain) {
		return;
	}
	if (swapchain->device->swap == swapchain) {
		device_load_swapchain(swapchain->device, nullptr);
	}
	destroy_swap(swapchain);
}

// 0x0 (minimized) leaves no swapchain: present is a no-op until a non-zero size.
EXPORT void device_resize(gs_device_t *device, uint32_t cx, uint32_t cy)
{
	gs_swap_chain *s = device->swap;
	if (!s) {
		blog(LOG_WARNING, "[libobs-vulkan] device_resize: no active swap");
		return;
	}
	s->cx = cx;
	s->cy = cy;
	build(s);
}

EXPORT enum gs_color_space device_get_color_space(gs_device_t *device)
{
	return device->space;
}

// shortcut: swapchains are SDR (SRGB_NONLINEAR) only, so the space never changes; HDR needs
// VK_EXT_swapchain_colorspace and monitor HDR detection (device_is_monitor_hdr), add with the HDR work.
EXPORT void device_update_color_space(gs_device_t *device)
{
	if (!device->swap) {
		blog(LOG_WARNING, "[libobs-vulkan] device_update_color_space: no active swap");
	}
}

EXPORT void device_get_size(const gs_device_t *device, uint32_t *x, uint32_t *y)
{
	*x = device->swap ? device->swap->target.width : 0;
	*y = device->swap ? device->swap->target.height : 0;
}

EXPORT uint32_t device_get_width(const gs_device_t *device)
{
	return device->swap ? device->swap->target.width : 0;
}

EXPORT uint32_t device_get_height(const gs_device_t *device)
{
	return device->swap ? device->swap->target.height : 0;
}

// Sync point (swapchain acquire), but it never blocks: a swapchain with no free image is skipped this frame.
// shortcut: readiness is the acquire with timeout 0, not VK_KHR_present_wait; see the task 5 report.
EXPORT bool device_is_present_ready(gs_device_t *device)
{
	gs_swap_chain *s = device->swap;
	if (!s) {
		blog(LOG_WARNING, "[libobs-vulkan] device_is_present_ready: no active swap");
		return false;
	}
	if (s->image != UINT32_MAX) {
		return true;
	}
	if (device->acquire_wait) {
		submit_pending(device); // another swapchain's acquire: one pending acquire wait at a time
	}
	for (int attempt = 0; attempt < 2; ++attempt) {
		if ((s->recreate || !s->swapchain) && !build(s)) {
			return false;
		}
		if (!s->swapchain) {
			return false; // 0x0 or minimized
		}
		VkSemaphore sem = take_sem(device);
		uint32_t idx = 0;
		VkResult r = s->test_not_ready
				     ? VK_NOT_READY
				     : vkAcquireNextImageKHR(device->dev, s->swapchain, 0, sem, VK_NULL_HANDLE, &idx);
		if (r == VK_SUCCESS && s->test_suboptimal) {
			r = VK_SUBOPTIMAL_KHR;
		}
		if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
			s->recreate = r == VK_SUBOPTIMAL_KHR && extent_changed(s); // still presentable
			s->image = idx;
			s->target.image = s->images[idx];
			s->target.rt_view = s->views[idx];
			device->acquire_wait = sem;
			// Contents are discarded every frame: restart from UNDEFINED. Recording the acquire as an
			// ALL_COMMANDS access makes the next barrier's source stage match the submit's semaphore wait.
			device->tracker.forget(s->target.image);
			vk_barriers none;
			device->tracker.use_image(s->target.image, device->graphics.family,
						  {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_IMAGE_LAYOUT_UNDEFINED},
						  none);
			return true;
		}
		device->spare_sems.push_back(sem); // a failed or not-ready acquire leaves it unsignalled
		if (r != VK_ERROR_OUT_OF_DATE_KHR) {
			if (r != VK_NOT_READY && r != VK_TIMEOUT) {
				blog(LOG_ERROR, "[libobs-vulkan] vkAcquireNextImageKHR failed (%d)", (int)r);
			}
			return false;
		}
		s->recreate = true;
	}
	return false;
}

// Queues the frame and the present; the semaphore orders them on the GPU, the CPU does not wait.
EXPORT void device_present(gs_device_t *device)
{
	gs_swap_chain *s = device->swap;
	if (!s) {
		blog(LOG_WARNING, "[libobs-vulkan] device_present: no active swap");
		return;
	}
	if (s->image == UINT32_MAX) {
		return; // not ready, or 0x0
	}
	const uint32_t idx = s->image;
	s->image = UINT32_MAX;
	s->target.image = VK_NULL_HANDLE;
	s->target.rt_view = VK_NULL_HANDLE;

	VkCommandBuffer cmd = vk_cmd(device);
	vk_barriers b;
	device->tracker.use_image(s->images[idx], device->graphics.family,
				  {VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR}, b);
	vk_record_barriers(cmd, b);

	VkSemaphore done = s->present_sems[idx];
	if (!vk_flush(device, &done)) {
		// `done` will never be signalled: skip the present. The rebuild on the next acquire destroys the
		// swapchain, which releases the image that stays acquired.
		s->recreate = true;
		return;
	}

	VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	pi.waitSemaphoreCount = 1;
	pi.pWaitSemaphores = &done;
	pi.swapchainCount = 1;
	pi.pSwapchains = &s->swapchain;
	pi.pImageIndices = &idx;
	VkResult r = vkQueuePresentKHR(device->graphics.q, &pi);
	if (r == VK_SUBOPTIMAL_KHR) {
		s->recreate = s->recreate || extent_changed(s);
	} else if (r != VK_SUCCESS) {
		if (r != VK_ERROR_OUT_OF_DATE_KHR) {
			blog(LOG_ERROR, "[libobs-vulkan] vkQueuePresentKHR failed (%d)", (int)r);
		}
		s->recreate = true;
	}
}
}
