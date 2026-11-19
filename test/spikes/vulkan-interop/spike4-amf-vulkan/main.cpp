// Spike 4 (AMD only): AMF on its own VkDevice encoding a VkImage exported by a second (renderer) VkDevice.
//
// The renderer device (our VkInstance) creates an NV12 VkImage with OPAQUE_WIN32 export and fills it
// with a known pattern. The AMF context runs on a separate VkDevice on the same GPU (device UUID
// checked), either
//   (amf-own) created by AMF itself via InitVulkan(nullptr), or
//   (app)     created by us like AMF's DeviceVulkan sample (AMF's GetVulkanDeviceExtensions + external memory)
// and the image memory is imported into that device, wrapped with CreateSurfaceFromVulkanNative and
// fed to the H.264 encoder. No copies on our side. Output is decoded with $SPIKE_FFMPEG and compared.
#include "nv12_pattern.hpp"

#include <core/Factory.h>
#include <core/VulkanAMF.h>
#include <components/VideoEncoderVCE.h>

#include <thread>

using namespace spike;

namespace {

constexpr uint32_t W = 1920, H = 1080, FRAMES = 8;
constexpr VkFormat FMT = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
constexpr VkImageUsageFlags USAGE = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
				    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
constexpr VkImageCreateFlags FLAGS = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;

amf::AMFFactory *g_factory = nullptr;

#define AMFC(expr)                                                                                  \
	do {                                                                                         \
		AMF_RESULT _r = (expr);                                                              \
		if (_r != AMF_OK) {                                                                  \
			::spike::log("  !! %s -> AMF_RESULT %d at line %d", #expr, (int)_r, __LINE__); \
			throw ::spike::check_failed{std::string(#expr) + " -> AMF_RESULT " +         \
						    std::to_string((int)_r)};                        \
		}                                                                                    \
	} while (0)

struct run_result {
	bool ok = false;
	std::string note;
};

// Creates the device AMF will use, the way AMF's CPPSamples/common/DeviceVulkan.cpp does, plus external memory.
VkDevice create_amf_style_device(VkPhysicalDevice pd, amf::AMFContext1 *ctx1, std::string &note)
{
	amf_size n = 0;
	ctx1->GetVulkanDeviceExtensions(&n, nullptr);
	std::vector<const char *> want(n);
	ctx1->GetVulkanDeviceExtensions(&n, want.data());
	std::string listed;
	for (auto e : want)
		listed += std::string(e) + " ";
	log("     AMF GetVulkanDeviceExtensions: %s", listed.c_str());
	want.push_back("VK_KHR_shader_atomic_int64");
	want.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
	want.push_back(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
	std::vector<const char *> exts;
	for (auto e : want)
		if (has_device_ext(pd, e) &&
		    std::find_if(exts.begin(), exts.end(), [&](const char *x) { return !strcmp(x, e); }) == exts.end())
			exts.push_back(e);
		else if (!has_device_ext(pd, e))
			note += std::string("missing ext ") + e + "; ";

	uint32_t qn = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
	std::vector<VkQueueFamilyProperties> q(qn);
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, q.data());
	uint32_t gfx = UINT32_MAX, comp = UINT32_MAX, dec = UINT32_MAX;
	for (uint32_t i = 0; i < qn; i++) {
		if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && gfx == UINT32_MAX)
			gfx = i;
		if ((q[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && comp == UINT32_MAX)
			comp = i;
		if ((q[i].queueFlags & VK_QUEUE_VIDEO_DECODE_BIT_KHR) && dec == UINT32_MAX)
			dec = i;
	}
	// AMF calls vkGetDeviceQueue for every queue index of the families it uses (seen up to 7), so expose all of them.
	std::vector<float> prio(64, 1.0f);
	std::vector<VkDeviceQueueCreateInfo> qci;
	for (uint32_t fam : {gfx, comp, dec})
		if (fam != UINT32_MAX) {
			VkDeviceQueueCreateInfo c{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
			c.queueFamilyIndex = fam;
			c.queueCount = q[fam].queueCount;
			c.pQueuePriorities = prio.data();
			qci.push_back(c);
		}

	VkPhysicalDeviceFeatures features{};
	vkGetPhysicalDeviceFeatures(pd, &features);
	VkPhysicalDeviceSynchronization2Features sync2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
	sync2.synchronization2 = VK_TRUE;
	VkPhysicalDeviceCoherentMemoryFeaturesAMD coh{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COHERENT_MEMORY_FEATURES_AMD};
	coh.deviceCoherentMemory = VK_TRUE;
	sync2.pNext = &coh;
	VkPhysicalDeviceDescriptorIndexingFeatures di{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
	di.descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
	di.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
	di.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
	di.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
	di.descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
	di.descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
	coh.pNext = &di;
	VkPhysicalDeviceShaderAtomicInt64Features a64{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES};
	a64.shaderBufferInt64Atomics = VK_TRUE;
	di.pNext = &a64;
	VkPhysicalDeviceTimelineSemaphoreFeatures tl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
	tl.timelineSemaphore = VK_TRUE;
	a64.pNext = &tl;

	VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	ci.pNext = &sync2;
	ci.queueCreateInfoCount = (uint32_t)qci.size();
	ci.pQueueCreateInfos = qci.data();
	ci.pEnabledFeatures = &features;
	ci.enabledExtensionCount = (uint32_t)exts.size();
	ci.ppEnabledExtensionNames = exts.data();
	VkDevice dev = VK_NULL_HANDLE;
	VKC(vkCreateDevice(pd, &ci, nullptr, &dev));
	return dev;
}

run_result run(instance &in, VkPhysicalDevice our_pd, const gpu_info &g, bool amf_own_device, VkImageLayout told_layout)
{
	run_result rr;
	device rd; // renderer / exporter
	amf::AMFContext *ctx = nullptr;
	amf::AMFContext1 *ctx1 = nullptr;
	amf::AMFComponent *enc = nullptr;
	amf::AMFSurface *surf = nullptr;
	VkDevice app_amf_dev = VK_NULL_HANDLE;
	VkImage rimg = VK_NULL_HANDLE, aimg = VK_NULL_HANDLE;
	VkDeviceMemory rmem = VK_NULL_HANDLE, amem = VK_NULL_HANDLE;
	HANDLE h = nullptr;
	buffer staging;
	VolkDeviceTable at{};
	VkDevice adev = VK_NULL_HANDLE;
	amf::AMFVulkanDevice app_vd{};
	amf::AMFVulkanSurface vs{};
	std::vector<uint8_t> stream;
	try {
		rd.create(our_pd, {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME});

		AMFC(g_factory->CreateContext(&ctx));
		AMFC(ctx->QueryInterface(amf::AMFContext1::IID(), (void **)&ctx1));
		if (amf_own_device) {
			AMFC(ctx1->InitVulkan(nullptr));
		} else {
			std::string n;
			app_amf_dev = create_amf_style_device(our_pd, ctx1, n);
			if (!n.empty())
				log("     %s", n.c_str());
			app_vd.cbSizeof = sizeof app_vd;
			app_vd.hInstance = in.inst;
			app_vd.hPhysicalDevice = our_pd;
			app_vd.hDevice = app_amf_dev;
			AMFC(ctx1->InitVulkan(&app_vd));
		}
		auto *vd = (amf::AMFVulkanDevice *)ctx1->GetVulkanDevice();
		if (!vd || !vd->hDevice)
			throw check_failed{"GetVulkanDevice returned nothing"};
		adev = vd->hDevice;
		gpu_info ag = query_gpu(vd->hPhysicalDevice);
		bool same_gpu = !memcmp(ag.id.deviceUUID, g.id.deviceUUID, VK_UUID_SIZE);
		log("     AMF VkDevice %p (instance %p, %s instance) on \"%s\", UUID %s -> %s", (void *)adev,
		    (void *)vd->hInstance, vd->hInstance == in.inst ? "our" : "its own", ag.props.deviceName,
		    ag.uuid_str().c_str(), same_gpu ? "same GPU as renderer" : "DIFFERENT GPU");
		if (!same_gpu)
			throw check_failed{"AMF device is on a different GPU"};
		if (adev == rd.dev)
			throw check_failed{"AMF device == renderer device?"};
		volkLoadDeviceTable(&at, adev);
		bool ext_on = vkGetDeviceProcAddr(adev, "vkGetMemoryWin32HandlePropertiesKHR") != nullptr;
		log("     VK_KHR_external_memory_win32 entry points on the AMF device: %s", ext_on ? "present" : "ABSENT");
		rr.note += std::string("AMF dev ext_mem_win32 ") + (ext_on ? "on" : "OFF") + "; ";

		// --- renderer: exportable NV12 image
		VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.pNext = &emi;
		ii.flags = FLAGS;
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = FMT;
		ii.extent = {W, H, 1};
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = USAGE;
		VKC(rd.t.vkCreateImage(rd.dev, &ii, nullptr, &rimg));
		VkMemoryRequirements rmr;
		rd.t.vkGetImageMemoryRequirements(rd.dev, rimg, &rmr);
		VkMemoryDedicatedAllocateInfo dai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
		dai.image = rimg;
		VkExportMemoryAllocateInfo ea{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
		ea.pNext = &dai;
		ea.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.pNext = &ea;
		ai.allocationSize = rmr.size;
		ai.memoryTypeIndex = rd.find_mem(rmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VKC(rd.t.vkAllocateMemory(rd.dev, &ai, nullptr, &rmem));
		VKC(rd.t.vkBindImageMemory(rd.dev, rimg, rmem, 0));
		VkMemoryGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
		gi.memory = rmem;
		gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VKC(rd.t.vkGetMemoryWin32HandleKHR(rd.dev, &gi, &h));

		// --- AMF device: same image, imported memory
		VKC(at.vkCreateImage(adev, &ii, nullptr, &aimg));
		VkMemoryRequirements amr;
		at.vkGetImageMemoryRequirements(adev, aimg, &amr);
		log("     memReq renderer %llu (bits 0x%x) / AMF device %llu (bits 0x%x)", (unsigned long long)rmr.size,
		    rmr.memoryTypeBits, (unsigned long long)amr.size, amr.memoryTypeBits);
		VkPhysicalDeviceMemoryProperties amp;
		vkGetPhysicalDeviceMemoryProperties(vd->hPhysicalDevice, &amp);
		uint32_t amt = UINT32_MAX;
		for (uint32_t i = 0; i < amp.memoryTypeCount; i++)
			if ((amr.memoryTypeBits & (1u << i)) &&
			    (amp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
				amt = i;
				break;
			}
		if (amt == UINT32_MAX)
			throw check_failed{"no device-local memory type on AMF device"};
		VkMemoryDedicatedAllocateInfo adai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
		adai.image = aimg;
		VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
		imp.pNext = &adai;
		imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		imp.handle = h;
		VkMemoryAllocateInfo aai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		aai.pNext = &imp;
		aai.allocationSize = amr.size;
		aai.memoryTypeIndex = amt;
		VKC(at.vkAllocateMemory(adev, &aai, nullptr, &amem));
		VKC(at.vkBindImageMemory(adev, aimg, amem, 0));

		vs.cbSizeof = sizeof vs;
		vs.hImage = aimg;
		vs.hMemory = amem;
		vs.iSize = (amf_int64)amr.size;
		vs.eFormat = FMT;
		vs.iWidth = W;
		vs.iHeight = H;
		vs.eCurrentLayout = told_layout;
		vs.eUsage = amf::AMF_SURFACE_USAGE_SHADER_RESOURCE | amf::AMF_SURFACE_USAGE_RENDER_TARGET |
			    amf::AMF_SURFACE_USAGE_TRANSFER_SRC | amf::AMF_SURFACE_USAGE_TRANSFER_DST;
		vs.eAccess = amf::AMF_MEMORY_CPU_NONE;
		vs.Sync.cbSizeof = sizeof vs.Sync;
		AMFC(ctx1->CreateSurfaceFromVulkanNative(&vs, &surf, nullptr));
		log("     CreateSurfaceFromVulkanNative OK: format %d, %dx%d, memory type %d", (int)surf->GetFormat(),
		    surf->GetPlaneAt(0)->GetWidth(), surf->GetPlaneAt(0)->GetHeight(), (int)surf->GetMemoryType());

		// --- encoder
		AMFC(g_factory->CreateComponent(ctx, AMFVideoEncoderVCE_AVC, &enc));
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_TRANSCODING));
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, ::AMFConstructSize(W, H)));
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, ::AMFConstructRate(60, 1)));
		enc->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, 0);
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
				      AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CONSTANT_QP));
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_QP_I, 16));
		AMFC(enc->SetProperty(AMF_VIDEO_ENCODER_QP_P, 16));
		AMFC(enc->Init(amf::AMF_SURFACE_NV12, W, H));

		auto pull = [&](bool wait_for_one) -> bool {
			for (int tries = 0; tries < 2000; tries++) {
				amf::AMFData *out = nullptr;
				AMF_RESULT r = enc->QueryOutput(&out);
				if (r == AMF_EOF)
					return false;
				if (out) {
					amf::AMFBuffer *b = nullptr;
					out->QueryInterface(amf::AMFBuffer::IID(), (void **)&b);
					if (b) {
						const uint8_t *p = (const uint8_t *)b->GetNative();
						stream.insert(stream.end(), p, p + b->GetSize());
						b->Release();
					}
					out->Release();
					return true;
				}
				if (!wait_for_one)
					return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			throw check_failed{"QueryOutput timed out"};
		};

		staging.create_host(rd, (VkDeviceSize)W * H * 3 / 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
		for (uint32_t f = 0; f < FRAMES; f++) {
			fill_nv12((uint8_t *)staging.map, W, H, f);
			VkCommandBuffer cb = rd.begin();
			VkImageAspectFlags asp = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;
			image_barrier(rd, cb, rimg, asp, f ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
				      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
				      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
				      f ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED,
				      f ? rd.qf : VK_QUEUE_FAMILY_IGNORED);
			VkBufferImageCopy cp[2]{};
			cp[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
			cp[0].imageExtent = {W, H, 1};
			cp[1].bufferOffset = W * H;
			cp[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
			cp[1].imageExtent = {W / 2, H / 2, 1};
			rd.t.vkCmdCopyBufferToImage(cb, staging.buf, rimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, cp);
			image_barrier(rd, cb, rimg, asp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
				      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0,
				      rd.qf, VK_QUEUE_FAMILY_EXTERNAL);
			rd.submit_wait(cb); // CPU-side sync between the two devices (good enough for a spike)
			surf->SetPts((amf_pts)f * 10000000 / 60);
			AMF_RESULT r = enc->SubmitInput(surf);
			while (r == AMF_INPUT_FULL) {
				pull(true);
				r = enc->SubmitInput(surf);
			}
			AMFC(r);
			pull(true); // wait for this frame before the renderer overwrites the image
		}
		enc->Drain();
		while (pull(true)) {
		}
		std::string tag = std::string(amf_own_device ? "amfown" : "appdev") + "_" +
				  (told_layout == VK_IMAGE_LAYOUT_GENERAL ? "general" : "undefined");
		std::string path = "spike4_" + tag + ".h264";
		std::ofstream(path, std::ios::binary).write((const char *)stream.data(), stream.size());
		bool vok;
		std::string v = verify_with_ffmpeg(path, W, H, FRAMES, vok);
		rr.ok = vok && !stream.empty();
		rr.note += std::to_string(stream.size()) + " bytes H.264, " + v;
	} catch (check_failed &e) {
		rr.ok = false;
		rr.note += "exception: " + e.what;
	}
	if (enc) {
		enc->Terminate();
		enc->Release();
	}
	if (surf)
		surf->Release();
	if (adev) {
		at.vkDeviceWaitIdle(adev);
		if (aimg)
			at.vkDestroyImage(adev, aimg, nullptr);
		if (amem)
			at.vkFreeMemory(adev, amem, nullptr);
	}
	if (ctx1)
		ctx1->Release();
	if (ctx) {
		ctx->Terminate();
		ctx->Release();
	}
	if (app_amf_dev)
		vkDestroyDevice(app_amf_dev, nullptr);
	if (rd.dev) {
		rd.t.vkDeviceWaitIdle(rd.dev);
		staging.destroy(rd);
		if (rimg)
			rd.t.vkDestroyImage(rd.dev, rimg, nullptr);
		if (rmem)
			rd.t.vkFreeMemory(rd.dev, rmem, nullptr);
		rd.destroy();
	}
	if (h)
		CloseHandle(h);
	return rr;
}

} // namespace

int main()
{
	results res;
	HMODULE amf = LoadLibraryW(AMF_DLL_NAME);
	if (!amf) {
		log("amfrt64.dll not found");
		return 2;
	}
	auto query = (AMFQueryVersion_Fn)GetProcAddress(amf, AMF_QUERY_VERSION_FUNCTION_NAME);
	auto init = (AMFInit_Fn)GetProcAddress(amf, AMF_INIT_FUNCTION_NAME);
	amf_uint64 ver = 0;
	query(&ver);
	log("AMF runtime %d.%d.%d.%d, headers %d.%d.%d.%d", (int)AMF_GET_MAJOR_VERSION(ver), (int)AMF_GET_MINOR_VERSION(ver),
	    (int)AMF_GET_SUBMINOR_VERSION(ver), (int)AMF_GET_BUILD_VERSION(ver), AMF_VERSION_MAJOR, AMF_VERSION_MINOR,
	    AMF_VERSION_RELEASE, AMF_VERSION_BUILD_NUM);
	if (init(AMF_FULL_VERSION, &g_factory) != AMF_OK) {
		log("AMFInit failed");
		return 2;
	}
	amf::AMFTrace *trace = nullptr;
	if (g_factory->GetTrace(&trace) == AMF_OK && trace) {
		trace->SetGlobalLevel(AMF_TRACE_DEBUG);
		trace->SetWriterLevel(AMF_TRACE_WRITER_FILE, AMF_TRACE_DEBUG);
		trace->SetPath(L"spike4_amf_trace.log");
		trace->EnableWriter(AMF_TRACE_WRITER_FILE, true);
		trace->SetWriterLevel(AMF_TRACE_WRITER_CONSOLE, AMF_TRACE_WARNING);
		trace->EnableWriter(AMF_TRACE_WRITER_CONSOLE, true);
	}

	instance in;
	try {
		// Same instance extensions as AMF's DeviceVulkan sample. AMF resolves the KHR-suffixed aliases
		// (e.g. vkGetPhysicalDeviceProperties2KHR) even on a 1.3 instance, so the promoted extensions
		// must be enabled explicitly or InitVulkan fails with AMF_FAIL.
		in.create("spike4-amf-vulkan", {VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, VK_KHR_SURFACE_EXTENSION_NAME,
						 VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
						 VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME});
	} catch (check_failed &) {
		return 2;
	}
	for (VkPhysicalDevice pd : in.physical_devices()) {
		gpu_info g = query_gpu(pd);
		if (!g.is_amd()) {
			log("\nskipping %s (not AMD)", g.props.deviceName);
			continue;
		}
		log("\n================================================================");
		print_gpu(g);
		for (bool own : {true, false})
			for (VkImageLayout lay : {VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_UNDEFINED}) {
				std::string name = std::string(own ? "AMF-own VkDevice" : "app-created AMF VkDevice") +
						   ", told layout " + (lay == VK_IMAGE_LAYOUT_GENERAL ? "GENERAL" : "UNDEFINED");
				if (skip_case(name))
					continue;
				log("  -- %s", name.c_str());
				vvl_scope vs;
				run_result r = run(in, pd, g, own, lay);
				log("     %s: %s", r.ok ? "PASS" : "FAIL", r.note.c_str());
				res.add(g.props.deviceName, name, r.ok, r.note, vs.errors());
			}
	}
	in.destroy();
	res.print();
	return res.fails ? 1 : 0;
}
