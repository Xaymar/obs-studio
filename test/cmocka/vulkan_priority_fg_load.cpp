// Synthetic GPU-bound foreground load for the queue priority measurement
// (docs/superpowers/specs/2026-11-17-libobs-vulkan-priority-measurement.md). Not OBS, not a test, never installed.
//
//   vulkan_priority_fg_load <seconds> <warm-up seconds> <shader iterations> <csv out>
//
// Draws one fullscreen triangle with a long pixel shader loop into a 1920x1080 window through D3D11 (the typical game
// API), presents without vsync, then waits for the GPU (event query) so each frame time is the full CPU+GPU time
// of that frame. Frame time = QueryPerformanceCounter delta between consecutive GPU completions. Frames inside the
// warm-up are discarded. Writes one frame time (ms) per CSV row and prints p50/p95/p99/max.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

static const char *shader = R"(
cbuffer c : register(b0) { uint iters; float t; float2 pad; };
float4 vs(uint id : SV_VertexID) : SV_Position
{
	float2 p = float2((id << 1) & 2, id & 2);
	return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 ps(float4 pos : SV_Position) : SV_Target
{
	float2 uv = pos.xy * 0.001;
	float3 c = 0;
	for (uint i = 0; i < iters; ++i) {
		c += sin(uv.xyx * (i + 1) + t) * cos(uv.yxy * 0.7 + c.zxy);
		uv = uv.yx * 1.0001 + c.xy * 0.001;
	}
	return float4(frac(c), 1);
}
)";

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_CLOSE) {
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

static ID3DBlob *compile(const char *entry, const char *target)
{
	ID3DBlob *code = nullptr, *err = nullptr;
	if (FAILED(D3DCompile(shader, strlen(shader), "fg_load", nullptr, nullptr, entry, target,
			      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) {
		fprintf(stderr, "shader: %s\n", err ? (const char *)err->GetBufferPointer() : "?");
		exit(1);
	}
	return code;
}

static double pct(const std::vector<double> &sorted, double q)
{
	// Nearest rank.
	size_t i = (size_t)std::ceil(q * sorted.size());
	return sorted[std::min(sorted.size() - 1, i ? i - 1 : 0)];
}

int main(int argc, char **argv)
{
	if (argc != 5) {
		fprintf(stderr, "usage: %s <seconds> <warm-up seconds> <shader iterations> <csv out>\n", argv[0]);
		return 2;
	}
	const double seconds = atof(argv[1]), warmup = atof(argv[2]);
	const UINT iters = (UINT)atoi(argv[3]);
	const UINT W = 1920, H = 1080;

	WNDCLASSW wc = {};
	wc.lpfnWndProc = wndproc;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"vulkan_priority_fg_load";
	wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	RegisterClassW(&wc);
	RECT r = {0, 0, (LONG)W, (LONG)H};
	AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = CreateWindowW(wc.lpszClassName, L"fg load", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
				  r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
	SetForegroundWindow(hwnd);

	DXGI_SWAP_CHAIN_DESC sd = {};
	sd.BufferCount = 2;
	sd.BufferDesc.Width = W;
	sd.BufferDesc.Height = H;
	sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.OutputWindow = hwnd;
	sd.SampleDesc.Count = 1;
	sd.Windowed = TRUE;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	IDXGISwapChain *swap = nullptr;
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
						 D3D11_SDK_VERSION, &sd, &swap, &dev, nullptr, &ctx))) {
		fprintf(stderr, "D3D11CreateDeviceAndSwapChain failed\n");
		return 1;
	}
	ID3D11Texture2D *back = nullptr;
	swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&back);
	ID3D11RenderTargetView *rtv = nullptr;
	dev->CreateRenderTargetView(back, nullptr, &rtv);

	ID3DBlob *vsb = compile("vs", "vs_5_0"), *psb = compile("ps", "ps_5_0");
	ID3D11VertexShader *vs = nullptr;
	ID3D11PixelShader *ps = nullptr;
	dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
	dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
	struct {
		UINT iters;
		float t, pad[2];
	} cb = {iters, 0.0f, {}};
	D3D11_BUFFER_DESC bd = {sizeof(cb), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
	ID3D11Buffer *cbuf = nullptr;
	dev->CreateBuffer(&bd, nullptr, &cbuf);
	D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT};
	ID3D11Query *done = nullptr;
	dev->CreateQuery(&qd, &done);

	D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->VSSetShader(vs, nullptr, 0);
	ctx->PSSetShader(ps, nullptr, 0);
	ctx->PSSetConstantBuffers(0, 1, &cbuf);
	ctx->RSSetViewports(1, &vp);

	LARGE_INTEGER freq, start, prev, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&start);
	prev = start;
	std::vector<double> ms;
	bool foreground = true; // the window stayed the foreground window for every measured frame
	for (MSG msg;;) {
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				return 1;
			}
			DispatchMessageW(&msg);
		}
		cb.t += 0.01f;
		ctx->UpdateSubresource(cbuf, 0, nullptr, &cb, 0, 0);
		ctx->OMSetRenderTargets(1, &rtv, nullptr);
		ctx->Draw(3, 0);
		swap->Present(0, 0);
		ctx->End(done);
		BOOL finished = FALSE;
		while (ctx->GetData(done, &finished, sizeof(finished), 0) != S_OK) {
		}
		QueryPerformanceCounter(&now);
		const double t = (double)(now.QuadPart - start.QuadPart) / freq.QuadPart;
		if (t > warmup) {
			ms.push_back(1000.0 * (now.QuadPart - prev.QuadPart) / freq.QuadPart);
			foreground &= GetForegroundWindow() == hwnd;
		}
		prev = now;
		if (t > warmup + seconds) {
			break;
		}
	}

	if (ms.empty()) {
		fprintf(stderr, "no frames measured\n");
		return 1;
	}
	FILE *f = fopen(argv[4], "w");
	if (!f) {
		fprintf(stderr, "cannot write %s\n", argv[4]);
		return 1;
	}
	fprintf(f, "frame,ms\n");
	for (size_t i = 0; i < ms.size(); ++i) {
		fprintf(f, "%zu,%.4f\n", i, ms[i]);
	}
	fclose(f);

	std::vector<double> s = ms;
	std::sort(s.begin(), s.end());
	double sum = 0;
	for (double v : s) {
		sum += v;
	}
	FILETIME c, e, k, u;
	GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
	const double cpu = ((((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) +
			    (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime)) /
			   1e7;
	printf("frames %zu mean %.3f p50 %.3f p95 %.3f p99 %.3f max %.3f ms; foreground %s; cpu %.1f s\n", s.size(),
	       sum / s.size(), pct(s, 0.50), pct(s, 0.95), pct(s, 0.99), s.back(), foreground ? "yes" : "no", cpu);
	return 0;
}
