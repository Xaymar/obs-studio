#pragma once

// Public API of knurbs-captions, for plugins that produce caption text, such
// as speech recognition.
//
// A provider submits finished lines of text. knurbs-captions sends them to
// every output the user picked (stream, recording) as CEA-608/708 captions.
// Nothing needs to be linked: the call goes through the libobs global
// procedure handler, so it is safe to call when knurbs-captions is not loaded
// (it then returns false).
//
// Call only after OBS_FRONTEND_EVENT_FINISHED_LOADING. Thread safe.

#include <obs.h>

#ifdef __cplusplus
extern "C" {
#endif

// Name of the procedure on obs_get_proc_handler(). Procedure names are C
// identifiers, so this cannot use the reverse-DNS form of other ids.
#define KNURBS_CAPTIONS_SUBMIT_TEXT_PROC "knurbs_captions_submit_text"
#define KNURBS_CAPTIONS_SUBMIT_TEXT_DECL \
	"void " KNURBS_CAPTIONS_SUBMIT_TEXT_PROC "(in string text, in float duration, out bool delivered)"

// Shows `text` (UTF-8, at most 32 characters a line, lines separated by \n)
// for `duration` seconds. Returns true if at least one output took it.
static inline bool knurbs_captions_submit_text(const char *text, double duration)
{
	if (!text) {
		return false;
	}

	calldata_t cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "text", text);
	calldata_set_float(&cd, "duration", duration);

	bool delivered = false;
	if (proc_handler_call(obs_get_proc_handler(), KNURBS_CAPTIONS_SUBMIT_TEXT_PROC, &cd)) {
		delivered = calldata_bool(&cd, "delivered");
	}
	calldata_free(&cd);
	return delivered;
}

#ifdef __cplusplus
}
#endif
