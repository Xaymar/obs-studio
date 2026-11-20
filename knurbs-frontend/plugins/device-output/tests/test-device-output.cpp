// Tests for the parts of knurbs-device-output that need neither Qt nor a
// running libobs: frame copies, output types and the saved config.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}

#include "frame-copy.hpp"
#include "output-types.hpp"
#include "settings.hpp"

#include <util/platform.h>

#include <cstring>
#include <string>
#include <vector>

using namespace knurbs::device_output;

// ---------------------------------------------------------------- copy_rows

// The old decklink/aja output UIs copied `dst_linesize` bytes per row out of
// the staging surface. When the destination rows are wider than the staged
// rows, that reads past each staged row and, on the last row, past the end of
// the mapping. Only the picture bytes may be copied.
static void copy_rows_wider_destination(void **)
{
	const uint32_t rows = 3, row_bytes = 8, src_linesize = 8, dst_linesize = 16;

	std::vector<uint8_t> src(src_linesize * rows);
	for (size_t i = 0; i < src.size(); i++) {
		src[i] = (uint8_t)(i + 1);
	}
	// A guard after the source: a copy of dst_linesize bytes from the last
	// row would pick these up.
	src.resize(src.size() + 16, 0xEE);

	std::vector<uint8_t> dst(dst_linesize * rows, 0xAA);
	copy_rows(dst.data(), dst_linesize, src.data(), src_linesize, row_bytes, rows);

	for (uint32_t y = 0; y < rows; y++) {
		assert_memory_equal(dst.data() + y * dst_linesize, src.data() + y * src_linesize, row_bytes);
		for (uint32_t x = row_bytes; x < dst_linesize; x++) {
			assert_int_equal(dst[y * dst_linesize + x], 0xAA);
		}
	}
}

static void copy_rows_wider_source(void **)
{
	const uint32_t rows = 2, row_bytes = 4, src_linesize = 12, dst_linesize = 4;

	std::vector<uint8_t> src(src_linesize * rows);
	for (size_t i = 0; i < src.size(); i++) {
		src[i] = (uint8_t)(i + 1);
	}
	std::vector<uint8_t> dst(dst_linesize * rows + 4, 0xAA);
	copy_rows(dst.data(), dst_linesize, src.data(), src_linesize, row_bytes, rows);

	assert_memory_equal(dst.data(), src.data(), 4);
	assert_memory_equal(dst.data() + 4, src.data() + 12, 4);
	// Nothing written past the destination.
	for (size_t i = dst_linesize * rows; i < dst.size(); i++) {
		assert_int_equal(dst[i], 0xAA);
	}
}

static void copy_rows_never_more_than_a_row(void **)
{
	// A row_bytes larger than either line size is clamped.
	std::vector<uint8_t> src(4, 0x11);
	std::vector<uint8_t> dst(8, 0xAA);
	copy_rows(dst.data(), 4, src.data(), 4, 64, 1);
	for (size_t i = 0; i < 4; i++) {
		assert_int_equal(dst[i], 0x11);
	}
	for (size_t i = 4; i < 8; i++) {
		assert_int_equal(dst[i], 0xAA);
	}
}

// ---------------------------------------------------------------- output types

static void types_for_filters_slot_and_registration(void **)
{
	auto all = [](const char *) {
		return true;
	};
	auto only_decklink = [](const char *id) {
		return std::strcmp(id, "decklink_output") == 0;
	};

	std::vector<const OutputType *> program = types_for(Slot::Program, all);
	assert_int_equal(program.size(), 2);
	assert_string_equal(program[0]->id, "decklink_output");
	assert_string_equal(program[1]->id, "aja_output");

	std::vector<const OutputType *> audio = types_for(Slot::Audio, all);
	assert_int_equal(audio.size(), 1);
	assert_string_equal(audio[0]->id, "asio_output");

	assert_int_equal(types_for(Slot::Preview, only_decklink).size(), 1);
	assert_int_equal(types_for(Slot::Audio, only_decklink).size(), 0);
}

static void feeds(void **)
{
	const OutputType *decklink = find_type("decklink_output");
	const OutputType *aja = find_type("aja_output");
	const OutputType *asio = find_type("asio_output");
	assert_non_null(decklink);
	assert_non_null(aja);
	assert_non_null(asio);
	assert_null(find_type("nope"));

	// DeckLink always got rendered frames; AJA's program output took
	// libobs video directly. The preview is always rendered.
	assert_true(feed_for(*decklink, Slot::Program) == Feed::Rendered);
	assert_true(feed_for(*aja, Slot::Program) == Feed::Direct);
	assert_true(feed_for(*aja, Slot::Preview) == Feed::Rendered);
	assert_true(feed_for(*asio, Slot::Audio) == Feed::AudioOnly);
}

static obs_data_t *asio_settings(const char *device, int64_t ch0)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "device_name", device);
	obs_data_set_int(data, "device_ch0", ch0);
	return data;
}

