// Shared helpers for the Linux Vulkan interop spikes. Throwaway test code.
#pragma once
#include <vulkan/vulkan.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define VK_CHECK(x)                                                                                           \
	do {                                                                                                  \
		VkResult r_ = (x);                                                                            \
		if (r_ != VK_SUCCESS) {                                                                       \
			fprintf(stderr, "%s:%d: %s -> %s (%d)\n", __FILE__, __LINE__, #x, vk_result_str(r_), \
				(int)r_);                                                                     \
			exit(2);                                                                              \
		}                                                                                             \
	} while (0)

inline const char *vk_result_str(VkResult r)
{
	switch (r) {
#define C(x) \
	case x: \
		return #x;
		C(VK_SUCCESS)
		C(VK_NOT_READY)
		C(VK_TIMEOUT)
		C(VK_ERROR_OUT_OF_HOST_MEMORY)
		C(VK_ERROR_OUT_OF_DEVICE_MEMORY)
		C(VK_ERROR_INITIALIZATION_FAILED)
		C(VK_ERROR_DEVICE_LOST)
		C(VK_ERROR_EXTENSION_NOT_PRESENT)
		C(VK_ERROR_FEATURE_NOT_PRESENT)
		C(VK_ERROR_FORMAT_NOT_SUPPORTED)
		C(VK_ERROR_INVALID_EXTERNAL_HANDLE)
		C(VK_ERROR_LAYER_NOT_PRESENT)
		C(VK_ERROR_UNKNOWN)
		C(VK_ERROR_OUT_OF_DATE_KHR)
		C(VK_SUBOPTIMAL_KHR)
		C(VK_ERROR_SURFACE_LOST_KHR)
		C(VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT)
#undef C
	default:
		return "VK_?";
	}
}

static int g_fail = 0;
inline void check(bool ok, const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	printf("%s ", ok ? "PASS" : "FAIL");
	vprintf(fmt, a);
	printf("\n");
	fflush(stdout);
	va_end(a);
	if (!ok)
		g_fail++;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
						  VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *d,
						  void *user)
{
	if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
		fprintf(stderr, "[validation] %s\n", d->pMessage);
		(*(int *)user)++;
	}
	return VK_FALSE;
}

struct Ctx {
	VkInstance inst = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice dev = VK_NULL_HANDLE;
	uint32_t qfam = 0;
	VkQueue q = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkPhysicalDeviceMemoryProperties memprops{};
	bool validation = false;
	int validation_msgs = 0;
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
	std::vector<std::string> enabled_dev_exts;

	template<class T> T dfn(const char *n) { return (T)vkGetDeviceProcAddr(dev, n); }
	template<class T> T ifn(const char *n) { return (T)vkGetInstanceProcAddr(inst, n); }

	static bool has_layer(const char *name)
	{
		uint32_t n = 0;
		vkEnumerateInstanceLayerProperties(&n, nullptr);
		std::vector<VkLayerProperties> l(n);
		vkEnumerateInstanceLayerProperties(&n, l.data());
		for (auto &p : l)
			if (!strcmp(p.layerName, name))
				return true;
		return false;
	}

