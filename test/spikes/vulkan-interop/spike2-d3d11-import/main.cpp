// Spike 2: import D3D11 shared textures into Vulkan.
//
// For each GPU: find the DXGI adapter with the same LUID, create D3D11 textures shared as
//  (a) legacy D3D11_RESOURCE_MISC_SHARED (KMT handle via IDXGIResource::GetSharedHandle, as game capture does)
//  (b) D3D11_RESOURCE_MISC_SHARED_NTHANDLE (NT handle via IDXGIResource1::CreateSharedHandle)
//  (c) (b) + keyed mutex, and legacy KMT + keyed mutex
//  (d) NV12 textures (NT and KMT)
// import them with VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE(_KMT)_BIT into a dedicated allocation,
// copy every plane to a buffer and verify against what D3D11 wrote.
#include "spike_common.hpp"

#include <d3d11_1.h>
#include <dxgi1_6.h>

using namespace spike;

namespace {

uint32_t W = 128, H = 64; // per case, see k_nv12_variants

template<class T> void release(T *&p)
{
	if (p) {
		p->Release();
		p = nullptr;
	}
}

#define HRC(expr)                                                                                    \
	do {                                                                                          \
		HRESULT _h = (expr);                                                                  \
		if (FAILED(_h)) {                                                                     \
			::spike::log("  !! %s -> HRESULT 0x%08lx at %s:%d", #expr, (unsigned long)_h, \
				     __FILE__, __LINE__);                                             \
			throw ::spike::check_failed{#expr};                                           \
		}                                                                                     \
	} while (0)

enum class handle_kind { kmt, nt };

struct test_case {
	const char *name;
	DXGI_FORMAT dxgi;
	VkFormat vk;
	UINT misc;
	handle_kind hk;
	VkExternalMemoryHandleTypeFlagBits ht;
	bool keyed_mutex;
	uint32_t w = 128, h = 64;
	VkImageCreateFlags vk_flags = 0;
	bool d3d_rt = false;
	std::string label() const
	{
		std::string s = name;
		if (dxgi == DXGI_FORMAT_NV12)
			s += ' ' + std::to_string(w) + 'x' + std::to_string(h) + (vk_flags ? " mut+ext" : "") + (d3d_rt ? " RT" : "");
		return s;
	}
};

const test_case k_cases[] = {
	{"BGRA MISC_SHARED (KMT)", DXGI_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, D3D11_RESOURCE_MISC_SHARED,
	 handle_kind::kmt, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, false},
	{"BGRA SHARED_NTHANDLE (NT)", DXGI_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
	 D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, handle_kind::nt,
	 VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, false},
	{"BGRA NTHANDLE+KEYEDMUTEX (NT)", DXGI_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
	 D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, handle_kind::nt,
	 VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, true},
	{"BGRA KEYEDMUTEX legacy (KMT)", DXGI_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
	 D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, handle_kind::kmt, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT,
	 true},
	{"NV12 SHARED_NTHANDLE (NT)", DXGI_FORMAT_NV12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM,
	 D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, handle_kind::nt,
	 VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, false},
	{"NV12 MISC_SHARED (KMT)", DXGI_FORMAT_NV12, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, D3D11_RESOURCE_MISC_SHARED,
	 handle_kind::kmt, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, false},
};

// BGRA pattern; 'gen' distinguishes the initial data from the later UpdateSubresource.
uint32_t bgra_px(uint32_t x, uint32_t y, int gen)
{
	uint8_t b = (uint8_t)(x * 2 + gen * 7), g = (uint8_t)(y * 3 + gen * 13), r = (uint8_t)((x ^ y) + gen * 31);
	return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
uint8_t nv12_y(uint32_t x, uint32_t y)
{
	return (uint8_t)(16 + ((x + y * 2) % 220));
}
uint8_t nv12_uv(uint32_t x, uint32_t y, int c)
{
	return (uint8_t)(c ? 255 - x * 3 - y : 30 + x * 2 + y * 3);
}

struct d3d {
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	std::string adapter;

	bool create(const uint8_t luid[VK_LUID_SIZE])
	{
		IDXGIFactory4 *f = nullptr;
		if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f))))
			return false;
		LUID l;
		memcpy(&l, luid, sizeof l);
		IDXGIAdapter1 *a = nullptr;
		HRESULT hr = f->EnumAdapterByLuid(l, IID_PPV_ARGS(&a));
		f->Release();
		if (FAILED(hr)) {
			log("  no DXGI adapter with this LUID (0x%08lx)", (unsigned long)hr);
			return false;
		}
		DXGI_ADAPTER_DESC1 ad;
		a->GetDesc1(&ad);
		char n[128];
		WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, n, sizeof n, nullptr, nullptr);
		adapter = n;
		D3D_FEATURE_LEVEL fl[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
		hr = D3D11CreateDevice(a, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fl, 2,
				       D3D11_SDK_VERSION, &dev, nullptr, &ctx);
		a->Release();
		if (FAILED(hr)) {
			log("  D3D11CreateDevice failed 0x%08lx", (unsigned long)hr);
			return false;
		}
		log("  D3D11 device on DXGI adapter \"%s\" (LUID matched)", adapter.c_str());
		return true;
	}

	void flush_and_wait()
	{
		D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
		ID3D11Query *q = nullptr;
		dev->CreateQuery(&qd, &q);
		ctx->End(q);
		ctx->Flush();
		BOOL done = FALSE;
		while (ctx->GetData(q, &done, sizeof done, 0) != S_OK || !done)
			Sleep(0);
		q->Release();
	}

	void destroy()
	{
		release(ctx);
		release(dev);
	}
};

