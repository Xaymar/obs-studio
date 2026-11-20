#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "obs-test.h"

#include <string.h>

#include <obs.h>
#include <obs-av1.h>
#include <obs-avc.h>
#include <obs-hevc.h>
#include <obs-nal.h>

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* High profile SPS (chroma_format_idc 1, 8 bit), PPS, SEI and an IDR slice */
static const uint8_t avc_sps[] = {0, 0, 0, 1, 0x67, 0x64, 0x00, 0x1f, 0xac};
static const uint8_t avc_pps[] = {0, 0, 0, 1, 0x68, 0xee, 0x3c, 0x80};
static const uint8_t avc_sei[] = {0, 0, 1, 0x06, 0x05, 0x01, 0x00, 0x80};
static const uint8_t avc_idr[] = {0, 0, 1, 0x65, 0x88, 0x84, 0x10};
static const uint8_t avc_slice[] = {0, 0, 1, 0x41, 0x9a, 0x02};

static size_t cat(uint8_t *out, const uint8_t *const *parts, const size_t *sizes, size_t count)
{
	size_t pos = 0;
	for (size_t i = 0; i < count; i++) {
		memcpy(out + pos, parts[i], sizes[i]);
		pos += sizes[i];
	}
	return pos;
}

static void free_parsed(struct encoder_packet *pkt)
{
	/* the parsed data is preceded by a reference count */
	bfree(pkt->data - sizeof(long));
}

static void nal_test(void **state)
{
	UNUSED_PARAMETER(state);

	uint8_t buf[64] = {0};
	/* no start code: the end is returned */
	memset(buf, 0xff, sizeof(buf));
	assert_ptr_equal(obs_nal_find_startcode(buf, buf + sizeof(buf)), buf + sizeof(buf));
	assert_ptr_equal(obs_nal_find_startcode(buf, buf), buf);
	assert_ptr_equal(obs_nal_find_startcode(buf, buf + 2), buf + 2);

	/* every offset, for both the aligned scan and the byte-wise head and tail. As in FFmpeg, a start
	 * code in the last three bytes (an empty NAL) is not reported. */
	for (size_t off = 0; off + 4 <= sizeof(buf); off++) {
		memset(buf, 0xff, sizeof(buf));
		buf[off] = 0;
		buf[off + 1] = 0;
		buf[off + 2] = 1;
		assert_ptr_equal(obs_nal_find_startcode(buf, buf + sizeof(buf)), buf + off);
		/* a four byte start code is found at its first zero */
		if (off > 0) {
			buf[off - 1] = 0;
			assert_ptr_equal(obs_nal_find_startcode(buf, buf + sizeof(buf)), buf + off - 1);
		}
	}
	memset(buf, 0xff, sizeof(buf));
	buf[61] = 0;
	buf[62] = 0;
	buf[63] = 1;
	assert_ptr_equal(obs_nal_find_startcode(buf, buf + sizeof(buf)), buf + sizeof(buf));
	assert_ptr_equal(obs_avc_find_startcode(avc_idr, avc_idr + sizeof(avc_idr)), avc_idr);
}

static void avc_test(void **state)
{
	UNUSED_PARAMETER(state);

	uint8_t pkt[64];
	const uint8_t *parts[] = {avc_sps, avc_pps, avc_sei, avc_idr};
	const size_t sizes[] = {sizeof(avc_sps), sizeof(avc_pps), sizeof(avc_sei), sizeof(avc_idr)};
	size_t size = cat(pkt, parts, sizes, ARRAY_LEN(parts));

	assert_true(obs_avc_keyframe(pkt, size));
	assert_false(obs_avc_keyframe(avc_slice, sizeof(avc_slice)));
	assert_false(obs_avc_keyframe(avc_sei, sizeof(avc_sei)));
	assert_false(obs_avc_keyframe(pkt, 0));

	/* Annex B to length-prefixed */
	struct encoder_packet src = {.data = pkt, .size = size, .type = OBS_ENCODER_VIDEO};
	struct encoder_packet out;
	obs_parse_avc_packet(&out, &src);
	assert_true(out.keyframe);
	assert_int_equal(out.priority, OBS_NAL_PRIORITY_HIGHEST);
	assert_int_equal(out.drop_priority, OBS_NAL_PRIORITY_HIGHEST);
	assert_int_equal(out.size, 4 * 4 + 5 + 4 + 5 + 4);
	static const uint8_t first[] = {0, 0, 0, 5, 0x67, 0x64, 0x00, 0x1f, 0xac};
	assert_memory_equal(out.data, first, sizeof(first));
	free_parsed(&out);

	src.data = (uint8_t *)avc_slice;
	src.size = sizeof(avc_slice);
	src.priority = 0;
	assert_int_equal(obs_parse_avc_packet_priority(&src), OBS_NAL_PRIORITY_HIGH);
	obs_parse_avc_packet(&out, &src);
	assert_false(out.keyframe);
	assert_int_equal(out.priority, OBS_NAL_PRIORITY_HIGH);
	free_parsed(&out);

	/* avcC header with the high profile extension */
	uint8_t *header = NULL;
	size_t header_size = obs_parse_avc_header(&header, pkt, size);
	static const uint8_t avcc[] = {0x01, 0x64, 0x00, 0x1f, 0xff, 0xe1, 0x00, 0x05, 0x67, 0x64, 0x00, 0x1f,
				       0xac, 0x01, 0x00, 0x04, 0x68, 0xee, 0x3c, 0x80, 0xfd, 0xf8, 0xf8, 0x00};
	assert_int_equal(header_size, sizeof(avcc));
	assert_memory_equal(header, avcc, sizeof(avcc));

	/* a header without start codes is already avcC and is copied */
	uint8_t *copy = NULL;
	assert_int_equal(obs_parse_avc_header(&copy, header, header_size), header_size);
	assert_memory_equal(copy, header, header_size);
	bfree(copy);
	bfree(header);

	/* too short, or no SPS/PPS */
	assert_int_equal(obs_parse_avc_header(&header, pkt, 6), 0);
	assert_int_equal(obs_parse_avc_header(&header, avc_idr, sizeof(avc_idr)), 0);

	/* split headers, SEI and the rest */
	uint8_t *data, *hdr, *sei;
	size_t data_size, hdr_size, sei_size;
	obs_extract_avc_headers(pkt, size, &data, &data_size, &hdr, &hdr_size, &sei, &sei_size);
	assert_int_equal(hdr_size, sizeof(avc_sps) + sizeof(avc_pps));
	assert_memory_equal(hdr, pkt, hdr_size);
	assert_int_equal(sei_size, sizeof(avc_sei));
	assert_memory_equal(sei, avc_sei, sei_size);
	assert_int_equal(data_size, sizeof(avc_idr));
	assert_memory_equal(data, avc_idr, data_size);
	bfree(data);
	bfree(hdr);
	bfree(sei);
}

