// Spike E: Vulkan memory + timeline semaphore exported as OPAQUE_FD, imported into CUDA (cuImportExternalMemory /
// cuImportExternalSemaphore), encoded with NVENC (H.264) without a copy. Two inputs:
//   buffer: pitch-linear NV12 VkBuffer as NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR (what worked on Windows)
//   image:  optimal-tiled R8 VkImage W x 1.5H (NV12 bytes) as CUarray, NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY
// Writes <out>/<variant>.h264 and ref.nv12; PSNR is measured with ffmpeg by run.sh / the README.
#include "sem.hpp"

#include <ffnvcodec/dynlink_loader.h>
#include <unistd.h>

#include <string>

static CudaFunctions *cu;
static NvencFunctions *nvf;
static NV_ENCODE_API_FUNCTION_LIST nv{NV_ENCODE_API_FUNCTION_LIST_VER};

#define CU_CHECK(x)                                                                         \
	do {                                                                                \
		CUresult r_ = (x);                                                          \
		if (r_ != CUDA_SUCCESS) {                                                   \
			const char *n_ = "?";                                               \
			cu->cuGetErrorName(r_, &n_);                                        \
			fprintf(stderr, "%s:%d: %s -> %s (%d)\n", __FILE__, __LINE__, #x, n_, r_); \
			return false;                                                       \
		}                                                                           \
	} while (0)
#define NV_CHECK(x)                                                                                    \
	do {                                                                                           \
		NVENCSTATUS s_ = (x);                                                                  \
		if (s_ != NV_ENC_SUCCESS) {                                                            \
			fprintf(stderr, "%s:%d: %s -> NVENCSTATUS %d (%s)\n", __FILE__, __LINE__, #x, s_, \
				enc ? nv.nvEncGetLastErrorString(enc) : "");                          \
			return false;                                                                  \
		}                                                                                      \
	} while (0)

static const uint32_t W = 640, H = 368, FRAMES = 30;

static void frame_pattern(uint32_t f, uint8_t *dst, uint32_t pitch)
{
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) {
			uint32_t bx = ((x + f * 4) / 32 + y / 32) & 1;
			dst[y * pitch + x] = (uint8_t)(bx ? 200 : 40) + (uint8_t)((x + y + f) & 15);
		}
	uint8_t *uv = dst + pitch * H;
	for (uint32_t y = 0; y < H / 2; y++)
		for (uint32_t x = 0; x < W / 2; x++) {
			uv[y * pitch + x * 2] = (uint8_t)(128 + 60 * ((x + f) % 64 < 32 ? 1 : -1));
			uv[y * pitch + x * 2 + 1] = (uint8_t)(128 + (int)((y * 2 + f) % 96) - 48);
		}
}

struct Encoder {
	void *enc = nullptr;
	NV_ENC_OUTPUT_PTR bs = nullptr;
	NV_ENC_REGISTERED_PTR reg = nullptr;

