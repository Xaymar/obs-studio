#include "caption-router.hpp"

#include <obs.hpp>

namespace knurbs::captions {

namespace {

int index(Target target)
{
	return target == Target::Recording ? 1 : 0;
}

} // namespace

Sink output_sink(obs_output_t *output)
{
	if (!output) {
		return {};
	}

	OBSWeakOutput weak = OBSGetWeakRef(output);

	Sink sink;
	sink.cea708 = [weak](const obs_source_cea_708 &captions) {
		OBSOutputAutoRelease strong = obs_weak_output_get_output(weak);
		if (!strong || !obs_output_active(strong)) {
			return false;
		}
		obs_output_caption(strong, &captions);
		return true;
	};
	sink.text = [weak](const std::string &text, double duration) {
		OBSOutputAutoRelease strong = obs_weak_output_get_output(weak);
		if (!strong || !obs_output_active(strong)) {
			return false;
		}
		obs_output_output_caption_text2(strong, text.c_str(), duration);
		return true;
	};
	return sink;
}

void CaptionRouter::setEnabled(Target target, bool enabled)
{
	std::lock_guard lock(mutex_);
	slots_[index(target)].enabled = enabled;
}

bool CaptionRouter::enabled(Target target) const
{
	std::lock_guard lock(mutex_);
	return slots_[index(target)].enabled;
}

void CaptionRouter::setSink(Target target, Sink sink)
{
	// Swap under the lock, destroy the old sink outside it: dropping the
	// last weak reference of an output must not happen while holding it.
	Sink previous;
	{
		std::lock_guard lock(mutex_);
		previous = std::move(slots_[index(target)].sink);
		slots_[index(target)].sink = std::move(sink);
	}
}

bool CaptionRouter::deliver(const obs_source_cea_708 &captions)
{
	bool taken = false;
	std::lock_guard lock(mutex_);
	for (Slot &slot : slots_) {
		if (slot.enabled && slot.sink.cea708) {
			taken = slot.sink.cea708(captions) || taken;
		}
	}
	return taken;
}

bool CaptionRouter::deliverText(const std::string &text, double duration)
{
	bool taken = false;
	std::lock_guard lock(mutex_);
	for (Slot &slot : slots_) {
		if (slot.enabled && slot.sink.text) {
			taken = slot.sink.text(text, duration) || taken;
		}
	}
	return taken;
}

} // namespace knurbs::captions