static const uint8_t hevc_vps[] = {0, 0, 0, 1, 0x40, 0x01, 0x0c};
static const uint8_t hevc_sps[] = {0, 0, 0, 1, 0x42, 0x01, 0x01};
static const uint8_t hevc_pps[] = {0, 0, 0, 1, 0x44, 0x01, 0xc1};
static const uint8_t hevc_sei[] = {0, 0, 1, 0x4e, 0x01, 0x05};
static const uint8_t hevc_idr[] = {0, 0, 1, 0x26, 0x01, 0xaf};
static const uint8_t hevc_trail_r[] = {0, 0, 1, 0x02, 0x01, 0xd0};
static const uint8_t hevc_trail_n[] = {0, 0, 1, 0x00, 0x01, 0xd0};
static const uint8_t hevc_aud[] = {0, 0, 1, 0x46, 0x01, 0x50};

static void hevc_test(void **state)
{
	UNUSED_PARAMETER(state);

	uint8_t pkt[64];
	const uint8_t *parts[] = {hevc_vps, hevc_sps, hevc_pps, hevc_sei, hevc_idr};
	const size_t sizes[] = {sizeof(hevc_vps), sizeof(hevc_sps), sizeof(hevc_pps), sizeof(hevc_sei),
				sizeof(hevc_idr)};
	size_t size = cat(pkt, parts, sizes, ARRAY_LEN(parts));

	assert_true(obs_hevc_keyframe(pkt, size));
	assert_false(obs_hevc_keyframe(hevc_trail_r, sizeof(hevc_trail_r)));
	assert_false(obs_hevc_keyframe(hevc_aud, sizeof(hevc_aud)));

	struct encoder_packet src = {.data = pkt, .size = size, .type = OBS_ENCODER_VIDEO};
	struct encoder_packet out;
	obs_parse_hevc_packet(&out, &src);
	assert_true(out.keyframe);
	assert_int_equal(out.priority, OBS_NAL_PRIORITY_HIGHEST);
	assert_int_equal(out.size, 5 * 4 + 3 + 3 + 3 + 3 + 3);
	free_parsed(&out);

	src.priority = 0;
	src.data = (uint8_t *)hevc_trail_r;
	src.size = sizeof(hevc_trail_r);
	assert_int_equal(obs_parse_hevc_packet_priority(&src), OBS_NAL_PRIORITY_HIGH);
	src.data = (uint8_t *)hevc_trail_n;
	src.size = sizeof(hevc_trail_n);
	assert_int_equal(obs_parse_hevc_packet_priority(&src), OBS_NAL_PRIORITY_DISPOSABLE);
	src.data = (uint8_t *)hevc_aud;
	src.size = sizeof(hevc_aud);
	assert_int_equal(obs_parse_hevc_packet_priority(&src), OBS_NAL_PRIORITY_DISPOSABLE);
	/* the priority only goes up */
	src.priority = OBS_NAL_PRIORITY_HIGH;
	assert_int_equal(obs_parse_hevc_packet_priority(&src), OBS_NAL_PRIORITY_HIGH);

	uint8_t *data, *hdr, *sei;
	size_t data_size, hdr_size, sei_size;
	obs_extract_hevc_headers(pkt, size, &data, &data_size, &hdr, &hdr_size, &sei, &sei_size);
	assert_int_equal(hdr_size, sizeof(hevc_vps) + sizeof(hevc_sps) + sizeof(hevc_pps));
	assert_memory_equal(hdr, pkt, hdr_size);
	assert_int_equal(sei_size, sizeof(hevc_sei));
	assert_int_equal(data_size, sizeof(hevc_idr));
	assert_memory_equal(data, hevc_idr, data_size);
	bfree(data);
	bfree(hdr);
	bfree(sei);
}

