#pragma once

#include <obs.h>

#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <string>
#include <vector>

namespace knurbs {

class Paths;

// Loads every module in two parallel phases with a barrier in between:
//   1. obs_module_load for all modules at once, on a worker pool;
//   2. barrier: types are sorted, queued frontend API registrations are applied;
//   3. obs_module_post_load for all modules at once.
// Modules must not create Qt objects during either phase; Qt is only
// guaranteed once the frontend API hook fires.
class ModuleLoader : public QObject {
	Q_OBJECT

public:
	struct Options {
		bool coreOnly = false;    // safe mode
		bool bundledOnly = false; // --only-bundled-plugins
	};

	struct Result {
		bool coreModulesLoaded = false;
		QStringList failed;
		int loaded = 0;
		qint64 loadMs = 0;
		qint64 postLoadMs = 0;
	};

	// Called on the GUI thread between phases, while workers run.
	using Progress = std::function<void(const QString &status, int done, int total)>;

	explicit ModuleLoader(QObject *parent = nullptr);

	Result loadAll(const Paths &paths, const Options &options, const Progress &progress);

private:
	struct Found {
		std::string bin;
		std::string data;
		std::string name;
		obs_runtime_module_type type;
	};

	void findModules(const Paths &paths, const Options &options);
	static void onFound(void *param, const obs_module_info2 *info);

	// Runs `task(i)` for i in [0, count) on a worker pool and keeps the GUI
	// event loop running until all are done.
	void runParallel(size_t count, const std::function<void(size_t)> &task, const QString &phase,
			 const Progress &progress);

	std::vector<Found> found_;
};

} // namespace knurbs
