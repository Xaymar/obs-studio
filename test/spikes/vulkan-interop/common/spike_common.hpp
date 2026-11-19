// Shared helpers for the Vulkan interop spikes. Throwaway test code.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifdef _WIN32
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <volk.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace spike {

inline int g_validation_errors = 0;
inline int g_validation_warnings = 0;
inline bool g_validation_enabled = false;

inline void log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fputc('\n', stdout);
	fflush(stdout);
}

inline const char *vkres(VkResult r)
{
	switch (r) {
#define R(x) \
	case x:  \
		return #x;
		R(VK_SUCCESS)
		R(VK_NOT_READY)
		R(VK_TIMEOUT)
		R(VK_INCOMPLETE)
		R(VK_ERROR_OUT_OF_HOST_MEMORY)
		R(VK_ERROR_OUT_OF_DEVICE_MEMORY)
		R(VK_ERROR_INITIALIZATION_FAILED)
		R(VK_ERROR_DEVICE_LOST)
		R(VK_ERROR_MEMORY_MAP_FAILED)
		R(VK_ERROR_LAYER_NOT_PRESENT)
		R(VK_ERROR_EXTENSION_NOT_PRESENT)
		R(VK_ERROR_FEATURE_NOT_PRESENT)
		R(VK_ERROR_INCOMPATIBLE_DRIVER)
		R(VK_ERROR_FORMAT_NOT_SUPPORTED)
		R(VK_ERROR_UNKNOWN)
		R(VK_ERROR_INVALID_EXTERNAL_HANDLE)
		R(VK_ERROR_OUT_OF_POOL_MEMORY)
#undef R
	default:
		return "VK_?";
	}
}

struct check_failed {
	std::string what;
};

#define VKC(expr)                                                                                       \
	do {                                                                                             \
		VkResult _r = (expr);                                                                    \
		if (_r != VK_SUCCESS) {                                                                  \
			::spike::log("  !! %s -> %s (%d) at %s:%d", #expr, ::spike::vkres(_r), (int)_r, \
				     __FILE__, __LINE__);                                                \
			throw ::spike::check_failed{#expr};                                              \
		}                                                                                        \
	} while (0)

inline VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
					       VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *d,
					       void *)
{
	if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
		g_validation_errors++;
		log("  [VVL ERROR] %s", d->pMessage);
	} else if (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
		g_validation_warnings++;
		log("  [VVL WARN] %s", d->pMessage);
	}
	return VK_FALSE;
}

struct instance {
	VkInstance inst = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

	void create(const char *name, const std::vector<const char *> &extra_exts = {})
	{
		VKC(volkInitialize());
		uint32_t n = 0;
		vkEnumerateInstanceLayerProperties(&n, nullptr);
		std::vector<VkLayerProperties> layers(n);
		vkEnumerateInstanceLayerProperties(&n, layers.data());
		bool have_vvl = false;
		for (auto &l : layers)
			if (!strcmp(l.layerName, "VK_LAYER_KHRONOS_validation"))
				have_vvl = true;
		const char *no_vvl = getenv("SPIKE_NO_VALIDATION");
		g_validation_enabled = have_vvl && !(no_vvl && *no_vvl == '1');

		std::vector<const char *> exts = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
		exts.insert(exts.end(), extra_exts.begin(), extra_exts.end());
		std::vector<const char *> lays;
		if (g_validation_enabled)
			lays.push_back("VK_LAYER_KHRONOS_validation");

		VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		ai.pApplicationName = name;
		ai.apiVersion = VK_API_VERSION_1_3;
		VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ci.pApplicationInfo = &ai;
		ci.enabledExtensionCount = (uint32_t)exts.size();
		ci.ppEnabledExtensionNames = exts.data();
		ci.enabledLayerCount = (uint32_t)lays.size();
		ci.ppEnabledLayerNames = lays.data();

		// Synchronization validation adds useful coverage for the interop paths.
		VkValidationFeatureEnableEXT vfe[] = {VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT};
		VkValidationFeaturesEXT vf{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
		vf.enabledValidationFeatureCount = 1;
		vf.pEnabledValidationFeatures = vfe;
		if (g_validation_enabled)
			ci.pNext = &vf;

		VKC(vkCreateInstance(&ci, nullptr, &inst));
		volkLoadInstance(inst);
		log("validation layer: %s", g_validation_enabled ? "ENABLED (VK_LAYER_KHRONOS_validation + sync)"
								  : "not available / disabled");

		VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
		mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
				     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
				 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
				 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		mi.pfnUserCallback = debug_cb;
		vkCreateDebugUtilsMessengerEXT(inst, &mi, nullptr, &messenger);
	}

	std::vector<VkPhysicalDevice> physical_devices() const
	{
		uint32_t n = 0;
		vkEnumeratePhysicalDevices(inst, &n, nullptr);
		std::vector<VkPhysicalDevice> v(n);
		vkEnumeratePhysicalDevices(inst, &n, v.data());
		return v;
	}

	void destroy()
	{
		if (messenger)
			vkDestroyDebugUtilsMessengerEXT(inst, messenger, nullptr);
		if (inst)
			vkDestroyInstance(inst, nullptr);
		inst = VK_NULL_HANDLE;
	}
};

struct gpu_info {
	VkPhysicalDeviceProperties props{};
	VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
	VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
	bool is_nvidia() const { return props.vendorID == 0x10DE; }
	bool is_amd() const { return props.vendorID == 0x1002; }
	std::string uuid_str() const
	{
		char b[64];
		char *p = b;
		for (int i = 0; i < 16; i++)
			p += sprintf(p, "%02x", id.deviceUUID[i]);
		return b;
	}
};

inline gpu_info query_gpu(VkPhysicalDevice pd)
{
	gpu_info g;
	g.id.pNext = &g.drv;
	VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	p2.pNext = &g.id;
	vkGetPhysicalDeviceProperties2(pd, &p2);
	g.props = p2.properties;
	return g;
}

inline void print_gpu(const gpu_info &g)
{
	uint32_t v = g.props.apiVersion;
	log("GPU: %s (vendor 0x%04x device 0x%04x)", g.props.deviceName, g.props.vendorID, g.props.deviceID);
	log("  driver: %s / %s, Vulkan API %u.%u.%u, driverVersion 0x%08x", g.drv.driverName, g.drv.driverInfo,
	    VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v), g.props.driverVersion);
	log("  deviceUUID %s, LUID valid %d", g.uuid_str().c_str(), (int)g.id.deviceLUIDValid);
}

