#pragma once

#include <obs.h>

#include <functional>
#include <mutex>
#include <string>

namespace knurbs::captions {

// Where captions can go.
enum class Target {
	Stream,
	Recording,
};

inline constexpr int kTargetCount = 2;

// Something that takes captions: normally a libobs output.
struct Sink {
	// Returns true if the caption was taken.
	std::function<bool(const obs_source_cea_708 &captions)> cea708;
	std::function<bool(const std::string &text, double duration)> text;
};

// A sink for a libobs output. Captions reach it only while it is active.
// Holds a weak reference, so a sink never keeps an output alive.
Sink output_sink(obs_output_t *output);

// Sends captions from every provider to the targets the user enabled.
//
// Providers call deliver*() from any thread (a source's caption callback,
// a speech plugin's worker). The UI thread changes targets and sinks.
class CaptionRouter {
public:
	void setEnabled(Target target, bool enabled);
	bool enabled(Target target) const;

	// Replaces the sink of a target, or clears it with an empty Sink.
	void setSink(Target target, Sink sink);

	// Each returns true if at least one target took the captions.
	bool deliver(const obs_source_cea_708 &captions);
	bool deliverText(const std::string &text, double duration);

private:
	struct Slot {
		bool enabled = false;
		Sink sink;
	};

	mutable std::mutex mutex_;
	Slot slots_[kTargetCount];
};

} // namespace knurbs::captions