	bool dev_has_ext(const char *name)
	{
		uint32_t n = 0;
		vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
		std::vector<VkExtensionProperties> e(n);
		vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, e.data());
		for (auto &p : e)
			if (!strcmp(p.extensionName, name))
				return true;
		return false;
	}

	// Creates instance + device on GPU 0 with Vulkan 1.3, sync2, dynamic rendering, timeline semaphores.
	void init(std::vector<const char *> dev_exts, std::vector<const char *> inst_exts = {})
	{
		VkApplicationInfo ai{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		ai.pApplicationName = "knurbs-spike";
		ai.apiVersion = VK_API_VERSION_1_3;
		std::vector<const char *> layers;
		validation = has_layer("VK_LAYER_KHRONOS_validation") && !getenv("SPIKE_NO_VALIDATION");
		if (validation) {
			layers.push_back("VK_LAYER_KHRONOS_validation");
			inst_exts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		}
		VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		ici.pApplicationInfo = &ai;
		ici.enabledLayerCount = (uint32_t)layers.size();
		ici.ppEnabledLayerNames = layers.data();
		ici.enabledExtensionCount = (uint32_t)inst_exts.size();
		ici.ppEnabledExtensionNames = inst_exts.data();
		VK_CHECK(vkCreateInstance(&ici, nullptr, &inst));
		if (validation) {
			VkDebugUtilsMessengerCreateInfoEXT m{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
			m.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
					    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
			m.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
					VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
					VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
			m.pfnUserCallback = vk_debug_cb;
			m.pUserData = &validation_msgs;
			ifn<PFN_vkCreateDebugUtilsMessengerEXT>("vkCreateDebugUtilsMessengerEXT")(inst, &m, nullptr,
												  &messenger);
		}
		uint32_t n = 0;
		vkEnumeratePhysicalDevices(inst, &n, nullptr);
		if (!n) {
			fprintf(stderr, "no Vulkan device\n");
			exit(2);
		}
		std::vector<VkPhysicalDevice> pds(n);
		vkEnumeratePhysicalDevices(inst, &n, pds.data());
		phys = pds[0];
		vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
		uint32_t qn = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
		std::vector<VkQueueFamilyProperties> qf(qn);
		vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qf.data());
		for (uint32_t i = 0; i < qn; i++)
			if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				qfam = i;
				break;
			}
		for (auto e : dev_exts) {
			if (!dev_has_ext(e)) {
				fprintf(stderr, "device extension missing: %s\n", e);
				exit(3);
			}
			enabled_dev_exts.push_back(e);
		}
		float prio = 1.0f;
		VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		qci.queueFamilyIndex = qfam;
		qci.queueCount = 1;
		qci.pQueuePriorities = &prio;
		VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
		f13.dynamicRendering = VK_TRUE;
		f13.synchronization2 = VK_TRUE;
		VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
		f12.timelineSemaphore = VK_TRUE;
		f12.pNext = &f13;
		VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
		f11.samplerYcbcrConversion = VK_TRUE;
		f11.pNext = &f12;
		VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		dci.pNext = &f11;
		dci.queueCreateInfoCount = 1;
		dci.pQueueCreateInfos = &qci;
		dci.enabledExtensionCount = (uint32_t)dev_exts.size();
		dci.ppEnabledExtensionNames = dev_exts.data();
		VK_CHECK(vkCreateDevice(phys, &dci, nullptr, &dev));
		vkGetDeviceQueue(dev, qfam, 0, &q);
		VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = qfam;
		VK_CHECK(vkCreateCommandPool(dev, &pci, nullptr, &pool));
	}

	void destroy()
	{
		if (dev) {
			vkDeviceWaitIdle(dev);
			vkDestroyCommandPool(dev, pool, nullptr);
			vkDestroyDevice(dev, nullptr);
		}
		if (messenger)
			ifn<PFN_vkDestroyDebugUtilsMessengerEXT>("vkDestroyDebugUtilsMessengerEXT")(inst, messenger,
												    nullptr);
		if (inst)
			vkDestroyInstance(inst, nullptr);
		dev = VK_NULL_HANDLE;
		inst = VK_NULL_HANDLE;
	}

	uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
	{
		for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
			if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
				return i;
		fprintf(stderr, "no memory type for bits 0x%x flags 0x%x\n", bits, want);
		exit(2);
	}

	VkCommandBuffer begin()
	{
		VkCommandBufferAllocateInfo a{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		a.commandPool = pool;
		a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		a.commandBufferCount = 1;
		VkCommandBuffer cb;
		VK_CHECK(vkAllocateCommandBuffers(dev, &a, &cb));
		VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		VK_CHECK(vkBeginCommandBuffer(cb, &b));
		return cb;
	}

	// Ends, submits (with optional semaphore waits/signals), waits idle, frees.
	void submit(VkCommandBuffer cb, std::vector<VkSemaphoreSubmitInfo> waits = {},
		    std::vector<VkSemaphoreSubmitInfo> signals = {}, bool wait_idle = true)
	{
		VK_CHECK(vkEndCommandBuffer(cb));
		VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
		cbi.commandBuffer = cb;
		VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
		si.commandBufferInfoCount = 1;
		si.pCommandBufferInfos = &cbi;
		si.waitSemaphoreInfoCount = (uint32_t)waits.size();
		si.pWaitSemaphoreInfos = waits.data();
		si.signalSemaphoreInfoCount = (uint32_t)signals.size();
		si.pSignalSemaphoreInfos = signals.data();
		VK_CHECK(vkQueueSubmit2(q, 1, &si, VK_NULL_HANDLE));
		if (wait_idle) {
			VK_CHECK(vkQueueWaitIdle(q));
			vkFreeCommandBuffers(dev, pool, 1, &cb);
		}
	}

	struct Buffer {
		VkBuffer buf = VK_NULL_HANDLE;
		VkDeviceMemory mem = VK_NULL_HANDLE;
		void *map = nullptr;
		VkDeviceSize size = 0;
	};

	Buffer host_buffer(VkDeviceSize size, VkBufferUsageFlags usage)
	{
		Buffer b;
		b.size = size;
		VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bci.size = size;
		bci.usage = usage;
		VK_CHECK(vkCreateBuffer(dev, &bci, nullptr, &b.buf));
		VkMemoryRequirements mr;
		vkGetBufferMemoryRequirements(dev, b.buf, &mr);
		VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		mai.allocationSize = mr.size;
		mai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
									 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		VK_CHECK(vkAllocateMemory(dev, &mai, nullptr, &b.mem));
		VK_CHECK(vkBindBufferMemory(dev, b.buf, b.mem, 0));
		VK_CHECK(vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map));
		return b;
	}
	void free_buffer(Buffer &b)
	{
		vkDestroyBuffer(dev, b.buf, nullptr);
		vkFreeMemory(dev, b.mem, nullptr);
		b = {};
	}
};