inline bool has_device_ext(VkPhysicalDevice pd, const char *name)
{
	uint32_t n = 0;
	vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> e(n);
	vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, e.data());
	for (auto &x : e)
		if (!strcmp(x.extensionName, name))
			return true;
	return false;
}

struct device {
	VkPhysicalDevice pd = VK_NULL_HANDLE;
	VkDevice dev = VK_NULL_HANDLE;
	uint32_t qf = 0;
	VkQueue queue = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	VolkDeviceTable t{};
	VkPhysicalDeviceMemoryProperties mem{};

	// Creates a device with one graphics+compute queue, Vulkan 1.3 core features
	// (dynamic rendering, sync2, timeline semaphores) and the requested extensions.
	void create(VkPhysicalDevice p, const std::vector<const char *> &exts, void *extra_features = nullptr)
	{
		pd = p;
		uint32_t n = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
		std::vector<VkQueueFamilyProperties> q(n);
		vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, q.data());
		for (uint32_t i = 0; i < n; i++)
			if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				qf = i;
				break;
			}
		float prio = 1.0f;
		VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qi.queueFamilyIndex = qf;
		qi.queueCount = 1;
		qi.pQueuePriorities = &prio;

		VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
		f13.dynamicRendering = VK_TRUE;
		f13.synchronization2 = VK_TRUE;
		f13.pNext = extra_features;
		VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
		f12.timelineSemaphore = VK_TRUE;
		f12.pNext = &f13;
		VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		f2.pNext = &f12;

		VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		ci.pNext = &f2;
		ci.queueCreateInfoCount = 1;
		ci.pQueueCreateInfos = &qi;
		ci.enabledExtensionCount = (uint32_t)exts.size();
		ci.ppEnabledExtensionNames = exts.data();
		VKC(vkCreateDevice(pd, &ci, nullptr, &dev));
		volkLoadDeviceTable(&t, dev);
		t.vkGetDeviceQueue(dev, qf, 0, &queue);
		VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pi.queueFamilyIndex = qf;
		VKC(t.vkCreateCommandPool(dev, &pi, nullptr, &pool));
		vkGetPhysicalDeviceMemoryProperties(pd, &mem);
	}

	void destroy()
	{
		if (!dev)
			return;
		t.vkDeviceWaitIdle(dev);
		t.vkDestroyCommandPool(dev, pool, nullptr);
		t.vkDestroyDevice(dev, nullptr);
		dev = VK_NULL_HANDLE;
	}

	uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) const
	{
		for (uint32_t i = 0; i < mem.memoryTypeCount; i++)
			if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want)
				return i;
		throw check_failed{"no suitable memory type"};
	}

	VkCommandBuffer begin()
	{
		VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		ai.commandPool = pool;
		ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		ai.commandBufferCount = 1;
		VkCommandBuffer cb;
		VKC(t.vkAllocateCommandBuffers(dev, &ai, &cb));
		VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VKC(t.vkBeginCommandBuffer(cb, &bi));
		return cb;
	}

	// Ends, submits and waits. Optional pNext for the submit (keyed mutex, timeline signal).
	void submit_wait(VkCommandBuffer cb, const void *submit_pnext = nullptr, VkSemaphore signal = VK_NULL_HANDLE)
	{
		VKC(t.vkEndCommandBuffer(cb));
		VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		si.pNext = submit_pnext;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cb;
		if (signal) {
			si.signalSemaphoreCount = 1;
			si.pSignalSemaphores = &signal;
		}
		VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		VkFence f;
		VKC(t.vkCreateFence(dev, &fi, nullptr, &f));
		VKC(t.vkQueueSubmit(queue, 1, &si, f));
		VKC(t.vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX));
		t.vkDestroyFence(dev, f, nullptr);
		t.vkFreeCommandBuffers(dev, pool, 1, &cb);
	}
};

