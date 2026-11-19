#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace knurbs::device_output {

// Copies `rows` rows of `row_bytes` bytes between two images whose rows are
// `dst_linesize` and `src_linesize` bytes apart. Only the bytes of the
// picture are copied, never the padding, and never more than either side
// holds per row, so differing line sizes cannot read or write past a row.
inline void copy_rows(uint8_t *dst, uint32_t dst_linesize, const uint8_t *src, uint32_t src_linesize,
		      uint32_t row_bytes, uint32_t rows)
{
	const uint32_t bytes = std::min({row_bytes, dst_linesize, src_linesize});
	for (uint32_t y = 0; y < rows; y++) {
		std::memcpy(dst + (size_t)dst_linesize * y, src + (size_t)src_linesize * y, bytes);
	}
}

} // namespace knurbs::device_output
