// Tests for the parts of knurbs-captions that need neither Qt nor a running
// libobs: routing, the saved config and the provider procedure.

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <setjmp.h>
extern "C" {
#include <cmocka.h>
}

#include "caption-router.hpp"
#include "captions-config.hpp"
#include "captions.hpp"

#include <knurbs-captions-api.h>

#include <obs.hpp>

#include <string>
#include <vector>

using namespace knurbs::captions;

namespace {

struct Recorder {
	std::vector<std::string> texts;
	int cea708 = 0;
	bool accept = true;

	Sink sink()
	{
		Sink s;
		s.cea708 = [this](const obs_source_cea_708 &) {
			cea708++;
			return accept;
		};
		s.text = [this](const std::string &text, double) {
			texts.push_back(text);
			return accept;
		};
		return s;
	}
};

obs_source_cea_708 some_captions()
{
	static uint8_t bytes[3] = {0xFC, 0x94, 0x2C};
	obs_source_cea_708 captions = {};
	captions.data = bytes;
	captions.packets = 1;
	return captions;
}

} // namespace

// ---------------------------------------------------------------- router

static void router_sends_only_to_enabled_targets(void **)
{
	CaptionRouter router;
	Recorder stream, recording;
	router.setSink(Target::Stream, stream.sink());
	router.setSink(Target::Recording, recording.sink());

	// Nothing enabled yet.
	assert_false(router.deliver(some_captions()));
	assert_false(router.deliverText("hello", 2.0));
	assert_int_equal(stream.cea708, 0);

	router.setEnabled(Target::Stream, true);
	assert_true(router.deliver(some_captions()));
	assert_true(router.deliverText("hello", 2.0));
	assert_int_equal(stream.cea708, 1);
	assert_int_equal(stream.texts.size(), 1);
	assert_int_equal(recording.cea708, 0);

	// decklink-captions could only reach the stream; now both work.
	router.setEnabled(Target::Recording, true);
	router.deliverText("both", 2.0);
	assert_string_equal(stream.texts.back().c_str(), "both");
	assert_string_equal(recording.texts.back().c_str(), "both");
}

static void router_reports_when_no_sink_takes_it(void **)
{
	CaptionRouter router;
	Recorder stream;
	stream.accept = false;
	router.setEnabled(Target::Stream, true);
	router.setSink(Target::Stream, stream.sink());
	assert_false(router.deliverText("x", 1.0));
	assert_int_equal(stream.texts.size(), 1);

	// A cleared sink takes nothing and does not crash.
	router.setSink(Target::Stream, {});
	assert_false(router.deliver(some_captions()));
	assert_false(router.deliverText("x", 1.0));
}

static void output_sink_of_null_is_empty(void **)
{
	Sink sink = output_sink(nullptr);
	assert_false((bool)sink.cea708);
	assert_false((bool)sink.text);
}

// ---------------------------------------------------------------- config

static void config_defaults(void **)
{
	CaptionsConfig config = config_from_data(nullptr, nullptr);
	assert_true(config.sourceUuid.empty());
	assert_true(config.stream);
	assert_false(config.recording);

	OBSDataAutoRelease empty = obs_data_create();
	config = config_from_data(empty, nullptr);
	assert_true(config.sourceUuid.empty());
	assert_true(config.stream);
}

static void config_round_trip(void **)
{
	CaptionsConfig config;
	config.sourceUuid = "8b1d6a2c-0000-4000-8000-000000000001";
	config.stream = false;
	config.recording = true;

	OBSDataAutoRelease collection = obs_data_create();
	config_to_data(config, collection);

	CaptionsConfig loaded = config_from_data(collection, nullptr);
	assert_string_equal(loaded.sourceUuid.c_str(), config.sourceUuid.c_str());
	assert_false(loaded.stream);
	assert_true(loaded.recording);
}