static void asio_restart_rules(void **)
{
	const OutputType *asio = find_type("asio_output");
	assert_non_null(asio);
	assert_true(asio->live_update);
	assert_non_null(asio->needs_restart);

	// Bitmask for channel c of track t: c + (1 << (t + 4)).
	const int64_t track0 = 0 + (1LL << 4);
	const int64_t track1 = 0 + (1LL << 5);
	const int64_t monitoring = 0 + (1LL << (MAX_AUDIO_MIXES + 4));

	OBSDataAutoRelease a = asio_settings("dev", track0);
	OBSDataAutoRelease b = asio_settings("dev", track1);
	OBSDataAutoRelease c = asio_settings("other", track0);
	OBSDataAutoRelease d = asio_settings("dev", monitoring);
	OBSDataAutoRelease e = asio_settings("dev", -1);

	assert_false(asio->needs_restart(a, a));
	// Moving between regular tracks applies live.
	assert_false(asio->needs_restart(a, b));
	// A different device restarts.
	assert_true(asio->needs_restart(a, c));
	// Onto or off the monitoring track restarts.
	assert_true(asio->needs_restart(a, d));
	assert_true(asio->needs_restart(d, a));
	assert_true(asio->needs_restart(e, d));
	// Unrouting a regular track applies live.
	assert_false(asio->needs_restart(a, e));
}

// ---------------------------------------------------------------- config

static void config_round_trip(void **)
{
	Config config;
	config.program.type = "decklink_output";
	config.program.autoStart = true;
	OBSDataAutoRelease program = obs_data_create();
	obs_data_set_string(program, "device_hash", "abc");
	config.program.settings = program.Get();

	config.audio.type = "asio_output";
	OBSDataAutoRelease audio = obs_data_create();
	obs_data_set_string(audio, "device_name", "dev");
	config.audio.settings = audio.Get();

	OBSDataAutoRelease empty = obs_data_create();
	config.preview.settings = empty.Get();

	OBSData saved = config_to_data(config);
	assert_int_equal(obs_data_get_int(saved, "version"), 1);

	Config loaded = config_from_data(saved);
	assert_string_equal(loaded.program.type.c_str(), "decklink_output");
	assert_true(loaded.program.autoStart);
	assert_string_equal(obs_data_get_string(loaded.program.settings, "device_hash"), "abc");
	assert_string_equal(loaded.audio.type.c_str(), "asio_output");
	assert_string_equal(obs_data_get_string(loaded.audio.settings, "device_name"), "dev");
	assert_true(loaded.preview.type.empty());
	assert_false(loaded.preview.autoStart);
	assert_non_null(loaded.preview.settings.Get());

	// The loaded settings are copies, not the saved objects.
	obs_data_set_string(loaded.program.settings, "device_hash", "changed");
	assert_string_equal(obs_data_get_string(program, "device_hash"), "abc");
}

static void config_rejects_unknown_and_wrong_slot_types(void **)
{
	OBSDataAutoRelease data = obs_data_create();

	OBSDataAutoRelease program = obs_data_create();
	obs_data_set_string(program, "type", "not_a_device");
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "x", "y");
	obs_data_set_obj(program, "settings", settings);
	obs_data_set_obj(data, "program", program);

	// ASIO is audio only.
	OBSDataAutoRelease preview = obs_data_create();
	obs_data_set_string(preview, "type", "asio_output");
	obs_data_set_obj(data, "preview", preview);

	Config config = config_from_data(data);
	assert_true(config.program.type.empty());
	// Settings of a type that is not used are not carried along.
	assert_false(obs_data_has_user_value(config.program.settings, "x"));
	assert_true(config.preview.type.empty());
}

static void config_defaults_without_data(void **)
{
	Config config = config_from_data(nullptr);
	for (Slot slot : {Slot::Program, Slot::Preview, Slot::Audio}) {
		assert_true(config.at(slot).type.empty());
		assert_false(config.at(slot).autoStart);
		assert_non_null(config.at(slot).settings.Get());
	}

	Config missing = load_config("/nonexistent/com.xaymar.knurbs.device-output.json");
	assert_true(missing.program.type.empty());
	assert_false(save_config(missing, ""));
}

static void config_file_round_trip(void **)
{
	char *dir = os_get_abs_path_ptr(".");
	std::string path = std::string(dir) + "/test-device-output-config.json";
	bfree(dir);
	os_unlink(path.c_str());

	Config config = config_from_data(nullptr);
	config.preview.type = "aja_output";
	config.preview.autoStart = true;
	assert_true(save_config(config, path));

	Config loaded = load_config(path);
	assert_string_equal(loaded.preview.type.c_str(), "aja_output");
	assert_true(loaded.preview.autoStart);

	os_unlink(path.c_str());
	std::string backup = path + ".bak";
	os_unlink(backup.c_str());
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(copy_rows_wider_destination),
		cmocka_unit_test(copy_rows_wider_source),
		cmocka_unit_test(copy_rows_never_more_than_a_row),
		cmocka_unit_test(types_for_filters_slot_and_registration),
		cmocka_unit_test(feeds),
		cmocka_unit_test(asio_restart_rules),
		cmocka_unit_test(config_round_trip),
		cmocka_unit_test(config_rejects_unknown_and_wrong_slot_types),
		cmocka_unit_test(config_defaults_without_data),
		cmocka_unit_test(config_file_round_trip),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