	bool open(CUcontext ctx, CUstream stream)
	{
		NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS op{NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER};
		op.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
		op.device = ctx;
		op.apiVersion = NVENCAPI_VERSION;
		NV_CHECK(nv.nvEncOpenEncodeSessionEx(&op, &enc));
		NV_ENC_PRESET_CONFIG pc{NV_ENC_PRESET_CONFIG_VER, {NV_ENC_CONFIG_VER}};
		NV_CHECK(nv.nvEncGetEncodePresetConfigEx(enc, NV_ENC_CODEC_H264_GUID, NV_ENC_PRESET_P4_GUID,
							 NV_ENC_TUNING_INFO_HIGH_QUALITY, &pc));
		NV_ENC_CONFIG cfg = pc.presetCfg;
		cfg.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
		cfg.rcParams.constQP = {18, 18, 18};
		cfg.gopLength = NVENC_INFINITE_GOPLENGTH;
		cfg.frameIntervalP = 1;
		NV_ENC_INITIALIZE_PARAMS ip{NV_ENC_INITIALIZE_PARAMS_VER};
		ip.encodeGUID = NV_ENC_CODEC_H264_GUID;
		ip.presetGUID = NV_ENC_PRESET_P4_GUID;
		ip.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
		ip.encodeWidth = ip.darWidth = W;
		ip.encodeHeight = ip.darHeight = H;
		ip.frameRateNum = 30;
		ip.frameRateDen = 1;
		ip.enablePTD = 1;
		ip.encodeConfig = &cfg;
		NV_CHECK(nv.nvEncInitializeEncoder(enc, &ip));
		NV_ENC_CREATE_BITSTREAM_BUFFER cb{NV_ENC_CREATE_BITSTREAM_BUFFER_VER};
		NV_CHECK(nv.nvEncCreateBitstreamBuffer(enc, &cb));
		bs = cb.bitstreamBuffer;
		// NVENC reads the input on `stream`, so the CUDA wait on the Vulkan semaphore orders it.
		NV_CHECK(nv.nvEncSetIOCudaStreams(enc, (NV_ENC_CUSTREAM_PTR)&stream, (NV_ENC_CUSTREAM_PTR)&stream));
		return true;
	}
	bool register_input(NV_ENC_INPUT_RESOURCE_TYPE type, void *res, uint32_t pitch)
	{
		NV_ENC_REGISTER_RESOURCE rr{NV_ENC_REGISTER_RESOURCE_VER};
		rr.resourceType = type;
		rr.resourceToRegister = res;
		rr.width = W;
		rr.height = H;
		rr.pitch = pitch;
		rr.bufferFormat = NV_ENC_BUFFER_FORMAT_NV12;
		rr.bufferUsage = NV_ENC_INPUT_IMAGE;
		NV_CHECK(nv.nvEncRegisterResource(enc, &rr));
		reg = rr.registeredResource;
		return true;
	}
	bool encode(FILE *out, uint32_t frame)
	{
		NV_ENC_MAP_INPUT_RESOURCE mp{NV_ENC_MAP_INPUT_RESOURCE_VER};
		mp.registeredResource = reg;
		NV_CHECK(nv.nvEncMapInputResource(enc, &mp));
		NV_ENC_PIC_PARAMS pp{NV_ENC_PIC_PARAMS_VER};
		pp.inputBuffer = mp.mappedResource;
		pp.bufferFmt = mp.mappedBufferFmt;
		pp.inputWidth = W;
		pp.inputHeight = H;
		pp.outputBitstream = bs;
		pp.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
		pp.inputTimeStamp = frame;
		NV_CHECK(nv.nvEncEncodePicture(enc, &pp));
		NV_ENC_LOCK_BITSTREAM lb{NV_ENC_LOCK_BITSTREAM_VER};
		lb.outputBitstream = bs;
		NV_CHECK(nv.nvEncLockBitstream(enc, &lb));
		fwrite(lb.bitstreamBufferPtr, 1, lb.bitstreamSizeInBytes, out);
		NV_CHECK(nv.nvEncUnlockBitstream(enc, bs));
		NV_CHECK(nv.nvEncUnmapInputResource(enc, mp.mappedResource));
		return true;
	}
	bool flush(FILE *out)
	{
		NV_ENC_PIC_PARAMS pp{NV_ENC_PIC_PARAMS_VER};
		pp.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
		NV_CHECK(nv.nvEncEncodePicture(enc, &pp));
		(void)out;
		return true;
	}
	void close()
	{
		if (!enc)
			return;
		if (reg)
			nv.nvEncUnregisterResource(enc, reg);
		if (bs)
			nv.nvEncDestroyBitstreamBuffer(enc, bs);
		nv.nvEncDestroyEncoder(enc);
		enc = nullptr;
	}
};

