#include "ModuleLoader.hpp"

#include "api/FrontendApi.hpp"
#include "app/Paths.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <thread>

namespace knurbs {

ModuleLoader::ModuleLoader(QObject *parent) : QObject(parent) {}

void ModuleLoader::onFound(void *param, const obs_module_info2 *info)
{
	auto *self = static_cast<ModuleLoader *>(param);
	self->found_.push_back({info->bin_path ? info->bin_path : "", info->data_path ? info->data_path : "",
				info->name ? info->name : "", info->type});
}

static std::string path_of(const QString &base, const char *relative)
{
	return QDir::cleanPath(QDir(base).filePath(QString::fromUtf8(relative))).toStdString();
}

static void find_plugins(const std::string &bin, const std::string &data, obs_runtime_module_type type,
			 obs_find_module_callback2_t callback, void *param)
{
	obs_runtime_module_info info = {};
	info.path_info.binary = bin.c_str();
	info.path_info.data = data.c_str();
	info.type = type;
	info.name = nullptr;
	obs_find_plugins(&info, callback, param);
}

void ModuleLoader::findModules(const Paths &paths, const Options &options)
{
	found_.clear();
	obs_find_core_modules(&ModuleLoader::onFound, this);
	if (options.coreOnly) {
		return;
	}

	const QString app = QCoreApplication::applicationDirPath();

	// Bundled plugins, then the user's own plugins. Same layout on every
	// platform except where legacy (OBS Studio style) installs differ.
#if defined(_WIN32)
	const std::string bundledBin = path_of(app, "../../plugins/%module%");
	find_plugins(bundledBin, bundledBin + "/data", OBS_MODULE_TYPE_PLUGIN, &ModuleLoader::onFound, this);
	find_plugins(path_of(app, "../../obs-plugins/64bit/"), path_of(app, "../../data/obs-plugins/%module%"),
		     OBS_MODULE_TYPE_LEGACY_PLUGIN, &ModuleLoader::onFound, this);
#elif defined(__APPLE__)
	const std::string bundledBin = path_of(app, "../PlugIns/%module%.plugin/Contents/MacOS");
	find_plugins(bundledBin, path_of(app, "../PlugIns/%module%.plugin/Contents/Resources"), OBS_MODULE_TYPE_PLUGIN,
		     &ModuleLoader::onFound, this);
#else
	find_plugins(path_of(app, "../lib/knurbs/plugins/%module%"), path_of(app, "../share/knurbs/plugins/%module%"),
		     OBS_MODULE_TYPE_PLUGIN, &ModuleLoader::onFound, this);
	find_plugins(path_of(app, "../lib/obs-plugins"), path_of(app, "../share/obs/obs-plugins/%module%"),
		     OBS_MODULE_TYPE_LEGACY_PLUGIN, &ModuleLoader::onFound, this);
#endif

	if (!options.bundledOnly) {
		const std::string userBin = path_of(paths.roamingConfig(), "plugins/%module%");
		find_plugins(userBin, userBin + "/data", OBS_MODULE_TYPE_PLUGIN, &ModuleLoader::onFound, this);
	}

	// Same order on every run and machine.
	std::stable_sort(found_.begin(), found_.end(), [](const Found &a, const Found &b) {
		if ((a.type == OBS_MODULE_TYPE_CORE) != (b.type == OBS_MODULE_TYPE_CORE)) {
			return a.type == OBS_MODULE_TYPE_CORE;
		}
		return a.name < b.name;
	});
}

void ModuleLoader::runParallel(size_t count, const std::function<void(size_t)> &task, const QString &phase,
			       const Progress &progress)
{
	if (count == 0) {
		return;
	}

	std::atomic<size_t> next{0};
	std::atomic<size_t> done{0};

	const size_t workers = std::clamp<size_t>(std::thread::hardware_concurrency(), 2, 32);
	std::vector<std::thread> threads;
	threads.reserve(std::min(workers, count));
	for (size_t t = 0; t < std::min(workers, count); ++t) {
		threads.emplace_back([&]() {
			for (size_t i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
				task(i);
				done.fetch_add(1);
			}
		});
	}

	// The GUI thread never blocks here: it keeps running events (and the
	// splash) until every worker is finished.
	QEventLoop loop;
	QTimer timer;
	QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
		const size_t d = done.load();
		if (progress) {
			progress(phase, int(d), int(count));
		}
		if (d >= count) {
			loop.quit();
		}
	});
	timer.start(16);
	loop.exec(QEventLoop::ExcludeUserInputEvents);

	for (std::thread &thread : threads) {
		thread.join();
	}
}

ModuleLoader::Result ModuleLoader::loadAll(const Paths &paths, const Options &options, const Progress &progress)
{
	Result result;
	findModules(paths, options);
	blog(LOG_INFO, "[knurbs] Found %zu modules", found_.size());

	// Phase 1: load.
	QElapsedTimer timer;
	timer.start();
	std::vector<char> ok(found_.size(), 0);
	api::FrontendApi::beginModuleLoad();
	runParallel(
		found_.size(),
		[&](size_t i) {
			const Found &f = found_[i];
			obs_module_info2 info = {f.bin.c_str(), f.data.c_str(), f.name.c_str(), f.type};
			api::FrontendApi::setLoadingModule(int(i), f.name);
			ok[i] = obs_load_found_module(&info) ? 1 : 0;
			api::FrontendApi::setLoadingModule(-1, {});
		},
		tr("Loading plugins"), progress);
	result.loadMs = timer.restart();

	// Phase 2: barrier.
	bool coreOk = true;
	for (size_t i = 0; i < found_.size(); ++i) {
		if (ok[i]) {
			++result.loaded;
			continue;
		}
		result.failed << QString::fromStdString(found_[i].name);
		if (found_[i].type == OBS_MODULE_TYPE_CORE) {
			coreOk = false;
		}
	}
	result.coreModulesLoaded = coreOk;
	obs_set_core_modules_loaded(coreOk);
	obs_sort_registered_types();
	api::FrontendApi::endModuleLoad();

	// Phase 3: post-load.
	std::vector<obs_module_t *> modules;
	obs_enum_modules(
		[](void *param, obs_module_t *module) {
			static_cast<std::vector<obs_module_t *> *>(param)->push_back(module);
		},
		&modules);
	std::sort(modules.begin(), modules.end(), [](obs_module_t *a, obs_module_t *b) {
		return strcmp(obs_get_module_file_name(a), obs_get_module_file_name(b)) < 0;
	});

	obs_prepare_post_load_modules();
	api::FrontendApi::beginModuleLoad();
	runParallel(
		modules.size(),
		[&](size_t i) {
			api::FrontendApi::setLoadingModule(int(i), obs_get_module_file_name(modules[i]));
			obs_post_load_module(modules[i]);
			api::FrontendApi::setLoadingModule(-1, {});
		},
		tr("Finishing plugin setup"), progress);
	api::FrontendApi::endModuleLoad();
	result.postLoadMs = timer.elapsed();

	obs_log_loaded_modules();
	blog(LOG_INFO, "[knurbs] Modules: %d loaded, %lld failed; load %lld ms, post-load %lld ms", result.loaded,
	     (long long)result.failed.size(), (long long)result.loadMs, (long long)result.postLoadMs);
	return result;
}

} // namespace knurbs
