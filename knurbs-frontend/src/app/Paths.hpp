#pragma once

#include <QString>

namespace knurbs {

// Where Knurbs keeps its files. Separate from OBS Studio. Data that should
// follow the user (settings, profiles, scene collections) is kept apart from
// data that belongs to this machine (logs, crash reports, caches).
class Paths {
public:
	// Resolves all locations. Portable mode keeps everything next to the program.
	static Paths resolve(bool portable);

	// Creates every directory. Returns false if one could not be created.
	bool create() const;

	QString roamingConfig() const { return roaming_; } // settings, profiles, scene collections
	QString localData() const { return local_; }       // logs, crash reports, profiler data
	QString cache() const { return cache_; }           // thumbnails, shader and browser caches
	QString logs() const;
	QString pluginConfig() const;
	QString profiles() const;
	QString sceneCollections() const;

private:
	QString roaming_;
	QString local_;
	QString cache_;
};

} // namespace knurbs