struct buffer {
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	VkDeviceSize size = 0;
	void *map = nullptr;

	void create_host(device &d, VkDeviceSize sz, VkBufferUsageFlags usage)
	{
		size = sz;
		VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bi.size = sz;
		bi.usage = usage;
		VKC(d.t.vkCreateBuffer(d.dev, &bi, nullptr, &buf));
		VkMemoryRequirements mr;
		d.t.vkGetBufferMemoryRequirements(d.dev, buf, &mr);
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = d.find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
									  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		VKC(d.t.vkAllocateMemory(d.dev, &ai, nullptr, &mem));
		VKC(d.t.vkBindBufferMemory(d.dev, buf, mem, 0));
		VKC(d.t.vkMapMemory(d.dev, mem, 0, VK_WHOLE_SIZE, 0, &map));
	}
	void destroy(device &d)
	{
		if (buf)
			d.t.vkDestroyBuffer(d.dev, buf, nullptr);
		if (mem)
			d.t.vkFreeMemory(d.dev, mem, nullptr);
		buf = VK_NULL_HANDLE;
		mem = VK_NULL_HANDLE;
	}
};

inline void image_barrier(device &d, VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect,
			  VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
			  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
			  uint32_t src_qf = VK_QUEUE_FAMILY_IGNORED, uint32_t dst_qf = VK_QUEUE_FAMILY_IGNORED)
{
	VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
	b.srcStageMask = src_stage;
	b.srcAccessMask = src_access;
	b.dstStageMask = dst_stage;
	b.dstAccessMask = dst_access;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = src_qf;
	b.dstQueueFamilyIndex = dst_qf;
	b.image = img;
	b.subresourceRange = {aspect, 0, 1, 0, 1};
	VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	di.imageMemoryBarrierCount = 1;
	di.pImageMemoryBarriers = &b;
	d.t.vkCmdPipelineBarrier2(cb, &di);
}

inline const char *fmt_features_str(VkFormatFeatureFlags f)
{
	static char s[512];
	s[0] = 0;
	auto add = [&](VkFormatFeatureFlags bit, const char *n) {
		if (f & bit) {
			strcat(s, n);
			strcat(s, " ");
		}
	};
	add(VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT, "SAMPLED");
	add(VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT, "STORAGE");
	add(VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT, "COLOR_ATTACHMENT");
	add(VK_FORMAT_FEATURE_TRANSFER_SRC_BIT, "TRANSFER_SRC");
	add(VK_FORMAT_FEATURE_TRANSFER_DST_BIT, "TRANSFER_DST");
	add(VK_FORMAT_FEATURE_DISJOINT_BIT, "DISJOINT");
	add(VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT, "MIDPOINT_CHROMA");
	add(VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT, "COSITED_CHROMA");
	add(VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT, "YCBCR_LINEAR");
	if (!s[0])
		strcpy(s, "(none)");
	return s;
}

inline const char *ext_mem_features_str(VkExternalMemoryFeatureFlags f)
{
	static char s[128];
	s[0] = 0;
	if (f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT)
		strcat(s, "DEDICATED_ONLY ");
	if (f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)
		strcat(s, "EXPORTABLE ");
	if (f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT)
		strcat(s, "IMPORTABLE ");
	if (!s[0])
		strcpy(s, "(none)");
	return s;
}

// Reset validation counters around a test case and report them.
struct vvl_scope {
	int e0, w0;
	vvl_scope() : e0(g_validation_errors), w0(g_validation_warnings) {}
	int errors() const { return g_validation_errors - e0; }
	int warnings() const { return g_validation_warnings - w0; }
};

// SPIKE_FILTER=<substring>: only run test cases whose label contains it. SPIKE_GPU=<substring>: only that GPU.
inline bool skip_case(const std::string &label)
{
	const char *f = getenv("SPIKE_FILTER");
	return f && *f && label.find(f) == std::string::npos;
}
inline bool skip_gpu(const char *name)
{
	const char *f = getenv("SPIKE_GPU");
	return f && *f && !strstr(name, f);
}

struct results {
	std::vector<std::string> lines;
	int fails = 0;
	void add(const std::string &gpu, const std::string &test, bool pass, const std::string &note, int vvl_err = -1)
	{
		char b[1024];
		snprintf(b, sizeof b, "%-4s | %-26s | %-48s | vvl_err=%s | %s", pass ? "PASS" : "FAIL", gpu.c_str(),
			 test.c_str(),
			 !g_validation_enabled ? "n/a" : (vvl_err < 0 ? "-" : std::to_string(vvl_err).c_str()),
			 note.c_str());
		lines.push_back(b);
		if (!pass)
			fails++;
	}
	void print() const
	{
		log("\n===== RESULTS =====");
		for (auto &l : lines)
			log("%s", l.c_str());
	}
};

} // namespace spike
