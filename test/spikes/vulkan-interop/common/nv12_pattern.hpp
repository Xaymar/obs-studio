// NV12 test pattern + ffmpeg round-trip check, shared by the encoder spikes. Throwaway test code.
#pragma once
#include "spike_common.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

namespace spike {

inline uint8_t pat_y(uint32_t x, uint32_t y, uint32_t f)
{
	return (uint8_t)(16 + ((x / 4 + y / 4 + f * 8) % 200));
}
inline uint8_t pat_uv(uint32_t x, uint32_t y, uint32_t f, int c)
{
	return (uint8_t)(c ? 64 + ((y * 2) % 128) : 64 + ((x + f * 4) % 128));
}

// Tightly packed NV12 (pitch == w), chroma at w*h.
inline void fill_nv12(uint8_t *dst, uint32_t w, uint32_t h, uint32_t f)
{
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
			dst[y * w + x] = pat_y(x, y, f);
	uint8_t *uv = dst + (size_t)w * h;
	for (uint32_t y = 0; y < h / 2; y++)
		for (uint32_t x = 0; x < w / 2; x++) {
			uv[y * w + x * 2 + 0] = pat_uv(x, y, f, 0);
			uv[y * w + x * 2 + 1] = pat_uv(x, y, f, 1);
		}
}

// Decodes 'stream' with $SPIKE_FFMPEG and compares every frame against the pattern.
inline std::string verify_with_ffmpeg(const std::string &stream, uint32_t w, uint32_t h, uint32_t frames, bool &ok)
{
	const char *ff = getenv("SPIKE_FFMPEG");
	if (!ff || !*ff) {
		ok = true;
		return "not decoded (set SPIKE_FFMPEG to verify)";
	}
	std::string yuv = stream + ".decoded.yuv";
	std::string cmd = std::string("\"\"") + ff + "\" -v error -y -i \"" + stream + "\" -f rawvideo -pix_fmt nv12 \"" +
			  yuv + "\"\"";
	int rc = system(cmd.c_str());
	std::ifstream in(yuv, std::ios::binary);
	std::vector<uint8_t> dec((std::istreambuf_iterator<char>(in)), {});
	size_t fs = (size_t)w * h * 3 / 2;
	if (rc != 0 || dec.size() < fs) {
		ok = false;
		return "ffmpeg decode failed (rc " + std::to_string(rc) + ", " + std::to_string(dec.size()) + " bytes)";
	}
	size_t nf = dec.size() / fs;
	std::vector<uint8_t> exp(fs);
	double worst_y = 1e9, worst_uv = 1e9;
	auto psnr = [](double se, size_t n) {
		double mse = se / n;
		return mse <= 1e-10 ? 99.0 : 10.0 * log10(255.0 * 255.0 / mse);
	};
	for (size_t f = 0; f < nf; f++) {
		fill_nv12(exp.data(), w, h, (uint32_t)f);
		double se_y = 0, se_uv = 0;
		for (size_t i = 0; i < (size_t)w * h; i++) {
			double d = (double)dec[f * fs + i] - exp[i];
			se_y += d * d;
		}
		for (size_t i = (size_t)w * h; i < fs; i++) {
			double d = (double)dec[f * fs + i] - exp[i];
			se_uv += d * d;
		}
		worst_y = std::min(worst_y, psnr(se_y, (size_t)w * h));
		worst_uv = std::min(worst_uv, psnr(se_uv, fs - (size_t)w * h));
	}
	ok = nf == frames && worst_y > 35 && worst_uv > 35;
	char b[160];
	snprintf(b, sizeof b, "decoded %zu frames, worst PSNR Y %.1f dB / UV %.1f dB vs pattern", nf, worst_y, worst_uv);
	return b;
}

} // namespace spike
