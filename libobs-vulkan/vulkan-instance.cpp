// Instance, physical device choice, logical device, queues and allocator.
#define VMA_IMPLEMENTATION
#include "vulkan-subsystem.hpp"

#include <util/base.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

static std::atomic<int> validation_messages{-1}; // -1: no validation layer active
static std::string adapter_name;
static std::string driver_version;

const char *vk_adapter_name()
{
	return adapter_name.c_str();
}

const char *vk_driver_version()
{
	return driver_version.c_str();
}

extern "C" int obs_vulkan_validation_messages(void)
{
	return validation_messages;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
					       VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data,
					       void *)
{
	if (validation_messages >= 0)
		++validation_messages;
	blog(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? LOG_ERROR : LOG_WARNING,
	     "[libobs-vulkan] validation: %s", data->pMessage);
	return VK_FALSE;
}

static VkDebugUtilsMessengerCreateInfoEXT messenger_info()
{
	VkDebugUtilsMessengerCreateInfoEXT mi = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
	mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
			 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	mi.pfnUserCallback = debug_cb;
	return mi;
}

static bool has_ext(const std::vector<VkExtensionProperties> &exts, const char *name)
{
	for (auto &e : exts)
		if (!strcmp(e.extensionName, name))
			return true;
	return false;
}

// vulkan-1.dll is delay-loaded on Windows, so a missing loader must be detected before the first Vulkan call.
// OBS_VULKAN_TEST_NO_LOADER=1 simulates its absence for tests. (Linux still hard-links libvulkan.)
static bool loader_available()
{
	const char *no = getenv("OBS_VULKAN_TEST_NO_LOADER");
	if (no && !strcmp(no, "1"))
		return false;
#ifdef _WIN32
	return LoadLibraryW(L"vulkan-1.dll") != nullptr;
#else
	return true;
#endif
}