static void av1_test(void **state)
{
	UNUSED_PARAMETER(state);

	/* temporal delimiter, sequence header, metadata and a key frame, all with size fields */
	static const uint8_t key[] = {0x12, 0x00, 0x0a, 0x03, 0xaa, 0xbb, 0xcc, 0x2a,
				      0x02, 0x04, 0x80, 0x32, 0x02, 0x10, 0x00};
	static const uint8_t inter[] = {0x12, 0x00, 0x32, 0x02, 0x30, 0x00};
	/* show_existing_frame */
	static const uint8_t existing[] = {0x1a, 0x01, 0x80};
	assert_true(obs_av1_keyframe(key, sizeof(key)));
	assert_false(obs_av1_keyframe(inter, sizeof(inter)));
	assert_false(obs_av1_keyframe(existing, sizeof(existing)));
	assert_false(obs_av1_keyframe(key, 2));

	uint8_t *data, *hdr;
	size_t data_size, hdr_size;
	obs_extract_av1_headers(key, sizeof(key), &data, &data_size, &hdr, &hdr_size);
	assert_int_equal(data_size, sizeof(key));
	assert_memory_equal(data, key, sizeof(key));
	assert_int_equal(hdr_size, 5 + 4);
	assert_memory_equal(hdr, key + 2, hdr_size);
	bfree(data);
	bfree(hdr);

	/* metadata OBUs */
	static const uint8_t payload[] = {1, 2, 3};
	static const uint8_t obu[] = {0x2a, 0x05, METADATA_TYPE_ITUT_T35, 1, 2, 3, 0x80};
	uint8_t *out;
	size_t out_size;
	metadata_obu_itu_t35(payload, sizeof(payload), &out, &out_size);
	assert_int_equal(out_size, sizeof(obu));
	assert_memory_equal(out, obu, sizeof(obu));
	bfree(out);
	/* a payload of 200 bytes needs a two byte size */
	uint8_t big[200] = {0};
	metadata_obu(big, sizeof(big), &out, &out_size, METADATA_TYPE_USER_PRIVATE_6);
	assert_int_equal(out_size, 1 + 2 + 1 + sizeof(big) + 1);
	assert_int_equal(out[1], (202 & 0x7f) | 0x80);
	assert_int_equal(out[2], 202 >> 7);
	assert_int_equal(out[3], METADATA_TYPE_USER_PRIVATE_6);
	assert_int_equal(out[out_size - 1], 0x80);
	bfree(out);
}

static void av1_malformed_test(void **state)
{
	UNUSED_PARAMETER(state);

	uint8_t *data, *hdr;
	size_t data_size, hdr_size;

	/* Regression: an OBU size past the end of the packet was copied as is, reading out of bounds */
	static const uint8_t truncated[] = {0x0a, 0x05, 0xaa};
	obs_extract_av1_headers(truncated, sizeof(truncated), &data, &data_size, &hdr, &hdr_size);
	assert_int_equal(data_size, sizeof(truncated));
	assert_int_equal(hdr_size, sizeof(truncated));
	bfree(data);
	bfree(hdr);

	/* Regression: without a size field the extension byte was counted twice */
	static const uint8_t extension[] = {0x0c, 0x00, 0xaa};
	obs_extract_av1_headers(extension, sizeof(extension), &data, &data_size, &hdr, &hdr_size);
	assert_int_equal(data_size, sizeof(extension));
	assert_int_equal(hdr_size, sizeof(extension));
	bfree(data);
	bfree(hdr);

	/* Regression: a frame OBU whose size points past the end read the byte after the packet */
	static const uint8_t frame_past_end[] = {0x32, 0x01, 0x00};
	assert_false(obs_av1_keyframe(frame_past_end, 2));

	/* Regression: the leb128 size was shifted as an int, so bits from 2^31 up were lost and a large size
	 * read as 0 */
	static const uint8_t large[] = {0x32, 0x80, 0x80, 0x80, 0x80, 0x10, 0x10, 0x00};
	assert_true(obs_av1_keyframe(large, sizeof(large)));
	obs_extract_av1_headers(large, sizeof(large), &data, &data_size, &hdr, &hdr_size);
	assert_int_equal(data_size, sizeof(large));
	assert_int_equal(hdr_size, 0);
	bfree(data);
	bfree(hdr);
}

int main(void)
{
	const struct CMUnitTest tests[] = {
		obs_unit_test(nal_test), obs_unit_test(avc_test),           obs_unit_test(hevc_test),
		obs_unit_test(av1_test), obs_unit_test(av1_malformed_test),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