inline void image_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from,
			  VkImageLayout to, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
			  VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access,
			  uint32_t src_q = VK_QUEUE_FAMILY_IGNORED, uint32_t dst_q = VK_QUEUE_FAMILY_IGNORED)
{
	VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
	b.srcStageMask = src_stage;
	b.srcAccessMask = src_access;
	b.dstStageMask = dst_stage;
	b.dstAccessMask = dst_access;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = src_q;
	b.dstQueueFamilyIndex = dst_q;
	b.image = img;
	b.subresourceRange = {aspect, 0, 1, 0, 1};
	VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	d.imageMemoryBarrierCount = 1;
	d.pImageMemoryBarriers = &b;
	vkCmdPipelineBarrier2(cb, &d);
}

inline void print_driver(Ctx &c)
{
	VkPhysicalDeviceDriverProperties dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
	VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	p.pNext = &dp;
	vkGetPhysicalDeviceProperties2(c.phys, &p);
	printf("device: %s, driver %s %s, Vulkan %u.%u.%u, validation %s\n", p.properties.deviceName, dp.driverName,
	       dp.driverInfo, VK_API_VERSION_MAJOR(p.properties.apiVersion), VK_API_VERSION_MINOR(p.properties.apiVersion),
	       VK_API_VERSION_PATCH(p.properties.apiVersion), c.validation ? "ON" : "not installed");
}

inline int finish(Ctx &c, const char *name)
{
	if (c.validation)
		check(c.validation_msgs == 0, "%s: validation messages: %d", name, c.validation_msgs);
	printf("%s: %s (%d failure(s))\n", name, g_fail ? "FAILED" : "OK", g_fail);
	return g_fail ? 1 : 0;
}