static int create_instance(gs_device *d)
{
	if (!loader_available())
		return GS_ERROR_NOT_SUPPORTED;

	uint32_t version = VK_API_VERSION_1_0;
	vkEnumerateInstanceVersion(&version);
	if (version < VK_API_VERSION_1_3)
		return GS_ERROR_NOT_SUPPORTED;

	const char *layers[1] = {};
	const char *exts[3] = {};
	uint32_t nlayers = 0, nexts = 0;

	uint32_t n = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> eprops(n);
	vkEnumerateInstanceExtensionProperties(nullptr, &n, eprops.data());

	// Presenting needs both; without them the device still works headless and swapchain creation fails.
	// TODO(linux): VK_KHR_xlib/xcb/wayland_surface, see vulkan-swapchain.cpp.
#ifdef _WIN32
	const char *surface_ext = "VK_KHR_win32_surface";
#else
	const char *surface_ext = nullptr;
#endif
	const bool surfaces = surface_ext && has_ext(eprops, VK_KHR_SURFACE_EXTENSION_NAME) && has_ext(eprops, surface_ext);
	if (surfaces) {
		exts[nexts++] = VK_KHR_SURFACE_EXTENSION_NAME;
		exts[nexts++] = surface_ext;
	}

	const char *env = getenv("OBS_VULKAN_VALIDATION");
	if (env && !strcmp(env, "1")) {
		vkEnumerateInstanceLayerProperties(&n, nullptr);
		std::vector<VkLayerProperties> props(n);
		vkEnumerateInstanceLayerProperties(&n, props.data());
		bool layer = false;
		for (auto &p : props)
			layer |= !strcmp(p.layerName, "VK_LAYER_KHRONOS_validation");

		if (layer && has_ext(eprops, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
			layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
			exts[nexts++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
		} else {
			blog(LOG_WARNING, "[libobs-vulkan] OBS_VULKAN_VALIDATION=1 but the validation layer is missing");
		}
	}

	VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "OBS Studio";
	app.apiVersion = VK_API_VERSION_1_3;

	// Chained so vkCreateInstance/vkDestroyInstance messages (e.g. leaked objects) are reported too.
	VkDebugUtilsMessengerCreateInfoEXT mi = messenger_info();

	VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ci.pNext = nlayers ? &mi : nullptr;
	ci.pApplicationInfo = &app;
	ci.enabledLayerCount = nlayers;
	ci.ppEnabledLayerNames = layers;
	ci.enabledExtensionCount = nexts;
	ci.ppEnabledExtensionNames = exts;
	VkInstance instance = VK_NULL_HANDLE;
	if (vkCreateInstance(&ci, nullptr, &instance) != VK_SUCCESS)
		return GS_ERROR_NOT_SUPPORTED;
	d->instance = instance;
	d->can_present = surfaces; // and VK_KHR_swapchain, checked in create_device

	if (nlayers) {
		validation_messages = 0;
		auto create = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
			d->instance, "vkCreateDebugUtilsMessengerEXT");
		if (create)
			create(d->instance, &mi, nullptr, &d->messenger);
	} else {
		validation_messages = -1;
	}
	return GS_SUCCESS;
}

static const char *priority_name(float p)
{
	return p <= 0.25f ? "low" : p <= 0.5f ? "medium" : p <= 0.75f ? "high" : "realtime";
}

static VkSemaphore create_timeline(VkDevice dev)
{
	VkSemaphoreTypeCreateInfo ti = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
	VkSemaphore sem = VK_NULL_HANDLE;
	vkCreateSemaphore(dev, &si, nullptr, &sem);
	return sem;
}

static int create_device(gs_device *d, uint32_t adapter)
{
	uint32_t n = 0;
	vkEnumeratePhysicalDevices(d->instance, &n, nullptr);
	if (n == 0)
		return GS_ERROR_NOT_SUPPORTED;
	if (adapter >= n)
		return GS_ERROR_FAIL;
	std::vector<VkPhysicalDevice> phys(n);
	vkEnumeratePhysicalDevices(d->instance, &n, phys.data());
	d->phys = phys[adapter];

	VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
	VkPhysicalDeviceProperties2 props = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &drv};
	vkGetPhysicalDeviceProperties2(d->phys, &props);
	if (props.properties.apiVersion < VK_API_VERSION_1_3)
		return GS_ERROR_NOT_SUPPORTED;

	VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
	VkPhysicalDeviceFeatures2 feats = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
	vkGetPhysicalDeviceFeatures2(d->phys, &feats);
	if (!f13.synchronization2 || !f13.dynamicRendering || !f12.timelineSemaphore)
		return GS_ERROR_NOT_SUPPORTED;

	// Only the features the backend uses are enabled.
	VkPhysicalDeviceVulkan13Features en13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
	en13.synchronization2 = en13.dynamicRendering = VK_TRUE;
	VkPhysicalDeviceVulkan12Features en12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &en13};
	en12.timelineSemaphore = VK_TRUE;
	VkPhysicalDeviceFeatures core = {};
	core.samplerAnisotropy = feats.features.samplerAnisotropy;
	d->anisotropy = feats.features.samplerAnisotropy;
	en12.samplerMirrorClampToEdge = d->mirror_clamp = f12.samplerMirrorClampToEdge;
	d->limits = props.properties.limits;

	// Queue families: first graphics family, plus a transfer-only family when one exists.
	uint32_t nfam = 0;
	vkGetPhysicalDeviceQueueFamilyProperties2(d->phys, &nfam, nullptr);
	std::vector<VkQueueFamilyGlobalPriorityPropertiesKHR> prio(nfam,
								   {VK_STRUCTURE_TYPE_QUEUE_FAMILY_GLOBAL_PRIORITY_PROPERTIES_KHR});
	std::vector<VkQueueFamilyProperties2> fams(nfam, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});

	uint32_t next = 0;
	vkEnumerateDeviceExtensionProperties(d->phys, nullptr, &next, nullptr);
	std::vector<VkExtensionProperties> dexts(next);
	vkEnumerateDeviceExtensionProperties(d->phys, nullptr, &next, dexts.data());
	const bool have_prio_ext = has_ext(dexts, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME);
	d->can_present = d->can_present && has_ext(dexts, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
	if (have_prio_ext)
		for (uint32_t i = 0; i < nfam; ++i)
			fams[i].pNext = &prio[i];
	vkGetPhysicalDeviceQueueFamilyProperties2(d->phys, &nfam, fams.data());

	uint32_t gfx = UINT32_MAX, xfer = UINT32_MAX;
	for (uint32_t i = 0; i < nfam; ++i) {
		VkQueueFlags fl = fams[i].queueFamilyProperties.queueFlags;
		if (gfx == UINT32_MAX && (fl & VK_QUEUE_GRAPHICS_BIT))
			gfx = i;
		else if (xfer == UINT32_MAX && (fl & VK_QUEUE_TRANSFER_BIT) && !(fl & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)))
			xfer = i;
	}
	if (gfx == UINT32_MAX)
		return GS_ERROR_NOT_SUPPORTED;

	// Request LOW (below the foreground app; never REALTIME) where the family reports it.
	auto supports_low = [&](uint32_t fam) {
		if (!have_prio_ext)
			return false;
		for (uint32_t i = 0; i < prio[fam].priorityCount; ++i)
			if (prio[fam].priorities[i] == VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR)
				return true;
		return false;
	};

	const float one = 0.5f;
	uint32_t families[2] = {gfx, xfer};
	const uint32_t nq = xfer == UINT32_MAX ? 1 : 2;

	VkResult res = VK_ERROR_INITIALIZATION_FAILED;
	bool granted_low[2] = {};
	for (int attempt = 0; attempt < 2; ++attempt) {
		VkDeviceQueueGlobalPriorityCreateInfoKHR gp[2] = {
			{VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR},
			{VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR}};
		VkDeviceQueueCreateInfo qi[2] = {{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO},
						 {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}};
		bool any = false;
		for (uint32_t i = 0; i < nq; ++i) {
			granted_low[i] = attempt == 0 && supports_low(families[i]);
			any |= granted_low[i];
			gp[i].globalPriority = VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR;
			qi[i].pNext = granted_low[i] ? &gp[i] : nullptr;
			qi[i].queueFamilyIndex = families[i];
			qi[i].queueCount = 1;
			qi[i].pQueuePriorities = &one;
		}
		const char *dev_exts[2];
		uint32_t ndev_exts = 0;
		if (any)
			dev_exts[ndev_exts++] = VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME;
		if (d->can_present)
			dev_exts[ndev_exts++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
		VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		di.pNext = &en12;
		di.pEnabledFeatures = &core;
		di.queueCreateInfoCount = nq;
		di.pQueueCreateInfos = qi;
		di.enabledExtensionCount = ndev_exts;
		di.ppEnabledExtensionNames = dev_exts;
		VkDevice dev = VK_NULL_HANDLE;
		res = vkCreateDevice(d->phys, &di, nullptr, &dev);
		if (res == VK_SUCCESS)
			d->dev = dev;
		if (res == VK_SUCCESS || !any)
			break; // a retry would send the same request
		blog(LOG_WARNING, "[libobs-vulkan] vkCreateDevice failed (%d), retrying without global priority",
		     (int)res);
	}
	if (res != VK_SUCCESS)
		return GS_ERROR_NOT_SUPPORTED;

	vk_queue *queues[2] = {&d->graphics, &d->transfer};
	const char *labels[2] = {"graphics", "transfer"};
	for (uint32_t i = 0; i < nq; ++i) {
		queues[i]->family = families[i];
		queues[i]->priority = granted_low[i] ? 0.25f : 0.5f;
		vkGetDeviceQueue(d->dev, families[i], 0, &queues[i]->q);
		queues[i]->timeline = create_timeline(d->dev);
		if (!queues[i]->timeline)
			return GS_ERROR_NOT_SUPPORTED;
		blog(LOG_INFO, "[libobs-vulkan] %s queue: family %u, global priority %s", labels[i], families[i],
		     priority_name(queues[i]->priority));
	}

	adapter_name = props.properties.deviceName;
	if (drv.driverInfo[0]) {
		driver_version = std::string(drv.driverName) + " " + drv.driverInfo;
	} else {
		uint32_t v = props.properties.driverVersion;
		driver_version = std::to_string(VK_API_VERSION_MAJOR(v)) + "." + std::to_string(VK_API_VERSION_MINOR(v)) +
				 "." + std::to_string(VK_API_VERSION_PATCH(v));
	}
	blog(LOG_INFO, "[libobs-vulkan] adapter: %s, driver %s", adapter_name.c_str(), driver_version.c_str());

	VmaAllocatorCreateInfo ai = {};
	ai.vulkanApiVersion = VK_API_VERSION_1_3;
	ai.instance = d->instance;
	ai.physicalDevice = d->phys;
	ai.device = d->dev;
	if (vmaCreateAllocator(&ai, &d->vma) != VK_SUCCESS)
		return GS_ERROR_FAIL;

	d->frames.vma = d->vma;
	if (!d->frames.init(d->dev, d->graphics.family))
		return GS_ERROR_FAIL;
	return GS_SUCCESS;
}