struct run_result {
	bool ok = false;
	std::string note;
};

void report_ext_props(VkPhysicalDevice pd, const test_case &tc)
{
	VkPhysicalDeviceExternalImageFormatInfo ei{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
	ei.handleType = tc.ht;
	VkPhysicalDeviceImageFormatInfo2 ifi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
	ifi.pNext = &ei;
	ifi.format = tc.vk;
	ifi.type = VK_IMAGE_TYPE_2D;
	ifi.tiling = VK_IMAGE_TILING_OPTIMAL;
	ifi.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	ifi.flags = tc.vk_flags;
	VkExternalImageFormatProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
	VkImageFormatProperties2 ip{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
	ip.pNext = &ep;
	VkResult r = vkGetPhysicalDeviceImageFormatProperties2(pd, &ifi, &ip);
	log("     external image format props: %s, features: %s, compatibleHandleTypes 0x%x", vkres(r),
	    ext_mem_features_str(ep.externalMemoryProperties.externalMemoryFeatures),
	    ep.externalMemoryProperties.compatibleHandleTypes);
}

run_result run_case(device &d, d3d &dx, const test_case &tc, bool have_keyed_mutex_ext)
{
	run_result rr;
	ID3D11Texture2D *tex = nullptr;
	IDXGIKeyedMutex *km = nullptr;
	HANDLE h = nullptr;
	VkImage img = VK_NULL_HANDLE;
	VkDeviceMemory mem = VK_NULL_HANDLE;
	buffer rb;
	bool nv12 = tc.dxgi == DXGI_FORMAT_NV12;
	W = tc.w;
	H = tc.h;
	try {
		if (tc.keyed_mutex && !have_keyed_mutex_ext)
			throw check_failed{"VK_KHR_win32_keyed_mutex not supported"};
		// --- D3D11 side
		std::vector<uint8_t> init;
		UINT pitch;
		if (nv12) {
			pitch = W;
			init.resize(W * H * 3 / 2);
			for (uint32_t y = 0; y < H; y++)
				for (uint32_t x = 0; x < W; x++)
					init[y * W + x] = nv12_y(x, y);
			for (uint32_t y = 0; y < H / 2; y++)
				for (uint32_t x = 0; x < W / 2; x++) {
					init[W * H + y * W + x * 2 + 0] = nv12_uv(x, y, 0);
					init[W * H + y * W + x * 2 + 1] = nv12_uv(x, y, 1);
				}
		} else {
			pitch = W * 4;
			init.resize(W * H * 4);
			for (uint32_t y = 0; y < H; y++)
				for (uint32_t x = 0; x < W; x++)
					((uint32_t *)init.data())[y * W + x] = bgra_px(x, y, 0);
		}
		D3D11_TEXTURE2D_DESC td{};
		td.Width = W;
		td.Height = H;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = tc.dxgi;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | ((!nv12 || tc.d3d_rt) ? D3D11_BIND_RENDER_TARGET : 0);
		td.MiscFlags = tc.misc;
		D3D11_SUBRESOURCE_DATA sd{init.data(), pitch, 0};
		HRC(dx.dev->CreateTexture2D(&td, &sd, &tex));

		if (!nv12) {
			// Overwrite after creation so the check proves later D3D11 writes are visible, not just init data.
			std::vector<uint32_t> upd(W * H);
			for (uint32_t y = 0; y < H; y++)
				for (uint32_t x = 0; x < W; x++)
					upd[y * W + x] = bgra_px(x, y, 1);
			if (tc.keyed_mutex) {
				HRC(tex->QueryInterface(IID_PPV_ARGS(&km)));
				HRC(km->AcquireSync(0, 1000));
			}
			dx.ctx->UpdateSubresource(tex, 0, nullptr, upd.data(), W * 4, 0);
			if (tc.keyed_mutex)
				HRC(km->ReleaseSync(1));
		} else if (tc.keyed_mutex) {
			HRC(tex->QueryInterface(IID_PPV_ARGS(&km)));
			HRC(km->AcquireSync(0, 1000));
			HRC(km->ReleaseSync(1));
		}
		dx.flush_and_wait();

		if (tc.hk == handle_kind::kmt) {
			IDXGIResource *r = nullptr;
			HRC(tex->QueryInterface(IID_PPV_ARGS(&r)));
			HRESULT hr = r->GetSharedHandle(&h);
			r->Release();
			HRC(hr);
		} else {
			IDXGIResource1 *r = nullptr;
			HRC(tex->QueryInterface(IID_PPV_ARGS(&r)));
			HRESULT hr = r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
							   nullptr, &h);
			r->Release();
			HRC(hr);
		}

		// --- Vulkan side
		VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
		VKC(d.t.vkGetMemoryWin32HandlePropertiesKHR(d.dev, tc.ht, h, &hp));
		log("     vkGetMemoryWin32HandlePropertiesKHR memoryTypeBits 0x%x", hp.memoryTypeBits);

		VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		emi.handleTypes = tc.ht;
		VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ii.pNext = &emi;
		ii.flags = tc.vk_flags;
		ii.imageType = VK_IMAGE_TYPE_2D;
		ii.format = tc.vk;
		ii.extent = {W, H, 1};
		ii.mipLevels = 1;
		ii.arrayLayers = 1;
		ii.samples = VK_SAMPLE_COUNT_1_BIT;
		ii.tiling = VK_IMAGE_TILING_OPTIMAL;
		ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VKC(d.t.vkCreateImage(d.dev, &ii, nullptr, &img));

		VkMemoryDedicatedRequirements dr{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
		VkMemoryRequirements2 mr{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
		mr.pNext = &dr;
		VkImageMemoryRequirementsInfo2 mri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
		mri.image = img;
		d.t.vkGetImageMemoryRequirements2(d.dev, &mri, &mr);
		log("     image memReq size %llu typeBits 0x%x, dedicated prefers %d requires %d",
		    (unsigned long long)mr.memoryRequirements.size, mr.memoryRequirements.memoryTypeBits,
		    dr.prefersDedicatedAllocation, dr.requiresDedicatedAllocation);
		uint32_t bits = mr.memoryRequirements.memoryTypeBits & hp.memoryTypeBits;
		if (!bits)
			throw check_failed{"no memory type in common between image and handle"};
		uint32_t mt = 0;
		while (!(bits & (1u << mt)))
			mt++;

		VkMemoryDedicatedAllocateInfo dai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
		dai.image = img;
		VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
		imp.pNext = &dai;
		imp.handleType = tc.ht;
		imp.handle = h;
		VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		ai.pNext = &imp;
		ai.allocationSize = mr.memoryRequirements.size;
		ai.memoryTypeIndex = mt;
		VKC(d.t.vkAllocateMemory(d.dev, &ai, nullptr, &mem));
		VKC(d.t.vkBindImageMemory(d.dev, img, mem, 0));

		VkDeviceSize sz = nv12 ? W * H * 3 / 2 : W * H * 4;
		rb.create_host(d, sz, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
		memset(rb.map, 0xCD, (size_t)sz);

		VkImageAspectFlags asp = nv12 ? (VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT)
					      : VK_IMAGE_ASPECT_COLOR_BIT;
		VkCommandBuffer cb = d.begin();
		// Acquire from the external (D3D11) owner, keep contents (oldLayout GENERAL, not UNDEFINED).
		image_barrier(d, cb, img, asp, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			      VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
			      VK_QUEUE_FAMILY_EXTERNAL, d.qf);
		VkBufferImageCopy cp[2]{};
		uint32_t ncp = 1;
		if (nv12) {
			cp[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
			cp[0].imageExtent = {W, H, 1};
			cp[1].bufferOffset = W * H;
			cp[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
			cp[1].imageExtent = {W / 2, H / 2, 1};
			ncp = 2;
		} else {
			cp[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			cp[0].imageExtent = {W, H, 1};
		}
		d.t.vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, ncp, cp);
		// Release back to the external owner.
		image_barrier(d, cb, img, asp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
			      VK_PIPELINE_STAGE_2_COPY_BIT, 0, VK_PIPELINE_STAGE_2_NONE, 0, d.qf,
			      VK_QUEUE_FAMILY_EXTERNAL);

		VkWin32KeyedMutexAcquireReleaseInfoKHR kmi{VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR};
		uint64_t acq_key = 1, rel_key = 2;
		uint32_t timeout = 1000;
		if (tc.keyed_mutex) {
			kmi.acquireCount = 1;
			kmi.pAcquireSyncs = &mem;
			kmi.pAcquireKeys = &acq_key;
			kmi.pAcquireTimeouts = &timeout;
			kmi.releaseCount = 1;
			kmi.pReleaseSyncs = &mem;
			kmi.pReleaseKeys = &rel_key;
		}
		d.submit_wait(cb, tc.keyed_mutex ? &kmi : nullptr);

		std::string km_note;
		if (tc.keyed_mutex) {
			HRESULT hr = km->AcquireSync(2, 1000);
			km_note = hr == S_OK ? "; D3D11 re-acquired key 2 released by Vulkan"
					     : "; D3D11 AcquireSync(2) did NOT succeed (hr " + std::to_string((long)hr) + ")";
			if (hr == S_OK)
				km->ReleaseSync(0);
			else
				rr.note += "keyed mutex hand-back failed; ";
		}

		// --- verify
		int bad = 0;
		char first[160] = "";
		const uint8_t *p = (const uint8_t *)rb.map;
		if (nv12) {
			for (uint32_t y = 0; y < H; y++)
				for (uint32_t x = 0; x < W; x++)
					if (p[y * W + x] != nv12_y(x, y) && !bad++)
						snprintf(first, sizeof first, " first Y(%u,%u) got %u exp %u", x, y,
							 p[y * W + x], nv12_y(x, y));
			for (uint32_t y = 0; y < H / 2; y++)
				for (uint32_t x = 0; x < W / 2; x++)
					for (int c = 0; c < 2; c++) {
						uint8_t g = p[W * H + (y * (W / 2) + x) * 2 + c];
						if (g != nv12_uv(x, y, c) && !bad++)
							snprintf(first, sizeof first, " first UV(%u,%u).%d got %u exp %u",
								 x, y, c, g, nv12_uv(x, y, c));
					}
			if (bad) {
				// Diagnostics: where does the received chroma come from?
				std::string got, exp;
				for (int i = 0; i < 12; i++) {
					got += std::to_string(p[W * H + i]) + ' ';
					exp += std::to_string(nv12_uv(i / 2, 0, i & 1)) + ' ';
				}
				log("     UV row0 got: %s", got.c_str());
				log("     UV row0 exp: %s", exp.c_str());
				int ybad = 0;
				for (uint32_t i = 0; i < W * H; i++)
					if (p[i] != nv12_y(i % W, i / W))
						ybad++;
				log("     Y plane bad texels: %d", ybad);
			}
		} else {
			int stale = 0;
			for (uint32_t y = 0; y < H; y++)
				for (uint32_t x = 0; x < W; x++) {
					uint32_t g = ((const uint32_t *)p)[y * W + x];
					if (g != bgra_px(x, y, 1)) {
						if (g == bgra_px(x, y, 0))
							stale++;
						if (!bad++)
							snprintf(first, sizeof first, " first (%u,%u) got %08x exp %08x", x,
								 y, g, bgra_px(x, y, 1));
					}
				}
			if (stale)
				km_note += "; " + std::to_string(stale) + " texels still show the pre-update data";
		}
		bool km_ok = !tc.keyed_mutex || km_note.find("re-acquired") != std::string::npos;
		rr.ok = bad == 0 && km_ok;
		rr.note += "memType " + std::to_string(mt) + ", " + std::to_string(bad) + " bad texels" + first + km_note;
	} catch (check_failed &e) {
		rr.ok = false;
		rr.note += "exception: " + e.what;
	}
	d.t.vkDeviceWaitIdle(d.dev);
	rb.destroy(d);
	if (img)
		d.t.vkDestroyImage(d.dev, img, nullptr);
	if (mem)
		d.t.vkFreeMemory(d.dev, mem, nullptr);
	if (h && tc.hk == handle_kind::nt)
		CloseHandle(h);
	release(km);
	release(tex);
	return rr;
}

} // namespace

int main()
{
	results res;
	instance in;
	try {
		in.create("spike2-d3d11-import");
	} catch (check_failed &) {
		return 2;
	}
	for (VkPhysicalDevice pd : in.physical_devices()) {
		gpu_info g = query_gpu(pd);
		if (g.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU || skip_gpu(g.props.deviceName))
			continue;
		log("\n================================================================");
		print_gpu(g);
		std::vector<const char *> exts = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME};
		bool have_km = has_device_ext(pd, VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME);
		log("  VK_KHR_win32_keyed_mutex: %s", have_km ? "yes" : "NO");
		if (have_km)
			exts.push_back(VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME);
		if (!g.id.deviceLUIDValid) {
			res.add(g.props.deviceName, "LUID", false, "deviceLUIDValid = false");
			continue;
		}
		d3d dx;
		if (!dx.create(g.id.deviceLUID)) {
			res.add(g.props.deviceName, "D3D11 device", false, "could not create D3D11 device by LUID");
			continue;
		}
		device d;
		try {
			d.create(pd, exts);
		} catch (check_failed &) {
			res.add(g.props.deviceName, "device creation", false, "vkCreateDevice failed");
			dx.destroy();
			continue;
		}
		std::vector<test_case> cases;
		for (auto &tc : k_cases) {
			if (tc.dxgi != DXGI_FORMAT_NV12) {
				cases.push_back(tc);
				continue;
			}
			// NV12 variants: size, Vulkan create flags, D3D11 bind flags.
			for (auto wh : {std::pair<uint32_t, uint32_t>{128, 64}, {1920, 1080}})
				for (VkImageCreateFlags fl :
				     {(VkImageCreateFlags)0,
				      (VkImageCreateFlags)(VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)})
					for (bool rt : {false, true}) {
						test_case v = tc;
						v.w = wh.first;
						v.h = wh.second;
						v.vk_flags = fl;
						v.d3d_rt = rt;
						cases.push_back(v);
					}
		}
		for (auto &tc : cases) {
			std::string label = tc.label();
			if (skip_case(label))
				continue;
			log("  -- %s", label.c_str());
			report_ext_props(pd, tc);
			vvl_scope vs;
			run_result r = run_case(d, dx, tc, have_km);
			log("     %s: %s", r.ok ? "PASS" : "FAIL", r.note.c_str());
			res.add(g.props.deviceName, label, r.ok, r.note, vs.errors());
		}
		d.destroy();
		dx.destroy();
	}
	in.destroy();
	res.print();
	return res.fails ? 1 : 0;
}
