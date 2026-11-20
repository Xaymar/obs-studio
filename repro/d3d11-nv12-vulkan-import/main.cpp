// Minimal reproducer: D3D11 NV12 shared texture imported into Vulkan as VK_FORMAT_G8_B8R8_2PLANE_420_UNORM.
//
// 1. Create a D3D11 device on the DXGI adapter whose LUID matches the selected VkPhysicalDevice.
// 2. Create a shared DXGI_FORMAT_NV12 texture (NT handle: MISC_SHARED | MISC_SHARED_NTHANDLE,
//    KMT handle: MISC_SHARED) with a known pattern as initial data, and check it with a D3D11 staging readback.
// 3. Import it with VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT (NT) or ..._D3D11_TEXTURE_KMT_BIT (KMT)
//    into a dedicated allocation bound to a VK_FORMAT_G8_B8R8_2PLANE_420_UNORM image (optimal tiling).
// 4. Acquire from VK_QUEUE_FAMILY_EXTERNAL, copy PLANE_0 and PLANE_1 to a host-visible buffer, compare.
//
// Every row of the D3D11 data has unique content, so a wrong plane offset shows up as "Vulkan row r
// contains D3D11 row r+k". One case per process. Exit code: 0 match, 1 mismatch, 2 setup error.
//
// Usage: d3d11-nv12-vulkan-import [--handle nt|kmt] [--size WxH] [--gpu <index|name substring>] [--rt] [--list]
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <d3d11_1.h>
#include <dxgi1_6.h>

#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// ---------------------------------------------------------------- Vulkan entry points (vulkan-1.dll, loaded at runtime)
#define VK_GLOBAL_FNS(X) X(vkCreateInstance) X(vkEnumerateInstanceVersion)
#define VK_INSTANCE_FNS(X)                                                                                  \
	X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties2)               \
	X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties)                 \
	X(vkGetPhysicalDeviceImageFormatProperties2) X(vkGetPhysicalDeviceFeatures2) X(vkCreateDevice)     \
	X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#define VK_DEVICE_FNS(X)                                                                                     \
	X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateImage) X(vkDestroyImage)                           \
	X(vkGetImageMemoryRequirements2) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory)           \
	X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory)         \
	X(vkMapMemory) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers)           \
	X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyImageToBuffer)     \
	X(vkCreateFence) X(vkDestroyFence) X(vkQueueSubmit) X(vkWaitForFences) X(vkDeviceWaitIdle)          \
	X(vkGetMemoryWin32HandlePropertiesKHR)
#define DECL(f) PFN_##f f = nullptr;
VK_GLOBAL_FNS(DECL)
VK_INSTANCE_FNS(DECL)
VK_DEVICE_FNS(DECL)
#undef DECL
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

[[noreturn]] void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fprintf(stdout, "SETUP ERROR: ");
	vfprintf(stdout, fmt, ap);
	fprintf(stdout, "\n");
	va_end(ap);
	fflush(stdout);
	exit(2);
}