// Shared per variant: Vulkan writes frame f into the exported memory and signals 2f+1; CUDA waits 2f+1, NVENC
// encodes, CUDA signals 2f+2; Vulkan waits 2f+2 before reusing the memory.
static bool run_variant(Ctx &c, CUcontext ctx, bool image, const std::string &outdir)
{
	const char *name = image ? "image-cuarray" : "buffer-devptr";
	printf("--- %s\n", name);
	const uint32_t pitch = image ? W : (W + 255) / 256 * 256;
	const VkDeviceSize size = (VkDeviceSize)pitch * H * 3 / 2;
	const auto OFD = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

	VkBuffer buf = VK_NULL_HANDLE;
	VkImage img = VK_NULL_HANDLE;
	VkMemoryRequirements mr;
	VkExternalMemoryBufferCreateInfo eb{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
	eb.handleTypes = OFD;
	VkExternalMemoryImageCreateInfo eim{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	eim.handleTypes = OFD;
	if (!image) {
		VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		bci.pNext = &eb;
		bci.size = size;
		bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
		VK_CHECK(vkCreateBuffer(c.dev, &bci, nullptr, &buf));
		vkGetBufferMemoryRequirements(c.dev, buf, &mr);
	} else {
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.pNext = &eim;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = VK_FORMAT_R8_UNORM;
		ici.extent = {W, H * 3 / 2, 1};
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		VK_CHECK(vkCreateImage(c.dev, &ici, nullptr, &img));
		vkGetImageMemoryRequirements(c.dev, img, &mr);
	}
	VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	ded.buffer = buf;
	ded.image = img;
	VkExportMemoryAllocateInfo ex{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
	ex.pNext = &ded;
	ex.handleTypes = OFD;
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = &ex;
	mai.allocationSize = mr.size;
	mai.memoryTypeIndex = c.mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VkDeviceMemory mem;
	VK_CHECK(vkAllocateMemory(c.dev, &mai, nullptr, &mem));
	if (buf)
		VK_CHECK(vkBindBufferMemory(c.dev, buf, mem, 0));
	else
		VK_CHECK(vkBindImageMemory(c.dev, img, mem, 0));
	VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
	gi.memory = mem;
	gi.handleType = OFD;
	int memfd = -1;
	VK_CHECK(c.dfn<PFN_vkGetMemoryFdKHR>("vkGetMemoryFdKHR")(c.dev, &gi, &memfd));

	VkSemaphore sem = make_sem(c, true, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
	int semfd = -1;
	VK_CHECK(sem_export(c, sem, VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, &semfd));

	CU_CHECK(cu->cuCtxPushCurrent(ctx));
	CUexternalMemory extmem;
	CUDA_EXTERNAL_MEMORY_HANDLE_DESC md{};
	md.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
	md.handle.fd = memfd; // CUDA owns the fd on success
	md.size = mr.size;
	md.flags = 1; // CUDA_EXTERNAL_MEMORY_DEDICATED
	CUresult cr = cu->cuImportExternalMemory(&extmem, &md);
	check(cr == CUDA_SUCCESS, "%s: cuImportExternalMemory(OPAQUE_FD, %llu bytes) = %d", name,
	      (unsigned long long)mr.size, cr);
	if (cr != CUDA_SUCCESS)
		return false;
	CUexternalSemaphore extsem;
	CUDA_EXTERNAL_SEMAPHORE_HANDLE_DESC sd{};
	sd.type = CU_EXTERNAL_SEMAPHORE_HANDLE_TYPE_TIMELINE_SEMAPHORE_FD;
	sd.handle.fd = semfd;
	cr = cu->cuImportExternalSemaphore(&extsem, &sd);
	check(cr == CUDA_SUCCESS, "%s: cuImportExternalSemaphore(TIMELINE_SEMAPHORE_FD) = %d", name, cr);
	if (cr != CUDA_SUCCESS)
		return false;

	CUdeviceptr dptr = 0;
	CUmipmappedArray mip = nullptr;
	CUarray arr = nullptr;
	if (!image) {
		CUDA_EXTERNAL_MEMORY_BUFFER_DESC bd{};
		bd.size = size;
		cr = cu->cuExternalMemoryGetMappedBuffer(&dptr, extmem, &bd);
		check(cr == CUDA_SUCCESS, "%s: cuExternalMemoryGetMappedBuffer = %d", name, cr);
	} else {
		CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC ad{};
		ad.arrayDesc.Width = W;
		ad.arrayDesc.Height = H * 3 / 2;
		ad.arrayDesc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
		ad.arrayDesc.NumChannels = 1;
		ad.numLevels = 1;
		cr = cu->cuExternalMemoryGetMappedMipmappedArray(&mip, extmem, &ad);
		check(cr == CUDA_SUCCESS, "%s: cuExternalMemoryGetMappedMipmappedArray = %d", name, cr);
		if (cr == CUDA_SUCCESS) {
			cr = cu->cuMipmappedArrayGetLevel(&arr, mip, 0);
			check(cr == CUDA_SUCCESS, "%s: cuMipmappedArrayGetLevel = %d", name, cr);
		}
	}
	if (cr != CUDA_SUCCESS)
		return false;
	CUstream stream;
	CU_CHECK(cu->cuStreamCreate(&stream, 0));

	Encoder e;
	bool ok = e.open(ctx, stream);
	check(ok, "%s: NVENC session (CUDA device) opened", name);
	if (ok) {
		ok = e.register_input(image ? NV_ENC_INPUT_RESOURCE_TYPE_CUDAARRAY : NV_ENC_INPUT_RESOURCE_TYPE_CUDADEVICEPTR,
				      image ? (void *)arr : (void *)dptr, pitch);
		check(ok, "%s: nvEncRegisterResource(%s)", name, image ? "CUDAARRAY" : "CUDADEVICEPTR");
	}
	cu->cuCtxPopCurrent(nullptr);

	std::string path = outdir + "/" + name + ".h264";
	FILE *out = fopen(path.c_str(), "wb");
	FILE *ref = fopen((outdir + "/ref.nv12").c_str(), "wb");
	Ctx::Buffer staging = c.host_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
	std::vector<uint8_t> tight(W * H * 3 / 2);
	bool all = ok;
	for (uint32_t f = 0; all && f < FRAMES; f++) {
		// Vulkan side: wait until CUDA is done with the previous frame, write frame f, signal 2f+1.
		if (f)
			VK_CHECK(host_wait(c, sem, 2 * f, 5000000000ull));
		frame_pattern(f, (uint8_t *)staging.map, pitch);
		for (uint32_t y = 0; y < H * 3 / 2; y++)
			memcpy(&tight[y * W], (uint8_t *)staging.map + y * pitch, W);
		fwrite(tight.data(), 1, tight.size(), ref);
		VkCommandBuffer cb = c.begin();
		if (!image) {
			VkBufferCopy bc{0, 0, size};
			vkCmdCopyBuffer(cb, staging.buf, buf, 1, &bc);
		} else {
			image_barrier(cb, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
				      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
				      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
			VkBufferImageCopy bic{};
			bic.bufferRowLength = pitch;
			bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			bic.imageExtent = {W, H * 3 / 2, 1};
			vkCmdCopyBufferToImage(cb, staging.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
			// CUDA expects the image in GENERAL; hand it over (external queue family)
			image_barrier(cb, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				      VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT,
				      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0, c.qfam,
				      VK_QUEUE_FAMILY_EXTERNAL);
		}
		c.submit(cb, {}, {sem_info(sem, 2 * f + 1)}, false);
		// CUDA/NVENC side: GPU wait on 2f+1 in the stream NVENC reads its input on.
		cu->cuCtxPushCurrent(ctx);
		CUDA_EXTERNAL_SEMAPHORE_WAIT_PARAMS wp{};
		wp.params.fence.value = 2 * f + 1;
		cr = cu->cuWaitExternalSemaphoresAsync(&extsem, &wp, 1, stream);
		if (cr != CUDA_SUCCESS) {
			check(false, "%s: cuWaitExternalSemaphoresAsync = %d", name, cr);
			all = false;
		}
		all = all && e.encode(out, f);
		CUDA_EXTERNAL_SEMAPHORE_SIGNAL_PARAMS sp{};
		sp.params.fence.value = 2 * f + 2;
		cr = cu->cuSignalExternalSemaphoresAsync(&extsem, &sp, 1, stream);
		if (cr != CUDA_SUCCESS) {
			check(false, "%s: cuSignalExternalSemaphoresAsync = %d", name, cr);
			all = false;
		}
		cu->cuCtxPopCurrent(nullptr);
		vkQueueWaitIdle(c.q);
	}
	if (all) {
		cu->cuCtxPushCurrent(ctx);
		e.flush(out);
		cu->cuCtxPopCurrent(nullptr);
		all = host_wait(c, sem, 2 * FRAMES, 5000000000ull) == VK_SUCCESS;
	}
	check(all, "%s: %u frames encoded with GPU-side semaphore handoff Vulkan -> CUDA/NVENC -> Vulkan", name, FRAMES);
	fclose(out);
	fclose(ref);
	printf("  wrote %s\n", path.c_str());

	cu->cuCtxPushCurrent(ctx);
	e.close();
	cu->cuStreamSynchronize(stream);
	cu->cuStreamDestroy(stream);
	if (mip)
		cu->cuMipmappedArrayDestroy(mip);
	if (dptr)
		cu->cuMemFree(dptr);
	cu->cuDestroyExternalSemaphore(extsem);
	cu->cuDestroyExternalMemory(extmem);
	cu->cuCtxPopCurrent(nullptr);
	c.free_buffer(staging);
	vkDestroySemaphore(c.dev, sem, nullptr);
	if (buf)
		vkDestroyBuffer(c.dev, buf, nullptr);
	if (img)
		vkDestroyImage(c.dev, img, nullptr);
	vkFreeMemory(c.dev, mem, nullptr);
	return all;
}

static bool cuda_main(Ctx &c, const std::string &outdir)
{
	if (cuda_load_functions(&cu, nullptr) < 0 || nvenc_load_functions(&nvf, nullptr) < 0) {
		check(false, "load libcuda / libnvidia-encode");
		return false;
	}
	CU_CHECK(cu->cuInit(0));
	int drv = 0;
	cu->cuDriverGetVersion(&drv);
	uint32_t maxv = 0;
	nvf->NvEncodeAPIGetMaxSupportedVersion(&maxv);
	printf("CUDA driver API %d.%d, NVENC API max %u.%u (headers %u.%u)\n", drv / 1000, drv % 1000 / 10, maxv >> 4,
	       maxv & 15, NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
	void *enc = nullptr; // for NV_CHECK
	(void)enc;
	if (nvf->NvEncodeAPICreateInstance(&nv) != NV_ENC_SUCCESS) {
		check(false, "NvEncodeAPICreateInstance");
		return false;
	}
	VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
	VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	p2.pNext = &idp;
	vkGetPhysicalDeviceProperties2(c.phys, &p2);
	int n = 0;
	CU_CHECK(cu->cuDeviceGetCount(&n));
	CUdevice dev = -1;
	for (int i = 0; i < n; i++) {
		CUdevice d;
		CUuuid u;
		cu->cuDeviceGet(&d, i);
		cu->cuDeviceGetUuid(&u, d);
		if (!memcmp(u.bytes, idp.deviceUUID, VK_UUID_SIZE))
			dev = d;
	}
	check(dev >= 0, "CUDA device matched to the Vulkan device by UUID");
	if (dev < 0)
		return false;
	CUcontext ctx;
	CU_CHECK(cu->cuCtxCreate(&ctx, CU_CTX_SCHED_BLOCKING_SYNC, dev));
	cu->cuCtxPopCurrent(nullptr);
	bool a = run_variant(c, ctx, false, outdir);
	bool b = run_variant(c, ctx, true, outdir);
	cu->cuCtxDestroy(ctx);
	return a && b;
}

int main(int argc, char **argv)
{
	std::string outdir = argc > 1 ? argv[1] : ".";
	Ctx c;
	c.init({VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME});
	print_driver(c);
	cuda_main(c, outdir);
	int rc = finish(c, "spike E");
	c.destroy();
	cuda_free_functions(&cu);
	nvenc_free_functions(&nvf);
	return rc;
}