int vk_device_init(gs_device *d, uint32_t adapter)
{
	int ret = create_instance(d);
	if (ret == GS_SUCCESS)
		ret = create_device(d, adapter);
	if (ret != GS_SUCCESS)
		vk_device_free(d);
	return ret;
}

void vk_device_free(gs_device *d)
{
	if (d->dev)
		vkDeviceWaitIdle(d->dev);
	d->frames.destroy(); // runs deferred work, which returns acquire semaphores to spare_sems
	vk_pipelines_free(d);
	if (d->acquire_wait)
		d->spare_sems.push_back(d->acquire_wait);
	for (VkSemaphore s : d->spare_sems)
		vkDestroySemaphore(d->dev, s, nullptr);
	if (d->vma)
		vmaDestroyAllocator(d->vma);
	for (vk_queue *q : {&d->graphics, &d->transfer})
		if (q->timeline)
			vkDestroySemaphore(d->dev, q->timeline, nullptr);
	if (d->dev)
		vkDestroyDevice(d->dev, nullptr);
	if (d->messenger) {
		auto destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
			d->instance, "vkDestroyDebugUtilsMessengerEXT");
		if (destroy)
			destroy(d->instance, d->messenger, nullptr);
	}
	if (d->instance)
		vkDestroyInstance(d->instance, nullptr);
	*d = gs_device{};
}