const char *vkres(VkResult r)
{
	switch (r) {
	case VK_SUCCESS: return "VK_SUCCESS";
	case VK_TIMEOUT: return "VK_TIMEOUT";
	case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
	case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
	case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
	case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
	case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
	default: return "VkResult(other)";
	}
}
#define VKC(expr)                                                                         \
	do {                                                                              \
		VkResult _r = (expr);                                                     \
		if (_r != VK_SUCCESS)                                                     \
			die("%s -> %s (%d) at line %d", #expr, vkres(_r), (int)_r, __LINE__); \
	} while (0)
#define HRC(expr)                                                                                    \
	do {                                                                                         \
		HRESULT _h = (expr);                                                                 \
		if (FAILED(_h))                                                                      \
			die("%s -> HRESULT 0x%08lx at line %d", #expr, (unsigned long)_h, __LINE__); \
	} while (0)

// ---------------------------------------------------------------- test pattern
// The NV12 data is treated as H*3/2 "rows" of W bytes: rows 0..H-1 are luma, rows H..H*3/2-1 are the
// interleaved CbCr rows. Every byte is a hash of (row, byte column), so each row is unique and a row found
// in the wrong place can be traced back to where it came from.
uint8_t pattern(uint32_t row, uint32_t col)
{
	uint32_t h = row * 0x9E3779B1u ^ (col + 1) * 0x85EBCA77u;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	return (uint8_t)(h >> 24);
}

void fill_nv12(std::vector<uint8_t> &v, uint32_t w, uint32_t h)
{
	v.resize((size_t)w * h * 3 / 2);
	for (uint32_t r = 0; r < h * 3 / 2; r++)
		for (uint32_t c = 0; c < w; c++)
			v[(size_t)r * w + c] = pattern(r, c);
}

std::string bytes_str(const uint8_t *p, size_t n)
{
	std::string s;
	char b[8];
	for (size_t i = 0; i < n; i++) {
		snprintf(b, sizeof b, "%02x ", p[i]);
		s += b;
	}
	return s;
}

struct options {
	bool nt = true;
	uint32_t w = 1920, h = 1080;
	std::string gpu = "0";
	bool rt = false;
	bool list = false;
};

options parse(int argc, char **argv)
{
	options o;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		auto next = [&]() -> std::string {
			if (i + 1 >= argc)
				die("missing value after %s", a.c_str());
			return argv[++i];
		};
		if (a == "--handle") {
			std::string v = next();
			if (v == "nt")
				o.nt = true;
			else if (v == "kmt")
				o.nt = false;
			else
				die("--handle must be nt or kmt");
		} else if (a == "--size") {
			std::string v = next();
			if (sscanf(v.c_str(), "%ux%u", &o.w, &o.h) != 2 || !o.w || !o.h || (o.w & 1) || (o.h & 1))
				die("--size must be WxH with even W and H");
		} else if (a == "--gpu") {
			o.gpu = next();
		} else if (a == "--rt") {
			o.rt = true;
		} else if (a == "--list") {
			o.list = true;
		} else {
			printf("usage: %s [--handle nt|kmt] [--size WxH] [--gpu <index|name substring>] [--rt] [--list]\n"
			       "  defaults: --handle nt --size 1920x1080 --gpu 0\n", argv[0]);
			exit(a == "--help" || a == "-h" ? 0 : 2);
		}
	}
	return o;
}

std::string windows_build()
{
	using fn_t = LONG(WINAPI *)(OSVERSIONINFOW *);
	auto fn = (fn_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion");
	OSVERSIONINFOW vi{sizeof vi};
	if (!fn || fn(&vi) != 0)
		return "unknown";
	DWORD ubr = 0, sz = sizeof ubr;
	RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "UBR", RRF_RT_REG_DWORD,
		     nullptr, &ubr, &sz);
	char b[64];
	snprintf(b, sizeof b, "%lu.%lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber, ubr);
	return b;
}

std::string vk_version(uint32_t v)
{
	char b[32];
	snprintf(b, sizeof b, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
	return b;
}

// Summarises where each received row came from, as runs: "rows a..b = D3D11 chroma rows c..d".
void trace_rows(const char *what, const uint8_t *got, uint32_t rows, uint32_t w, uint32_t h, uint32_t expect_first,
		const std::unordered_map<std::string, uint32_t> &index, const std::vector<uint8_t> &ref,
		int &detected_offset)
{
	auto name = [&](uint32_t vr) {
		char b[48];
		if (vr < h)
			snprintf(b, sizeof b, "D3D11 luma row %u", vr);
		else
			snprintf(b, sizeof b, "D3D11 chroma row %u", vr - h);
		return std::string(b);
	};
	const int64_t none = INT64_MIN;
	auto src_of = [&](uint32_t r) -> int64_t {
		auto it = index.find(std::string((const char *)got + (size_t)r * w, w));
		return it == index.end() ? none : (int64_t)it->second;
	};
	detected_offset = INT32_MIN;
	uint32_t r = 0;
	int runs = 0;
	while (r < rows) {
		int64_t s = src_of(r);
		uint32_t e = r + 1;
		while (e < rows) {
			int64_t t = src_of(e);
			if (s == none ? t != none : t != s + (int64_t)(e - r))
				break;
			e++;
		}
		if (runs < 12) {
			if (s == none) {
				printf("  %s rows %u..%u: no matching D3D11 row (row %u starts: %s)\n", what, r, e - 1, r,
				       bytes_str(got + (size_t)r * w, w < 12 ? w : 12).c_str());
			} else {
				printf("  %s rows %u..%u = %s..%s%s\n", what, r, e - 1, name((uint32_t)s).c_str(),
				       name((uint32_t)(s + (e - 1 - r))).c_str(),
				       (uint32_t)s == expect_first + r ? "  (correct)" : "");
			}
		} else if (runs == 12) {
			printf("  %s: more runs not shown\n", what);
		}
		if (r == 0 && s != none)
			detected_offset = (int)(s - (int64_t)expect_first);
		runs++;
		r = e;
	}
}

// Byte-level trace for data that is not row aligned: walks the received plane and splits it into
// segments that are contiguous copies of the D3D11 linear data (pitch w), printing where each came from.
// Returns the D3D11 linear offset that received byte 0 came from (-1 if not found).
long long trace_bytes(const char *what, const uint8_t *got, size_t n, uint32_t w, uint32_t h, const std::vector<uint8_t> &ref)
{
	long long first_src = -1;
	size_t segments = 0, max_len = 0;
	std::unordered_map<uint64_t, uint32_t> win; // 8-byte window -> first linear offset in ref
	win.reserve(ref.size());
	for (size_t i = 0; i + 8 <= ref.size(); i++) {
		uint64_t k;
		memcpy(&k, ref.data() + i, 8);
		win.emplace(k, (uint32_t)i);
	}
	auto where = [&](size_t off) {
		char b[96];
		size_t ysz = (size_t)w * h;
		if (off < ysz)
			snprintf(b, sizeof b, "luma row %zu + %zu", off / w, off % w);
		else
			snprintf(b, sizeof b, "chroma row %zu + %zu", (off - ysz) / w, (off - ysz) % w);
		return std::string(b);
	};
	printf("  %s byte trace (received plane offset <- D3D11 linear offset, pitch %u):\n", what, w);
	size_t p = 0, unmatched = 0, matched = 0;
	int shown = 0;
	long long last_delta = LLONG_MIN;
	size_t same_run = 0;
	while (p + 8 <= n) {
		uint64_t k;
		memcpy(&k, got + p, 8);
		auto it = win.find(k);
		if (it == win.end()) {
			p++;
			unmatched++;
			continue;
		}
		size_t s = it->second, len = 0;
		while (p + len < n && s + len < ref.size() && got[p + len] == ref[s + len])
			len++;
		long long delta = (long long)s - (long long)p;
		if (p == 0)
			first_src = (long long)s;
		segments++;
		if (len > max_len)
			max_len = len;
		if (delta == last_delta) {
			same_run++;
		} else {
			if (same_run && shown < 24)
				printf("    ... %zu more segments with the same delta\n", same_run);
			same_run = 0;
			if (shown < 24)
				printf("    [%zu..%zu) row %zu+%zu <- %s (delta %+lld bytes), %zu bytes\n", p, p + len, p / w, p % w,
				       where(s).c_str(), delta, len);
			else if (shown == 24)
				printf("    ... (further segments not shown)\n");
			shown++;
			last_delta = delta;
		}
		matched += len;
		p += len;
	}
	if (same_run && shown <= 24)
		printf("    ... %zu more segments with the same delta\n", same_run);
	printf("  %s: %zu of %zu bytes traced to D3D11 data in %zu segments (longest %zu bytes), %zu not found\n", what,
	       matched, n, segments, max_len, n - matched);
	return first_src;
}

} // namespace

int main(int argc, char **argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	options o = parse(argc, argv);
	const uint32_t W = o.w, H = o.h;

	// ------------------------------------------------------------ Vulkan instance
	HMODULE vk = LoadLibraryA("vulkan-1.dll");
	if (!vk)
		die("vulkan-1.dll not found");
	vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
#define LOAD_G(f) f = (PFN_##f)vkGetInstanceProcAddr(nullptr, #f);
	VK_GLOBAL_FNS(LOAD_G)
	uint32_t loader_ver = VK_API_VERSION_1_0;
	if (vkEnumerateInstanceVersion)
		vkEnumerateInstanceVersion(&loader_ver);

	VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	app.pApplicationName = "d3d11-nv12-vulkan-import";
	app.apiVersion = VK_API_VERSION_1_3;
	VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	ici.pApplicationInfo = &app;
	VkInstance inst;
	VKC(vkCreateInstance(&ici, nullptr, &inst));
#define LOAD_I(f)                                               \
	f = (PFN_##f)vkGetInstanceProcAddr(inst, #f);           \
	if (!f)                                                 \
		die("missing instance function %s", #f);
	VK_INSTANCE_FNS(LOAD_I)

	uint32_t npd = 0;
	VKC(vkEnumeratePhysicalDevices(inst, &npd, nullptr));
	std::vector<VkPhysicalDevice> pds(npd);
	VKC(vkEnumeratePhysicalDevices(inst, &npd, pds.data()));

	VkPhysicalDevice pd = VK_NULL_HANDLE;
	VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
	VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
	VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	for (uint32_t i = 0; i < npd; i++) {
		VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
		VkPhysicalDeviceDriverProperties dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
		id.pNext = &dp;
		VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		p.pNext = &id;
		vkGetPhysicalDeviceProperties2(pds[i], &p);
		if (o.list)
			printf("GPU %u: %s (%s %s)\n", i, p.properties.deviceName, dp.driverName, dp.driverInfo);
		bool match = (o.gpu == std::to_string(i)) ||
			     (!isdigit((unsigned char)o.gpu[0]) && strstr(p.properties.deviceName, o.gpu.c_str()));
		if (match && pd == VK_NULL_HANDLE) {
			pd = pds[i];
			idp = id;
			drv = dp;
			props = p;
			idp.pNext = nullptr;
			drv.pNext = nullptr;
		}
	}
	if (o.list)
		return 0;
	if (pd == VK_NULL_HANDLE)
		die("no GPU matches --gpu %s (use --list)", o.gpu.c_str());
	if (!idp.deviceLUIDValid)
		die("deviceLUIDValid = VK_FALSE");

	// ------------------------------------------------------------ D3D11 device on the same adapter
	IDXGIFactory4 *fac = nullptr;
	HRC(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)));
	LUID luid;
	memcpy(&luid, idp.deviceLUID, sizeof luid);
	IDXGIAdapter1 *adapter = nullptr;
	HRC(fac->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter)));
	DXGI_ADAPTER_DESC1 ad;
	adapter->GetDesc1(&ad);
	LARGE_INTEGER umd{};
	adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd);
	ID3D11Device *dev11 = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
	HRC(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, fls, 2, D3D11_SDK_VERSION, &dev11, nullptr,
			      &ctx));

	char adname[128];
	WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, adname, sizeof adname, nullptr, nullptr);
	printf("Windows        %s\n", windows_build().c_str());
	printf("Vulkan loader  %s\n", vk_version(loader_ver).c_str());
	printf("GPU            %s (vendor 0x%04x device 0x%04x)\n", props.properties.deviceName, props.properties.vendorID,
	       props.properties.deviceID);
	printf("Vulkan driver  %s %s, conformance %u.%u.%u.%u, API %s, driverVersion 0x%08x\n", drv.driverName, drv.driverInfo,
	       drv.conformanceVersion.major, drv.conformanceVersion.minor, drv.conformanceVersion.subminor,
	       drv.conformanceVersion.patch, vk_version(props.properties.apiVersion).c_str(), props.properties.driverVersion);
	printf("DXGI adapter   %s, UMD %u.%u.%u.%u (LUID matched)\n", adname, HIWORD(umd.HighPart), LOWORD(umd.HighPart),
	       HIWORD(umd.LowPart), LOWORD(umd.LowPart));
	printf("Case           %ux%u NV12, %s handle (%s), D3D11 bind SHADER_RESOURCE%s\n", W, H, o.nt ? "NT" : "KMT",
	       o.nt ? "MISC_SHARED|MISC_SHARED_NTHANDLE -> D3D11_TEXTURE_BIT" : "MISC_SHARED -> D3D11_TEXTURE_KMT_BIT",
	       o.rt ? "|RENDER_TARGET" : "");

	// ------------------------------------------------------------ D3D11 NV12 texture with the pattern
	std::vector<uint8_t> ref;
	fill_nv12(ref, W, H);
	D3D11_TEXTURE2D_DESC td{};
	td.Width = W;
	td.Height = H;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_NV12;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (o.rt ? D3D11_BIND_RENDER_TARGET : 0);
	td.MiscFlags = o.nt ? (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE) : D3D11_RESOURCE_MISC_SHARED;
	D3D11_SUBRESOURCE_DATA sd{ref.data(), W, 0};
	ID3D11Texture2D *tex = nullptr;
	HRC(dev11->CreateTexture2D(&td, &sd, &tex));

	// Control: D3D11's own view of the texture (staging copy).
	{
		D3D11_TEXTURE2D_DESC st = td;
		st.Usage = D3D11_USAGE_STAGING;
		st.BindFlags = 0;
		st.MiscFlags = 0;
		st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Texture2D *stg = nullptr;
		HRC(dev11->CreateTexture2D(&st, nullptr, &stg));
		ctx->CopyResource(stg, tex);
		D3D11_MAPPED_SUBRESOURCE m;
		HRC(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m));
		size_t bad = 0;
		for (uint32_t r = 0; r < H * 3 / 2; r++)
			bad += memcmp((const uint8_t *)m.pData + (size_t)r * m.RowPitch, ref.data() + (size_t)r * W, W) != 0;
		ctx->Unmap(stg, 0);
		stg->Release();
		printf("D3D11 staging readback: %s (%zu bad rows, RowPitch %u)\n", bad ? "MISMATCH" : "matches pattern", bad,
		       m.RowPitch);
		if (bad)
			die("D3D11 does not see its own data; nothing to test");
	}
	{
		// Make sure the initial data upload is complete before Vulkan reads the memory.
		D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
		ID3D11Query *q = nullptr;
		HRC(dev11->CreateQuery(&qd, &q));
		ctx->End(q);
		ctx->Flush();
		BOOL done = FALSE;
		while (ctx->GetData(q, &done, sizeof done, 0) != S_OK || !done)
			Sleep(0);
		q->Release();
	}

	HANDLE handle = nullptr;
	if (o.nt) {
		IDXGIResource1 *r = nullptr;
		HRC(tex->QueryInterface(IID_PPV_ARGS(&r)));
		HRC(r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle));
		r->Release();
	} else {
		IDXGIResource *r = nullptr;
		HRC(tex->QueryInterface(IID_PPV_ARGS(&r)));
		HRC(r->GetSharedHandle(&handle));
		r->Release();
	}
	const VkExternalMemoryHandleTypeFlagBits ht =
		o.nt ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT : VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

	// ------------------------------------------------------------ Vulkan device
	uint32_t nqf = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, nullptr);
	std::vector<VkQueueFamilyProperties> qfs(nqf);
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qfs.data());
	uint32_t qf = UINT32_MAX;
	for (uint32_t i = 0; i < nqf && qf == UINT32_MAX; i++)
		if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			qf = i;
	if (qf == UINT32_MAX)
		die("no graphics queue");
	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	qci.queueFamilyIndex = qf;
	qci.queueCount = 1;
	qci.pQueuePriorities = &prio;
	VkPhysicalDeviceSamplerYcbcrConversionFeatures ycf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
	VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
	f2.pNext = &ycf;
	vkGetPhysicalDeviceFeatures2(pd, &f2);
	VkPhysicalDeviceSamplerYcbcrConversionFeatures ycf_on{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES};
	ycf_on.samplerYcbcrConversion = ycf.samplerYcbcrConversion;
	const char *dexts[] = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME};
	VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	dci.pNext = &ycf_on;
	dci.queueCreateInfoCount = 1;
	dci.pQueueCreateInfos = &qci;
	dci.enabledExtensionCount = 1;
	dci.ppEnabledExtensionNames = dexts;
	VkDevice dev;
	VKC(vkCreateDevice(pd, &dci, nullptr, &dev));
