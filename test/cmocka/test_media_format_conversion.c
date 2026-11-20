#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <string.h>

#include <media-io/format-conversion.h>

/* 8x2 pixels: the SSE paths work on 4 pixels and 2 rows at a time, on 16-byte aligned rows */
#define W 8
#define H 2

/* packed "UYVX": byte 0 = U, 1 = Y, 2 = V, 3 = X */
static uint8_t *make_uyvx(void)
{
	uint8_t *px = bmalloc(W * H * 4);
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint8_t *p = px + (y * W + x) * 4;
			p[0] = (uint8_t)(10 + x * 4 + y);  /* U */
			p[1] = (uint8_t)(100 + x + y * W); /* Y */
			p[2] = (uint8_t)(200 - x * 2 - y); /* V */
			p[3] = 0xff;
		}
	}
	return px;
}

static uint8_t avg4(const uint8_t *px, int x, int c)
{
	int sum = px[x * 4 + c] + px[(x + 1) * 4 + c] + px[(W + x) * 4 + c] + px[(W + x + 1) * 4 + c];
	return (uint8_t)(sum >> 2);
}

static void compress_test(void **state)
{
	UNUSED_PARAMETER(state);

	uint8_t *in = make_uyvx();
	uint8_t *planes = bzalloc(W * H * 3);
	uint8_t *out[3] = {planes, planes + W * H, planes + W * H * 2};

	/* I420 */
	uint32_t ls420[3] = {W, W / 2, W / 2};
	compress_uyvx_to_i420(in, W * 4, 0, H, out, ls420);
	for (int i = 0; i < W * H; i++)
		assert_int_equal(out[0][i], in[i * 4 + 1]);
	for (int x = 0; x < W / 2; x++) {
		assert_int_equal(out[1][x], avg4(in, x * 2, 0));
		assert_int_equal(out[2][x], avg4(in, x * 2, 2));
	}

	/* NV12: interleaved U, V */
	memset(planes, 0, W * H * 3);
	uint32_t lsnv12[2] = {W, W};
	compress_uyvx_to_nv12(in, W * 4, 0, H, out, lsnv12);
	for (int i = 0; i < W * H; i++)
		assert_int_equal(out[0][i], in[i * 4 + 1]);
	for (int x = 0; x < W / 2; x++) {
		assert_int_equal(out[1][x * 2], avg4(in, x * 2, 0));
		assert_int_equal(out[1][x * 2 + 1], avg4(in, x * 2, 2));
	}

	/* I444 */
	memset(planes, 0, W * H * 3);
	uint32_t ls444[3] = {W, W, W};
	convert_uyvx_to_i444(in, W * 4, 0, H, out, ls444);
	for (int i = 0; i < W * H; i++) {
		assert_int_equal(out[0][i], in[i * 4 + 1]);
		assert_int_equal(out[1][i], in[i * 4 + 0]);
		assert_int_equal(out[2][i], in[i * 4 + 2]);
	}

	bfree(planes);
	bfree(in);
}

static void decompress_planar_test(void **state)
{
	UNUSED_PARAMETER(state);

	uint8_t y[W * H], u[W / 2], v[W / 2], uv[W];
	for (int i = 0; i < W * H; i++)
		y[i] = (uint8_t)(i + 1);
	for (int i = 0; i < W / 2; i++) {
		u[i] = (uint8_t)(0x40 + i);
		v[i] = (uint8_t)(0x80 + i);
		uv[i * 2] = u[i];
		uv[i * 2 + 1] = v[i];
	}

	uint32_t out[W * H];

	/* 4:2:0 planar: packed as Y << 16 | U << 8 | V */
	const uint8_t *in420[3] = {y, u, v};
	const uint32_t ls420[3] = {W, W / 2, W / 2};
	memset(out, 0, sizeof(out));
	decompress_420(in420, ls420, 0, H, (uint8_t *)out, W * 4);
	for (int r = 0; r < H; r++)
		for (int x = 0; x < W; x++)
			assert_int_equal(out[r * W + x], (uint32_t)y[r * W + x] << 16 | u[x / 2] << 8 | v[x / 2]);

	/* NV12: packed as V << 16 | U << 8 | Y, the reverse order of decompress_420() (see known-bugs.md) */
	const uint8_t *innv12[2] = {y, uv};
	const uint32_t lsnv12[2] = {W, W};
	memset(out, 0, sizeof(out));
	decompress_nv12(innv12, lsnv12, 0, H, (uint8_t *)out, W * 4);
	for (int r = 0; r < H; r++)
		for (int x = 0; x < W; x++)
			assert_int_equal(out[r * W + x], (uint32_t)v[x / 2] << 16 | u[x / 2] << 8 | y[r * W + x]);
}

/* Regression test: decompress_422() walked linesize / 2 dwords per row, twice the row, reading past the input row and
 * writing past the output row. */
static void decompress_422_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* one row of 4 pixels, then guard bytes */
	uint32_t in[4] = {0, 0, 0xdeadbeef, 0xdeadbeef};
	uint8_t *b = (uint8_t *)in;
	const uint8_t yuy2[8] = {1, 2, 3, 4, 5, 6, 7, 8}; /* Y0 U Y1 V Y2 U Y3 V */
	memcpy(b, yuy2, 8);

	uint32_t out[8];
	for (int i = 0; i < 8; i++)
		out[i] = 0xcccccccc;
	decompress_422(b, 8, 0, 1, (uint8_t *)out, 16, true);
	/* each source dword yields two pixels: the dword itself, then with byte 0 replaced by byte 2 */
	assert_int_equal(out[0], 0x04030201);
	assert_int_equal(out[1], 0x04030203);
	assert_int_equal(out[2], 0x08070605);
	assert_int_equal(out[3], 0x08070607);
	for (int i = 4; i < 8; i++)
		assert_int_equal(out[i], 0xcccccccc);

	const uint8_t uyvy[8] = {1, 2, 3, 4, 5, 6, 7, 8}; /* U Y0 V Y1 ... */
	memcpy(b, uyvy, 8);
	for (int i = 0; i < 8; i++)
		out[i] = 0xcccccccc;
	decompress_422(b, 8, 0, 1, (uint8_t *)out, 16, false);
	/* the second pixel takes byte 3 into byte 1 */
	assert_int_equal(out[0], 0x04030201);
	assert_int_equal(out[1], 0x04030401);
	assert_int_equal(out[2], 0x08070605);
	assert_int_equal(out[3], 0x08070805);
	for (int i = 4; i < 8; i++)
		assert_int_equal(out[i], 0xcccccccc);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(compress_test),
		obs_unit_test(decompress_planar_test),
		obs_unit_test(decompress_422_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
