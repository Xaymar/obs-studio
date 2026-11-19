#pragma once

#include "caption-router.hpp"
#include "captions-config.hpp"

#include <obs.hpp>

#include <mutex>

namespace knurbs::captions {

// Everything knurbs-captions does, without its UI. One instance, owned by the
// plugin. UI-thread methods are marked; delivery runs on any thread.
class Captions {
public:
	Captions();
	~Captions();

	Captions(const Captions &) = delete;
	Captions &operator=(const Captions &) = delete;

	CaptionRouter &router() { return router_; }

	// UI thread. Applies a config: picks the source and the targets.
	void apply(const CaptionsConfig &config);
	const CaptionsConfig &config() const { return config_; }

	// UI thread. Points the targets at the frontend's current outputs.
	// Call when streaming or recording starts or stops.
	void refreshOutputs();

	// UI thread. Lets go of the source and the outputs, for example before
	// the scene collection goes away.
	void detach();

	// The libobs procedure behind knurbs_captions_submit_text().
	static void submitTextProc(void *data, calldata_t *cd);

private:
	void attachSource(const std::string &uuid);
	void detachSource();

	static void sourceCaptions(void *param, obs_source_t *source, const obs_source_cea_708 *captions);

	CaptionRouter router_;
	CaptionsConfig config_;
	OBSWeakSource source_;
};

} // namespace knurbs::captions