#define LOAD_D(f)                                     \
	f = (PFN_##f)vkGetDeviceProcAddr(dev, #f);    \
	if (!f)                                       \
		die("missing device function %s", #f);
	VK_DEVICE_FNS(LOAD_D)
	VkQueue queue;
	vkGetDeviceQueue(dev, qf, 0, &queue);

	const VkFormat fmt = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
	{
		VkPhysicalDeviceExternalImageFormatInfo ei{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
		ei.handleType = ht;
		VkPhysicalDeviceImageFormatInfo2 fi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
		fi.pNext = &ei;
		fi.format = fmt;
		fi.type = VK_IMAGE_TYPE_2D;
		fi.tiling = VK_IMAGE_TILING_OPTIMAL;
		fi.usage = usage;
		VkExternalImageFormatProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
		VkImageFormatProperties2 ip{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
		ip.pNext = &ep;
		VkResult r = vkGetPhysicalDeviceImageFormatProperties2(pd, &fi, &ip);
		auto f = ep.externalMemoryProperties.externalMemoryFeatures;
		printf("vkGetPhysicalDeviceImageFormatProperties2: %s, externalMemoryFeatures 0x%x%s%s%s, compatibleHandleTypes 0x%x\n",
		       vkres(r), f, f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT ? " DEDICATED_ONLY" : "",
		       f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT ? " EXPORTABLE" : "",
		       f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT ? " IMPORTABLE" : "",
		       ep.externalMemoryProperties.compatibleHandleTypes);
		if (r != VK_SUCCESS || !(f & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
			die("import of this format/handle type is not supported");
	}

	VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
	VKC(vkGetMemoryWin32HandlePropertiesKHR(dev, ht, handle, &hp));

	VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	emi.handleTypes = ht;
	VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ii.pNext = &emi;
	ii.imageType = VK_IMAGE_TYPE_2D;
	ii.format = fmt;
	ii.extent = {W, H, 1};
	ii.mipLevels = 1;
	ii.arrayLayers = 1;
	ii.samples = VK_SAMPLE_COUNT_1_BIT;
	ii.tiling = VK_IMAGE_TILING_OPTIMAL;
	ii.usage = usage;
	ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage img;
	VKC(vkCreateImage(dev, &ii, nullptr, &img));

	VkMemoryDedicatedRequirements dreq{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
	VkMemoryRequirements2 mr{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
	mr.pNext = &dreq;
	VkImageMemoryRequirementsInfo2 mri{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
	mri.image = img;
	vkGetImageMemoryRequirements2(dev, &mri, &mr);
	uint32_t bits = mr.memoryRequirements.memoryTypeBits & hp.memoryTypeBits;
	printf("image memory requirements: size %llu, alignment %llu, memoryTypeBits 0x%x; handle memoryTypeBits 0x%x; "
	       "prefersDedicated %u requiresDedicated %u\n",
	       (unsigned long long)mr.memoryRequirements.size, (unsigned long long)mr.memoryRequirements.alignment,
	       mr.memoryRequirements.memoryTypeBits, hp.memoryTypeBits, dreq.prefersDedicatedAllocation,
	       dreq.requiresDedicatedAllocation);
	if (!bits)
		die("no memory type in common between image and handle");
	uint32_t mt = 0;
	while (!(bits & (1u << mt)))
		mt++;

	VkMemoryDedicatedAllocateInfo dai{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	dai.image = img;
	VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
	imp.pNext = &dai;
	imp.handleType = ht;
	imp.handle = handle;
	VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	mai.pNext = &imp;
	mai.allocationSize = mr.memoryRequirements.size;
	mai.memoryTypeIndex = mt;
	VkDeviceMemory mem;
	VKC(vkAllocateMemory(dev, &mai, nullptr, &mem));
	VKC(vkBindImageMemory(dev, img, mem, 0));

	// Host-visible readback buffer: plane 0 at 0 (W*H bytes), plane 1 at W*H (W*H/2 bytes), both tightly packed.
	const VkDeviceSize ysz = (VkDeviceSize)W * H, total = ysz * 3 / 2;
	VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bci.size = total;
	bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VkBuffer buf;
	VKC(vkCreateBuffer(dev, &bci, nullptr, &buf));
	VkMemoryRequirements bmr;
	vkGetBufferMemoryRequirements(dev, buf, &bmr);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	uint32_t bmt = UINT32_MAX;
	const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	for (uint32_t i = 0; i < mp.memoryTypeCount && bmt == UINT32_MAX; i++)
		if ((bmr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
			bmt = i;
	if (bmt == UINT32_MAX)
		die("no host-visible memory type");
	VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	bai.allocationSize = bmr.size;
	bai.memoryTypeIndex = bmt;
	VkDeviceMemory bmem;
	VKC(vkAllocateMemory(dev, &bai, nullptr, &bmem));
	VKC(vkBindBufferMemory(dev, buf, bmem, 0));
	uint8_t *got = nullptr;
	VKC(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&got));
	memset(got, 0xCD, (size_t)total);

	// ------------------------------------------------------------ copy both planes out
	VkCommandPoolCreateInfo cpci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	cpci.queueFamilyIndex = qf;
	VkCommandPool pool;
	VKC(vkCreateCommandPool(dev, &cpci, nullptr, &pool));
	VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	cbai.commandPool = pool;
	cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	VkCommandBuffer cb;
	VKC(vkAllocateCommandBuffers(dev, &cbai, &cb));
	VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	VKC(vkBeginCommandBuffer(cb, &cbbi));

	VkImageMemoryBarrier acq{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	acq.srcAccessMask = 0;
	acq.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	acq.oldLayout = VK_IMAGE_LAYOUT_GENERAL; // keep the D3D11 contents
	acq.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	acq.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	acq.dstQueueFamilyIndex = qf;
	acq.image = img;
	acq.subresourceRange = {VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
			     1, &acq);

	VkBufferImageCopy cp[2]{};
	cp[0].bufferOffset = 0;
	cp[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
	cp[0].imageExtent = {W, H, 1};
	cp[1].bufferOffset = ysz;
	cp[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
	cp[1].imageExtent = {W / 2, H / 2, 1};
	vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 2, cp);

	VkImageMemoryBarrier rel = acq;
	rel.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	rel.dstAccessMask = 0;
	rel.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	rel.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	rel.srcQueueFamilyIndex = qf;
	rel.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	VkBufferMemoryBarrier hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
	hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	hb.buffer = buf;
	hb.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT | VK_PIPELINE_STAGE_HOST_BIT,
			     0, 0, nullptr, 1, &hb, 1, &rel);
	VKC(vkEndCommandBuffer(cb));

	VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence;
	VKC(vkCreateFence(dev, &fci, nullptr, &fence));
	VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	si.commandBufferCount = 1;
	si.pCommandBuffers = &cb;
	VKC(vkQueueSubmit(queue, 1, &si, fence));
	VKC(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000));

	// ------------------------------------------------------------ compare
	std::unordered_map<std::string, uint32_t> index; // row content -> D3D11 row (0..H*3/2-1)
	for (uint32_t r = 0; r < H * 3 / 2; r++)
		index.emplace(std::string((const char *)ref.data() + (size_t)r * W, W), r);

	size_t ybad = 0, uvbad = 0;
	int shown = 0;
	printf("\nComparing Vulkan readback against the D3D11 data:\n");
	for (uint32_t y = 0; y < H; y++)
		for (uint32_t x = 0; x < W; x++) {
			uint8_t g = got[(size_t)y * W + x], e = ref[(size_t)y * W + x];
			if (g != e && ybad++ < 4)
				printf("  mismatch PLANE_0 (x %u, y %u): got 0x%02x expected 0x%02x\n", x, y, g, e);
		}
	for (uint32_t y = 0; y < H / 2; y++)
		for (uint32_t x = 0; x < W / 2; x++)
			for (int c = 0; c < 2; c++) {
				size_t i = (size_t)y * W + x * 2 + c;
				uint8_t g = got[ysz + i], e = ref[ysz + i];
				if (g != e && shown++ < 8)
					printf("  mismatch PLANE_1 (x %u, y %u) %s: got 0x%02x expected 0x%02x\n", x, y,
					       c ? "Cr" : "Cb", g, e);
				uvbad += g != e;
			}
	printf("PLANE_0 (Y):    %zu of %llu bytes differ\n", ybad, (unsigned long long)ysz);
	printf("PLANE_1 (CbCr): %zu of %llu bytes differ\n", uvbad, (unsigned long long)(ysz / 2));

	printf("\nWhere the received rows come from:\n");
	int yoff = 0, uvoff = 0;
	trace_rows("PLANE_0", got, H, W, H, 0, index, ref, yoff);
	trace_rows("PLANE_1", got + ysz, H / 2, W, H, H, index, ref, uvoff);
	long long uv_first = -1;
	if (ybad && yoff == INT32_MIN)
		trace_bytes("PLANE_0", got, (size_t)ysz, W, H, ref);
	if (uvbad && uvoff == INT32_MIN)
		uv_first = trace_bytes("PLANE_1", got + ysz, (size_t)ysz / 2, W, H, ref);
	if (uvoff == INT32_MIN && uv_first >= (long long)ysz)
		printf("Detected chroma row offset: not a uniform shift. Vulkan PLANE_1 byte 0 = D3D11 chroma row %lld + %lld bytes "
		       "(%+lld rows), the rest of the plane is pieced together from other rows (see byte trace)\n",
		       (uv_first - (long long)ysz) / W, (uv_first - (long long)ysz) % W, (uv_first - (long long)ysz) / W);
	else if (uvoff == INT32_MIN && uv_first >= 0)
		printf("Detected chroma row offset: Vulkan PLANE_1 byte 0 = D3D11 luma row %lld + %lld bytes\n", uv_first / W,
		       uv_first % W);
	else if (uvoff == INT32_MIN)
		printf("Detected chroma row offset: none (PLANE_1 byte 0 is not found anywhere in the D3D11 data)\n");
	else
		printf("Detected chroma row offset: %+d (Vulkan PLANE_1 row 0 = D3D11 chroma row %d; %+lld bytes at pitch %u)\n",
		       uvoff, uvoff, (long long)uvoff * W, W);

	bool ok = ybad == 0 && uvbad == 0;
	printf("\nRESULT: %s\n", ok ? "PASS (Vulkan sees exactly what D3D11 wrote)" : "FAIL (Vulkan readback differs from D3D11 data)");

	vkDeviceWaitIdle(dev);
	vkDestroyFence(dev, fence, nullptr);
	vkDestroyCommandPool(dev, pool, nullptr);
	vkDestroyBuffer(dev, buf, nullptr);
	vkFreeMemory(dev, bmem, nullptr);
	vkDestroyImage(dev, img, nullptr);
	vkFreeMemory(dev, mem, nullptr);
	vkDestroyDevice(dev, nullptr);
	vkDestroyInstance(inst, nullptr);
	if (o.nt)
		CloseHandle(handle);
	tex->Release();
	ctx->Release();
	dev11->Release();
	adapter->Release();
	fac->Release();
	return ok ? 0 : 1;
}