// decklink-captions saved the source by name, so renaming the source lost the
// link. The name is read once, turned into a UUID, and saved as the UUID.
static void config_migrates_decklink_captions(void **)
{
	OBSDataAutoRelease collection = obs_data_create();
	OBSDataAutoRelease legacy = obs_data_create();
	obs_data_set_string(legacy, "source", "DeckLink In");
	obs_data_set_obj(collection, kLegacyConfigKey, legacy);

	std::vector<std::string> asked;
	auto lookup = [&asked](const char *name) {
		asked.emplace_back(name);
		return std::string("uuid-of-decklink-in");
	};

	CaptionsConfig config = config_from_data(collection, lookup);
	assert_int_equal(asked.size(), 1);
	assert_string_equal(asked[0].c_str(), "DeckLink In");
	assert_string_equal(config.sourceUuid.c_str(), "uuid-of-decklink-in");
	assert_true(config.stream);
	assert_false(config.recording);

	// Once saved, the UUID is used and the name is no longer looked up.
	config_to_data(config, collection);
	asked.clear();
	CaptionsConfig again = config_from_data(collection, lookup);
	assert_int_equal(asked.size(), 0);
	assert_string_equal(again.sourceUuid.c_str(), "uuid-of-decklink-in");
}

static void config_migration_without_the_source(void **)
{
	OBSDataAutoRelease collection = obs_data_create();
	OBSDataAutoRelease legacy = obs_data_create();
	obs_data_set_string(legacy, "source", "Gone");
	obs_data_set_obj(collection, kLegacyConfigKey, legacy);

	CaptionsConfig config = config_from_data(collection, [](const char *) { return std::string(); });
	assert_true(config.sourceUuid.empty());

	// An empty legacy name is not looked up at all.
	obs_data_set_string(legacy, "source", "");
	bool asked = false;
	config = config_from_data(collection, [&asked](const char *) {
		asked = true;
		return std::string("x");
	});
	assert_false(asked);
	assert_true(config.sourceUuid.empty());
}

// ---------------------------------------------------------------- provider API

static void submit_text_procedure(void **)
{
	Captions captions;
	Recorder stream;
	CaptionsConfig config;
	config.stream = true;
	captions.apply(config);
	captions.router().setSink(Target::Stream, stream.sink());

	// The declaration must parse, or providers could never reach us.
	proc_handler_t *handler = proc_handler_create();
	proc_handler_add(handler, KNURBS_CAPTIONS_SUBMIT_TEXT_DECL, Captions::submitTextProc, &captions);

	calldata_t cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "text", "Hello world");
	calldata_set_float(&cd, "duration", 3.0);
	assert_true(proc_handler_call(handler, KNURBS_CAPTIONS_SUBMIT_TEXT_PROC, &cd));
	assert_true(calldata_bool(&cd, "delivered"));
	assert_int_equal(stream.texts.size(), 1);
	assert_string_equal(stream.texts[0].c_str(), "Hello world");

	// A zero duration would never show; it is refused.
	calldata_set_float(&cd, "duration", 0.0);
	assert_true(proc_handler_call(handler, KNURBS_CAPTIONS_SUBMIT_TEXT_PROC, &cd));
	assert_false(calldata_bool(&cd, "delivered"));
	assert_int_equal(stream.texts.size(), 1);

	calldata_free(&cd);
	proc_handler_destroy(handler);
}

int main()
{
	const struct CMUnitTest tests[] = {
		cmocka_unit_test(router_sends_only_to_enabled_targets),
		cmocka_unit_test(router_reports_when_no_sink_takes_it),
		cmocka_unit_test(output_sink_of_null_is_empty),
		cmocka_unit_test(config_defaults),
		cmocka_unit_test(config_round_trip),
		cmocka_unit_test(config_migrates_decklink_captions),
		cmocka_unit_test(config_migration_without_the_source),
		cmocka_unit_test(submit_text_procedure),
	};
	return cmocka_run_group_tests(tests, nullptr, nullptr);
}
