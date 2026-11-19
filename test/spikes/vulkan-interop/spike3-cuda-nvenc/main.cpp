// Spike 3 (NVIDIA only): Vulkan -> CUDA -> NVENC without copies.
//
// Vulkan exports (OPAQUE_WIN32) either
//  (A) an NV12 VkImage (optimal tiling, the kind of image the renderer would draw into), or
//  (B) a pitch-linear NV12 VkBuffer (fallback),
// plus a timeline semaphore. CUDA imports both (cuImportExternalMemory / cuImportExternalSemaphore),
// maps the image as a CUarray (or the buffer as a CUdeviceptr) and NVENC registers that resource
// directly (NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY / CUDADEVICEPTR) and encodes H.264 from it.
// The Vulkan side fills the frames with a known pattern; if SPIKE_FFMPEG points at ffmpeg.exe the
// streams are decoded again and compared against the pattern (PSNR).
#include "spike_common.hpp"

#include <ffnvcodec/dynlink_loader.h>

#include <cmath>
#include <fstream>

using namespace spike;

namespace {

constexpr uint32_t W = 1920, H = 1080, FRAMES = 8;
#ifndef CUDA_EXTERNAL_MEMORY_DEDICATED
#define CUDA_EXTERNAL_MEMORY_DEDICATED 0x1
#endif
#ifndef CUDA_ARRAY3D_COLOR_ATTACHMENT
#define CUDA_ARRAY3D_COLOR_ATTACHMENT 0x20
#endif

CudaFunctions *cu = nullptr;
NvencFunctions *nvdl = nullptr;
NV_ENCODE_API_FUNCTION_LIST nv{NV_ENCODE_API_FUNCTION_LIST_VER};
typedef CUresult(CUDAAPI *pfn_cuArrayGetPlane)(CUarray *, CUarray, unsigned int);
pfn_cuArrayGetPlane p_cuArrayGetPlane = nullptr;

const char *cuerr(CUresult r)
{
	const char *s = nullptr;
	if (cu && cu->cuGetErrorName)
		cu->cuGetErrorName(r, &s);
	return s ? s : "?";
}

#define CUC(expr)                                                                                     \
	do {                                                                                           \
		CUresult _r = (expr);                                                                  \
		if (_r != CUDA_SUCCESS) {                                                              \
			::spike::log("  !! %s -> %s (%d) at line %d", #expr, cuerr(_r), (int)_r, __LINE__); \
			throw ::spike::check_failed{std::string(#expr) + " -> " + cuerr(_r)};         \
		}                                                                                      \
	} while (0)
#define NVC(expr)                                                                                    \
	do {                                                                                          \
		NVENCSTATUS _s = (expr);                                                              \
		if (_s != NV_ENC_SUCCESS) {                                                           \
			::spike::log("  !! %s -> NVENCSTATUS %d at line %d", #expr, (int)_s, __LINE__); \
			throw ::spike::check_failed{std::string(#expr) + " -> NVENCSTATUS " +         \
						    std::to_string((int)_s)};                         \
		}                                                                                     \
	} while (0)

uint8_t pat_y(uint32_t x, uint32_t y, uint32_t f)
{
	return (uint8_t)(16 + ((x / 4 + y / 4 + f * 8) % 200));
}
uint8_t pat_uv(uint32_t x, uint32_t y, uint32_t f, int c)
{
	return (uint8_t)(c ? 64 + ((y * 2) % 128) : 64 + ((x + f * 4) % 128));
}

void fill_nv12(uint8_t *dst, uint32_t f)
{
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++)
			dst[y * W + x] = pat_y(x, y, f);
	uint8_t *uv = dst + W * H;
	for (uint32_t y = 0; y < H / 2; y++)
		for (uint32_t x = 0; x < W / 2; x++) {
			uv[y * W + x * 2 + 0] = pat_uv(x, y, f, 0);
			uv[y * W + x * 2 + 1] = pat_uv(x, y, f, 1);
		}
}

// ---------------------------------------------------------------- NVENC session
struct encoder {
	void *enc = nullptr;
	NV_ENC_OUTPUT_PTR bs = nullptr;
	std::vector<uint8_t> stream;

	void open(CUcontext ctx)
	{
		NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op{NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER};
		op.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
		op.device = ctx;
		op.apiVersion = NVENCAPI_VERSION;
		NVC(nv.nvEncOpenEncodeSessionEx(&op, &enc));

		NV_ENC_PRESET_CONFIG pc{NV_ENC_PRESET_CONFIG_VER};
		pc.presetCfg.version = NV_ENC_CONFIG_VER;
		NVC(nv.nvEncGetEncodePresetConfigEx(enc, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID,
						    NV_ENC_TUNING_INFO_LOW_LATENCY, &pc));
		NV_ENC_CONFIG cfg = pc.presetCfg;
		cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
		cfg.rcParams.constQP = {16, 16, 16};
		cfg.gopLength = NVENC_INFINITE_GOPLENGTH;
		cfg.frameIntervalP = 1;
		NV_ENC_INITIALIZE_PARAMS ip{NV_ENC_INITIALIZE_PARAMS_VER};
		ip.encodeGUID = NV_ENC_CODEC_H264_GUID;
		ip.presetGUID = NV_ENC_PRESET_P4_GUID;
		ip.tuningInfo = NV_ENC_TUNING_INFO_LOW_LATENCY;
		ip.encodeWidth = ip.darWidth = ip.maxEncodeWidth = W;
		ip.encodeHeight = ip.darHeight = ip.maxEncodeHeight = H;
		ip.frameRateNum = 60;
		ip.frameRateDen = 1;
		ip.enablePTD = 1;
		ip.encodeConfig = &cfg;
		NVC(nv.nvEncInitializeEncoder(enc, &ip));
		NV_ENC_CREATE_BITSTREAM_BUFFER cb{NV_ENC_CREATE_BITSTREAM_BUFFER_VER};
		NVC(nv.nvEncCreateBitstreamBuffer(enc, &cb));
		bs = cb.bitstreamBuffer;
	}

	NV_ENC_REGISTERED_PTR reg(NV_ENC_INPUT_RESOURCE_TYPE type, void *res, uint32_t pitch)
	{
		NV_ENC_REGISTER_RESOURCE rr{NV_ENC_REGISTER_RESOURCE_VER};
		rr.resourceType = type;
		rr.width = W;
		rr.height = H;
		rr.pitch = pitch;
		rr.resourceToRegister = res;
		rr.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
		rr.bufferUsage = NV_ENC_INPUT_IMAGE;
		NVC(nv.nvEncRegisterResource(enc, &rr));
		return rr.registeredResource;
	}

	void encode(NV_ENC_REGISTERED_PTR r, uint32_t pitch, uint32_t f)
	{
		NV_ENC_MAP_INPUT_RESOURCE mi{NV_ENC_MAP_INPUT_RESOURCE_VER};
		mi.registeredResource = r;
		NVC(nv.nvEncMapInputResource(enc, &mi));
		NV_ENC_PIC_PARAMS pp{NV_ENC_PIC_PARAMS_VER};
		pp.inputBuffer = mi.mappedResource;
		pp.bufferFmt = mi.mappedBufferFmt;
		pp.inputWidth = W;
		pp.inputHeight = H;
		pp.inputPitch = pitch;
		pp.outputBitstream = bs;
		pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
		pp.inputTimeStamp = f;
		NVENCSTATUS s = nv.nvEncEncodePicture(enc, &pp);
		if (s != NV_ENC_SUCCESS && s != NV_ENC_ERR_NEED_MORE_INPUT) {
			nv.nvEncUnmapInputResource(enc, mi.mappedResource);
			NVC(s);
		}
		drain();
		NVC(nv.nvEncUnmapInputResource(enc, mi.mappedResource));
	}

	void drain()
	{
		NV_ENC_LOCK_BITSTREAM lb{NV_ENC_LOCK_BITSTREAM_VER};
		lb.outputBitstream = bs;
		NVC(nv.nvEncLockBitstream(enc, &lb));
		const uint8_t *p = (const uint8_t *)lb.bitstreamBufferPtr;
		stream.insert(stream.end(), p, p + lb.bitstreamSizeInBytes);
		NVC(nv.nvEncUnlockBitstream(enc, bs));
	}

	void flush()
	{
		NV_ENC_PIC_PARAMS pp{NV_ENC_PIC_PARAMS_VER};
		pp.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
		nv.nvEncEncodePicture(enc, &pp);
	}

	void close()
	{
		if (!enc)
			return;
		if (bs)
			nv.nvEncDestroyBitstreamBuffer(enc, bs);
		nv.nvEncDestroyEncoder(enc);
		enc = nullptr;
	}
};

// ---------------------------------------------------------------- verification via ffmpeg
std::string verify_with_ffmpeg(const std::string &h264, bool &ok)
{
	const char *ff = getenv("SPIKE_FFMPEG");
	if (!ff || !*ff) {
		ok = true;
		return "not decoded (set SPIKE_FFMPEG to verify)";
	}
	std::string yuv = h264 + ".decoded.yuv";
	std::string cmd = std::string("\"\"") + ff + "\" -v error -y -i \"" + h264 +
			  "\" -f rawvideo -pix_fmt nv12 \"" + yuv + "\"\"";
	int rc = system(cmd.c_str());
	std::ifstream in(yuv, std::ios::binary);
	std::vector<uint8_t> dec((std::istreambuf_iterator<char>(in)), {});
	size_t fs = (size_t)W * H * 3 / 2;
	if (rc != 0 || dec.size() < fs) {
		ok = false;
		return "ffmpeg decode failed (rc " + std::to_string(rc) + ", " + std::to_string(dec.size()) + " bytes)";
	}
	size_t nf = dec.size() / fs;
	std::vector<uint8_t> exp(fs);
	double worst_y = 1e9, worst_uv = 1e9;
	for (size_t f = 0; f < nf; f++) {
		fill_nv12(exp.data(), (uint32_t)f);
		double se_y = 0, se_uv = 0;
		for (size_t i = 0; i < (size_t)W * H; i++) {
			double d = (double)dec[f * fs + i] - exp[i];
			se_y += d * d;
		}
		for (size_t i = (size_t)W * H; i < fs; i++) {
			double d = (double)dec[f * fs + i] - exp[i];
			se_uv += d * d;
		}
		auto psnr = [](double se, size_t n) {
			double mse = se / n;
			return mse <= 1e-10 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
		};
		worst_y = std::min(worst_y, psnr(se_y, (size_t)W * H));
		worst_uv = std::min(worst_uv, psnr(se_uv, fs - (size_t)W * H));
	}
	ok = nf == FRAMES && worst_y > 35 && worst_uv > 35;
	char b[160];
	snprintf(b, sizeof b, "decoded %zu frames, worst PSNR Y %.1f dB / UV %.1f dB vs pattern", nf, worst_y, worst_uv);
	return b;
}

// ---------------------------------------------------------------- Vulkan export helpers
HANDLE export_memory(device &d, VkDeviceMemory m)
{
	VkMemoryGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
	gi.memory = m;
	gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
	HANDLE h = nullptr;
	VKC(d.t.vkGetMemoryWin32HandleKHR(d.dev, &gi, &h));
	return h;
}

struct timeline {
	VkSemaphore sem = VK_NULL_HANDLE;
	HANDLE h = nullptr;
	CUexternalSemaphore cs = nullptr;
	void create(device &d)
	{
		VkExportSemaphoreCreateInfo es{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
		es.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkSemaphoreTypeCreateInfo st{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
		st.pNext = &es;
		st.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
		VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		si.pNext = &st;
		VKC(d.t.vkCreateSemaphore(d.dev, &si, nullptr, &sem));
		VkSemaphoreGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
		gi.semaphore = sem;
		gi.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VKC(d.t.vkGetSemaphoreWin32HandleKHR(d.dev, &gi, &h));
		CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC sd{};
		sd.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_WIN32;
		sd.handle.win32.handle = h;
		CUC(cu->cuImportExternalSemaphore(&cs, &sd));
	}
	void cuda_wait(CUstream s, uint64_t v)
	{
		CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS wp{};
		wp.params.fence.value = v;
		CUC(cu->cuWaitExternalSemaphoresAsync(&cs, &wp, 1, s));
	}
	void destroy(device &d)
	{
		if (cs)
			cu->cuDestroyExternalSemaphore(cs);
		if (h)
			CloseHandle(h);
		if (sem)
			d.t.vkDestroySemaphore(d.dev, sem, nullptr);
		cs = nullptr;
		h = nullptr;
		sem = VK_NULL_HANDLE;
	}
};

void submit_signal(device &d, VkCommandBuffer cb, timeline &tl, uint64_t value)
{
	VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
	ts.signalSemaphoreValueCount = 1;
	ts.pSignalSemaphoreValues = &value;
	d.submit_wait(cb, &ts, tl.sem);
}

struct run_result {
	bool ok = false;
	std::string note;
};

// Sanity check for the CUDA readback itself and for the NV12 luma layout:
// a VkImage of 'fmt' (R8 or NV12) is filled with the frame-0 luma pattern and mapped in CUDA as a
// plain UINT8 1-channel W x H array, then read back with cuMemcpy2D.
run_result run_r8_sanity(device &d, VkFormat fmt)
{
	run_result rr;
	VkImage img = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	HANDLE h = nullptr;
	CUexternalMemory em = nullptr;
	CUmipmappedArray mip = nullptr;
	buffer staging;
	try {
		VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.pNext = &emi;
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = fmt;
		ii.extent = {W, H, 1};
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		VKC(d.t.vkCreateImage(d.dev, &ii, nullptr, &img));
		VkMemoryRequirements mr;
		d.t.vkGetImageMemoryRequirements(d.dev, img, &mr);
		VkMemoryDedicatedAllocateInfo dai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
		dai.image = img;
		VkExportMemoryAllocateInfo ea{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
		ea.pNext = &dai;
		ea.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.pNext = &ea;
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = d.find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VKC(d.t.vkAllocateMemory(d.dev, &ai, nullptr, &mem));
		VKC(d.t.vkBindImageMemory(d.dev, img, mem, 0));
		h = export_memory(d, mem);
		staging.create_host(d, (VkDeviceSize)W * H * 3 / 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
		fill_nv12((uint8_t *)staging.map, 0);
		bool nv12 = fmt == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
		VkImageAspectFlags asp = nv12 ? VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
		VkCommandBuffer cb = d.begin();
		image_barrier(d, cb, img, asp, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			      VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		VkBufferImageCopy cp[2]{};
		cp[0].imageSubresource = {nv12 ? (VkImageAspectFlags)VK_IMAGE_ASPECT_PLANE_0_BIT : (VkImageAspectFlags)VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		cp[0].imageExtent = {W, H, 1};
		cp[1].bufferOffset = W * H;
		cp[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
		cp[1].imageExtent = {W / 2, H / 2, 1};
		d.t.vkCmdCopyBufferToImage(cb, staging.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nv12 ? 2 : 1, cp);
		image_barrier(d, cb, img, asp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
			      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0, d.qf,
			      VK_QUEUE_FAMILY_EXTERNAL);
		d.submit_wait(cb);

		CUDA_EXTERNAL_MEMORY_HANDLE_DESC md{};
		md.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
		md.handle.win32.handle = h;
		md.size = mr.size;
		md.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
		CUC(cu->cuImportExternalMemory(&em, &md));
		CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC ad{};
		ad.arrayDesc.Width = W;
		ad.arrayDesc.Height = H;
		ad.arrayDesc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
		ad.arrayDesc.NumChannels = 1;
		ad.numLevels = 1;
		CUC(cu->cuExternalMemoryGetMappedMipmappedArray(&mip, em, &ad));
		CUarray arr;
		CUC(cu->cuMipmappedArrayGetLevel(&arr, mip, 0));
		std::vector<uint8_t> host((size_t)W * H, 0xCD);
		CUDA_MEMCPY2D c{};
		c.srcMemoryType = CU_MEMORYTYPE_ARRAY;
		c.srcArray = arr;
		c.dstMemoryType = CU_MEMORYTYPE_HOST;
		c.dstHost = host.data();
		c.dstPitch = W;
		c.WidthInBytes = W;
		c.Height = H;
		CUC(cu->cuMemcpy2D(&c));
		size_t bad = 0;
		for (size_t i = 0; i < host.size(); i++)
			bad += host[i] != ((const uint8_t *)staging.map)[i];
		rr.ok = bad == 0;
		rr.note = "allocation " + std::to_string(mr.size) + " bytes, UINT8x1 " + std::to_string(W) + "x" +
			  std::to_string(H) + " array readback: " + std::to_string(bad) + " bad luma bytes";
	} catch (check_failed &e) {
		rr.ok = false;
		rr.note = "exception: " + e.what;
	}
	d.t.vkDeviceWaitIdle(d.dev);
	if (mip)
		cu->cuMipmappedArrayDestroy(mip);
	if (em)
		cu->cuDestroyExternalMemory(em);
	if (h)
		CloseHandle(h);
	staging.destroy(d);
	if (img)
		d.t.vkDestroyImage(d.dev, img, nullptr);
	if (mem)
		d.t.vkFreeMemory(d.dev, mem, nullptr);
	return rr;
}

// (A) NV12 image -> CUarray -> NVENC CUDAARRAY
run_result run_image(device &d, CUcontext ctx, CUstream stream, VkImageCreateFlags img_flags,
		     VkImageUsageFlags img_usage, const std::string &tag)
{
	run_result rr;
	VkImage img = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	HANDLE h = nullptr;
	CUexternalMemory em = nullptr;
	CUmipmappedArray mip = nullptr;
	buffer staging;
	timeline tl;
	encoder enc;
	NV_ENC_REGISTERED_PTR reg = nullptr;
	try {
		VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.pNext = &emi;
		ii.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
		ii.extent = {W, H, 1};
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
			   VK_IMAGE_USAGE_SAMPLED_BIT;
		{
			VkPhysicalDeviceExternalImageFormatInfo ei{
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
			ei.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
			VkPhysicalDeviceImageFormatInfo2 ifi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
			ifi.pNext = &ei;
			ifi.format = ii.format;
			ifi.type = ii.imageType;
			ifi.tiling = ii.tiling;
			ifi.usage = ii.usage;
			ifi.flags = ii.flags;
			VkExternalImageFormatProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
			VkImageFormatProperties2 ip{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
			ip.pNext = &ep;
			VkResult r = vkGetPhysicalDeviceImageFormatProperties2(d.pd, &ifi, &ip);
			log("     NV12 OPAQUE_WIN32 export props: %s, %s", vkres(r),
			    ext_mem_features_str(ep.externalMemoryProperties.externalMemoryFeatures));
		}
		VKC(d.t.vkCreateImage(d.dev, &ii, nullptr, &img));
		VkMemoryRequirements mr;
		d.t.vkGetImageMemoryRequirements(d.dev, img, &mr);
		VkMemoryDedicatedAllocateInfo dai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
		dai.image = img;
		VkExportMemoryAllocateInfo ea{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
		ea.pNext = &dai;
		ea.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.pNext = &ea;
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = d.find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VKC(d.t.vkAllocateMemory(d.dev, &ai, nullptr, &mem));
		VKC(d.t.vkBindImageMemory(d.dev, img, mem, 0));
		h = export_memory(d, mem);
		log("     image allocation %llu bytes (NV12 payload %u)", (unsigned long long)mr.size, W * H * 3 / 2);

		CUDA_EXTERNAL_MEMORY_HANDLE_DESC md{};
		md.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
		md.handle.win32.handle = h;
		md.size = mr.size;
		md.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
		CUC(cu->cuImportExternalMemory(&em, &md));

		tl.create(d);
		enc.open(ctx);

		staging.create_host(d, (VkDeviceSize)W * H * 3 / 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
		auto upload = [&](uint32_t f) {
			fill_nv12((uint8_t *)staging.map, f);
			VkCommandBuffer cb = d.begin();
			VkImageAspectFlags asp = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;
			if (f == 0)
				image_barrier(d, cb, img, asp, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					      VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
					      VK_ACCESS_2_TRANSFER_WRITE_BIT);
			else
				image_barrier(d, cb, img, asp, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
					      VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
					      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_EXTERNAL, d.qf);
			VkBufferImageCopy cp[2]{};
			cp[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
			cp[0].imageExtent = {W, H, 1};
			cp[1].bufferOffset = W * H;
			cp[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
			cp[1].imageExtent = {W / 2, H / 2, 1};
			d.t.vkCmdCopyBufferToImage(cb, staging.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, cp);
			image_barrier(d, cb, img, asp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
				      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0,
				      d.qf, VK_QUEUE_FAMILY_EXTERNAL);
			submit_signal(d, cb, tl, f + 1);
			tl.cuda_wait(stream, f + 1);
			CUC(cu->cuStreamSynchronize(stream));
		};
		upload(0);

		// Read a CUarray's planes back to the host and count mismatches against frame 0.
		auto cuda_check = [&](CUarray arr) -> std::string {
			if (!p_cuArrayGetPlane)
				return "cuArrayGetPlane unavailable";
			std::vector<uint8_t> host((size_t)W * H * 3 / 2, 0xCD), exp((size_t)W * H * 3 / 2);
			fill_nv12(exp.data(), 0);
			for (unsigned p = 0; p < 2; p++) {
				CUarray pl = nullptr;
				CUresult r = p_cuArrayGetPlane(&pl, arr, p);
				if (r != CUDA_SUCCESS)
					return std::string("cuArrayGetPlane ") + cuerr(r);
				CUDA_MEMCPY2D c{};
				c.srcMemoryType = CU_MEMORYTYPE_ARRAY;
				c.srcArray = pl;
				c.dstMemoryType = CU_MEMORYTYPE_HOST;
				c.dstHost = host.data() + (p ? (size_t)W * H : 0);
				c.dstPitch = W;
				c.WidthInBytes = W;
				c.Height = p ? H / 2 : H;
				r = cu->cuMemcpy2D(&c);
				if (r != CUDA_SUCCESS)
					return std::string("cuMemcpy2D ") + cuerr(r);
			}
			size_t bad_y = 0, bad_uv = 0;
			for (size_t i = 0; i < (size_t)W * H; i++)
				bad_y += host[i] != exp[i];
			for (size_t i = (size_t)W * H; i < host.size(); i++)
				bad_uv += host[i] != exp[i];
			return "CUDA readback: " + std::to_string(bad_y) + " bad Y, " + std::to_string(bad_uv) + " bad UV bytes";
		};

		// Try array descriptors; prefer one whose CUDA view matches what Vulkan wrote and NVENC accepts.
		struct attempt {
			unsigned ch, flags;
		};
		const attempt attempts[] = {
			{3, CUDA_ARRAY3D_COLOR_ATTACHMENT | CUDA_ARRAY3D_VIDEO_ENCODE_DECODE},
			{3, CUDA_ARRAY3D_VIDEO_ENCODE_DECODE},
			{3, CUDA_ARRAY3D_COLOR_ATTACHMENT},
			{3, 0},
			{3, CUDA_ARRAY3D_SURFACE_LDST},
			{3, CUDA_ARRAY3D_SURFACE_LDST | CUDA_ARRAY3D_VIDEO_ENCODE_DECODE},
			{1, CUDA_ARRAY3D_VIDEO_ENCODE_DECODE},
			{1, 0},
		};
		std::string tried;
		bool content_ok = false;
		for (auto &a : attempts) {
			CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC ad{};
			ad.offset = 0;
			ad.arrayDesc.Width = W;
			ad.arrayDesc.Height = H;
			ad.arrayDesc.Depth = 0;
			ad.arrayDesc.Format = CU_AD_FORMAT_NV12;
			ad.arrayDesc.NumChannels = a.ch;
			ad.arrayDesc.Flags = a.flags;
			ad.numLevels = 1;
			CUmipmappedArray m = nullptr;
			CUresult r = cu->cuExternalMemoryGetMappedMipmappedArray(&m, em, &ad);
			char b[256];
			if (r != CUDA_SUCCESS) {
				snprintf(b, sizeof b, "[ch%u flags 0x%x: map %s] ", a.ch, a.flags, cuerr(r));
				tried += b;
				log("     %s", b);
				continue;
			}
			CUarray arr = nullptr;
			CUC(cu->cuMipmappedArrayGetLevel(&arr, m, 0));
			std::string chk = cuda_check(arr);
			bool match = chk.find(": 0 bad Y, 0 bad UV") != std::string::npos;
			NV_ENC_REGISTER_RESOURCE rgs{NV_ENC_REGISTER_RESOURCE_VER};
			rgs.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY;
			rgs.width = W;
			rgs.height = H;
			rgs.pitch = W;
			rgs.resourceToRegister = arr;
			rgs.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
			rgs.bufferUsage = NV_ENC_INPUT_IMAGE;
			NVENCSTATUS s = nv.nvEncRegisterResource(enc.enc, &rgs);
			snprintf(b, sizeof b, "[ch%u flags 0x%x: map OK, %s, nvEncRegisterResource %d] ", a.ch, a.flags,
				 chk.c_str(), (int)s);
			tried += b;
			log("     %s", b);
			bool take = s == NV_ENC_SUCCESS && (match || !reg);
			if (take) {
				if (reg) {
					nv.nvEncUnregisterResource(enc.enc, reg);
					cu->cuMipmappedArrayDestroy(mip);
				}
				reg = rgs.registeredResource;
				mip = m;
				content_ok = match;
				if (match)
					break;
			} else {
				if (s == NV_ENC_SUCCESS)
					nv.nvEncUnregisterResource(enc.enc, rgs.registeredResource);
				cu->cuMipmappedArrayDestroy(m);
			}
		}
		if (!reg)
			throw check_failed{"no CUDA array descriptor worked: " + tried};
		log("     using the %s descriptor", content_ok ? "content-matching" : "first registrable (content NOT matching)");

		if (!content_ok && !getenv("SPIKE3_ENCODE_MISMATCH"))
			throw check_failed{"CUDA view of the image does not match what Vulkan wrote; not encoding (set "
					   "SPIKE3_ENCODE_MISMATCH=1 to encode anyway). " + tried};
		for (uint32_t f = 0; f < FRAMES; f++) {
			if (f)
				upload(f);
			enc.encode(reg, W, f);
		}
		enc.flush();
		std::string path = "spike3_cudaarray_" + tag + ".h264";
		std::ofstream(path, std::ios::binary).write((const char *)enc.stream.data(), enc.stream.size());
		bool vok;
		std::string v = verify_with_ffmpeg(path, vok);
		rr.ok = vok && !enc.stream.empty();
		rr.ok = rr.ok && content_ok;
		rr.note = tried + "| " + std::to_string(enc.stream.size()) + " bytes H.264, " + v;
	} catch (check_failed &e) {
		rr.ok = false;
		rr.note = "exception: " + e.what;
	}
	d.t.vkDeviceWaitIdle(d.dev);
	if (reg)
		nv.nvEncUnregisterResource(enc.enc, reg);
	enc.close();
	if (mip)
		cu->cuMipmappedArrayDestroy(mip);
	if (em)
		cu->cuDestroyExternalMemory(em);
	if (h)
		CloseHandle(h);
	tl.destroy(d);
	staging.destroy(d);
	if (img)
		d.t.vkDestroyImage(d.dev, img, nullptr);
	if (mem)
		d.t.vkFreeMemory(d.dev, mem, nullptr);
	return rr;
}

// (B) pitch-linear NV12 buffer -> CUdeviceptr -> NVENC CUDADEVICEPTR
run_result run_buffer(device &d, CUcontext ctx, CUstream stream)
{
	run_result rr;
	VkBuffer buf = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	HANDLE h = nullptr;
	CUexternalMemory em = nullptr;
	CUdeviceptr dptr = 0;
	buffer staging;
	timeline tl;
	encoder enc;
	NV_ENC_REGISTERED_PTR reg = nullptr;
	const uint32_t pitch = W; // 1920 is already 256-aligned
	try {
		VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
		eb.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bi.pNext = &eb;
		bi.size = (VkDeviceSize)pitch * H * 3 / 2;
		bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		VKC(d.t.vkCreateBuffer(d.dev, &bi, nullptr, &buf));
		VkMemoryRequirements mr;
		d.t.vkGetBufferMemoryRequirements(d.dev, buf, &mr);
		VkMemoryDedicatedAllocateInfo dai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
		dai.buffer = buf;
		VkExportMemoryAllocateInfo ea{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
		ea.pNext = &dai;
		ea.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.pNext = &ea;
		ai.allocationSize = mr.size;
		ai.memoryTypeIndex = d.find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VKC(d.t.vkAllocateMemory(d.dev, &ai, nullptr, &mem));
		VKC(d.t.vkBindBufferMemory(d.dev, buf, mem, 0));
		h = export_memory(d, mem);

		CUDA_EXTERNAL_MEMORY_HANDLE_DESC md{};
		md.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32;
		md.handle.win32.handle = h;
		md.size = mr.size;
		md.flags = CUDA_EXTERNAL_MEMORY_DEDICATED;
		CUC(cu->cuImportExternalMemory(&em, &md));
		CUDA_EXTERNAL_MEMORY_BUFFER_DESC bd{};
		bd.offset = 0;
		bd.size = bi.size;
		CUC(cu->cuExternalMemoryGetMappedBuffer(&dptr, em, &bd));

		tl.create(d);
		enc.open(ctx);
		reg = enc.reg(NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR, (void *)dptr, pitch);

		staging.create_host(d, bi.size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
		for (uint32_t f = 0; f < FRAMES; f++) {
			fill_nv12((uint8_t *)staging.map, f);
			VkCommandBuffer cb = d.begin();
			VkBufferCopy c{0, 0, bi.size};
			d.t.vkCmdCopyBuffer(cb, staging.buf, buf, 1, &c);
			VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
			bb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
			bb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
			bb.srcQueueFamilyIndex = d.qf;
			bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
			bb.buffer = buf;
			bb.size = VK_WHOLE_SIZE;
			VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
			di.bufferMemoryBarrierCount = 1;
			di.pBufferMemoryBarriers = &bb;
			d.t.vkCmdPipelineBarrier2(cb, &di);
			submit_signal(d, cb, tl, f + 1);
			tl.cuda_wait(stream, f + 1);
			CUC(cu->cuStreamSynchronize(stream));
			enc.encode(reg, pitch, f);
		}
		enc.flush();
		std::string path = "spike3_cudadeviceptr.h264";
		std::ofstream(path, std::ios::binary).write((const char *)enc.stream.data(), enc.stream.size());
		bool vok;
		std::string v = verify_with_ffmpeg(path, vok);
		rr.ok = vok && !enc.stream.empty();
		rr.note = std::to_string(enc.stream.size()) + " bytes H.264, " + v;
	} catch (check_failed &e) {
		rr.ok = false;
		rr.note = "exception: " + e.what;
	}
	d.t.vkDeviceWaitIdle(d.dev);
	if (reg)
		nv.nvEncUnregisterResource(enc.enc, reg);
	enc.close();
	if (dptr)
		cu->cuMemFree(dptr);
	if (em)
		cu->cuDestroyExternalMemory(em);
	if (h)
		CloseHandle(h);
	tl.destroy(d);
	staging.destroy(d);
	if (buf)
		d.t.vkDestroyBuffer(d.dev, buf, nullptr);
	if (mem)
		d.t.vkFreeMemory(d.dev, mem, nullptr);
	return rr;
}

} // namespace

int main()
{
	results res;
	if (cuda_load_functions(&cu, nullptr) < 0 || !cu->cuImportExternalMemory) {
		log("could not load nvcuda.dll (or it lacks external memory entry points)");
		return 2;
	}
	if (nvenc_load_functions(&nvdl, nullptr) < 0) {
		log("could not load nvEncodeAPI64.dll");
		return 2;
	}
	uint32_t maxver = 0;
	nvdl->NvEncodeAPIGetMaxSupportedVersion(&maxver);
	log("NVENC: header API %d.%d, driver max %u.%u", NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION, maxver >> 4,
	    maxver & 0xF);
	if (nvdl->NvEncodeAPICreateInstance(&nv) != NV_ENC_SUCCESS) {
		log("NvEncodeAPICreateInstance failed");
		return 2;
	}
	p_cuArrayGetPlane = (pfn_cuArrayGetPlane)GetProcAddress(GetModuleHandleA("nvcuda.dll"), "cuArrayGetPlane");
	CUresult cr = cu->cuInit(0);
	int drv = 0;
	cu->cuDriverGetVersion(&drv);
	log("CUDA: cuInit %d, driver API version %d", (int)cr, drv);

	instance in;
	try {
		in.create("spike3-cuda-nvenc");
	} catch (check_failed &) {
		return 2;
	}
	for (VkPhysicalDevice pd : in.physical_devices()) {
		gpu_info g = query_gpu(pd);
		if (!g.is_nvidia()) {
			log("\nskipping %s (not NVIDIA)", g.props.deviceName);
			continue;
		}
		log("\n================================================================");
		print_gpu(g);
		// Find the CUDA device with the same UUID.
		int n = 0;
		cu->cuDeviceGetCount(&n);
		CUdevice cud = -1;
		for (int i = 0; i < n; i++) {
			CUdevice dv;
			cu->cuDeviceGet(&dv, i);
			CUuuid u{};
			if (cu->cuDeviceGetUuid_v2)
				cu->cuDeviceGetUuid_v2(&u, dv);
			else
				cu->cuDeviceGetUuid(&u, dv);
			if (!memcmp(u.bytes, g.id.deviceUUID, 16))
				cud = dv;
		}
		if (cud < 0) {
			res.add(g.props.deviceName, "CUDA device by UUID", false, "no CUDA device with matching UUID");
			continue;
		}
		log("  CUDA device %d matches the Vulkan deviceUUID", (int)cud);
		CUcontext ctx = nullptr;
		CUstream stream = nullptr;
		device d;
		try {
			CUC(cu->cuCtxCreate(&ctx, 0, cud));
			CUC(cu->cuStreamCreate(&stream, 0));
			d.create(pd, {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME});
		} catch (check_failed &e) {
			res.add(g.props.deviceName, "setup", false, e.what);
			continue;
		}
		struct img_variant {
			const char *tag;
			VkImageCreateFlags flags;
			VkImageUsageFlags usage;
		};
		const img_variant variants[] = {
			{"mut+ext_ca+td+s", VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT,
			 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT},
			{"plain_td+s", 0, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT},
			{"plain_td", 0, VK_IMAGE_USAGE_TRANSFER_DST_BIT},
			{"mut+ext_storage+td", VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT,
			 VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT},
		};
		for (VkFormat sf : {VK_FORMAT_R8_UNORM, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM}) {
			std::string name = sf == VK_FORMAT_R8_UNORM ? "sanity R8 image -> UINT8 CUarray"
								    : "sanity NV12 image luma -> UINT8 CUarray";
			if (skip_case(name))
				continue;
			log("  -- %s", name.c_str());
			vvl_scope vs;
			run_result r = run_r8_sanity(d, sf);
			log("     %s: %s", r.ok ? "PASS" : "FAIL", r.note.c_str());
			res.add(g.props.deviceName, name, r.ok, r.note, vs.errors());
		}
		for (auto &v : variants) {
			std::string name = std::string("NV12 image -> CUDAARRAY ") + v.tag;
			if (skip_case(name))
				continue;
			log("  -- (A) %s", name.c_str());
			vvl_scope vs;
			run_result r = run_image(d, ctx, stream, v.flags, v.usage, v.tag);
			log("     %s: %s", r.ok ? "PASS" : "FAIL", r.note.c_str());
			res.add(g.props.deviceName, name, r.ok, r.note, vs.errors());
		}
		{
			log("  -- (B) NV12 VkBuffer -> CUdeviceptr -> NVENC CUDADEVICEPTR");
			vvl_scope vs;
			run_result r = run_buffer(d, ctx, stream);
			log("     %s: %s", r.ok ? "PASS" : "FAIL", r.note.c_str());
			res.add(g.props.deviceName, "NV12 buffer -> CUDADEVICEPTR", r.ok, r.note, vs.errors());
		}
		d.destroy();
		cu->cuStreamDestroy(stream);
		cu->cuCtxDestroy(ctx);
	}
	in.destroy();
	res.print();
	return res.fails ? 1 : 0;
}
