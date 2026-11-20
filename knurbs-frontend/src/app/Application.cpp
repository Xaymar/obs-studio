#include "Application.hpp"

#include "Log.hpp"
#include "SingleInstance.hpp"
#include "SplashScreen.hpp"
#include "api/FrontendApi.hpp"
#include "core/Core.hpp"
#include "core/ModuleLoader.hpp"
#include "shell/MainWindow.hpp"

#include <QLocale>
#include <QMessageBox>
#include <QSessionManager>

#include <obs.h>

#include <cstdio>

#ifndef _WIN32
#include <QSocketNotifier>

#include <csignal>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace knurbs {

#ifndef _WIN32
namespace {
int signalFds[2] = {-1, -1};

void onTerminationSignal(int)
{
	// Only async-signal-safe work here; the event loop picks it up.
	const char byte = 1;
	[[maybe_unused]] ssize_t written = ::write(signalFds[0], &byte, sizeof(byte));
}
} // namespace
#endif

Application::Application(int &argc, char **argv) : QApplication(argc, argv)
{
	setQuitOnLastWindowClosed(false);
	installSignalHandlers();
}

void Application::installSignalHandlers()
{
#ifndef _WIN32
	// SIGTERM and SIGINT shut down cleanly (save, EXIT event, libobs teardown)
	// instead of killing the process with the lock file left behind.
	if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, signalFds) != 0) {
		blog(LOG_WARNING, "[knurbs] Could not set up signal handling");
		return;
	}
	auto *notifier = new QSocketNotifier(signalFds[1], QSocketNotifier::Read, this);
	connect(notifier, &QSocketNotifier::activated, this, [this]() {
		char byte;
		[[maybe_unused]] ssize_t got = ::read(signalFds[1], &byte, sizeof(byte));
		blog(LOG_INFO, "[knurbs] Termination signal received, shutting down");
		requestQuit();
	});

	struct sigaction action = {};
	action.sa_handler = onTerminationSignal;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_RESTART;
	sigaction(SIGTERM, &action, nullptr);
	sigaction(SIGINT, &action, nullptr);
#endif
}

Application::~Application()
{
	shutdown();
}

void Application::status(const QString &text)
{
	blog(LOG_INFO, "[knurbs] %s", text.toUtf8().constData());
	if (splash_) {
		splash_->setStatus(text);
	}
}

void Application::fail(const QString &message)
{
	blog(LOG_ERROR, "[knurbs] %s", message.toUtf8().constData());
	failed_ = true;
	if (splash_) {
		splash_->hide();
	}
	// Offscreen (headless tests) has nobody to close the dialog.
	if (platformName() != QStringLiteral("offscreen")) {
		QMessageBox::critical(nullptr, QStringLiteral("Knurbs"), message);
	}
}

static std::string obs_locale()
{
	// libobs uses BCP 47 style names such as "en-US".
	return QLocale::system().name().replace(QLatin1Char('_'), QLatin1Char('-')).toStdString();
}

bool Application::startup()
{
	// 3. Splash, first thing after QApplication.
	splash_ = new SplashScreen();
	splash_->show();
	splash_->setStatus(tr("Starting"));

	// 4. Arguments.
	QString message;
	if (!CommandLine::parse(arguments(), options_, message)) {
		splash_->hide();
#ifdef _WIN32
		QMessageBox::information(nullptr, QStringLiteral("Knurbs"), message);
#else
		fprintf(stdout, "%s\n", message.toUtf8().constData());
#endif
		return false;
	}

	// Paths.
	paths_ = Paths::resolve(options_.portable);
	if (!paths_.create()) {
		fail(tr("Could not create the Knurbs data folders."));
		return false;
	}

	// 5. Log.
	if (!log::open(paths_.logs(), options_.verbose)) {
		fprintf(stderr, "Could not open a log file in %s\n", paths_.logs().toUtf8().constData());
	}
	blog(LOG_INFO, "[knurbs] Qt %s (runtime), %s (compiled); platform %s", qVersion(), QT_VERSION_STR,
	     platformName().toUtf8().constData());
	blog(LOG_INFO, "[knurbs] Portable mode: %s", options_.portable ? "on" : "off");

	// 6. Single instance.
	if (!options_.multi) {
		instance_ = std::make_unique<SingleInstance>(paths_.localData());
		if (!instance_->acquire(arguments())) {
			blog(LOG_INFO, "[knurbs] Another instance is running; handed over arguments");
			return false;
		}
	}

	// 7. Config and locale.
	status(tr("Loading settings"));
	core_ = std::make_unique<Core>(paths_);
	if (!core_->loadConfig()) {
		fail(tr("Could not open the Knurbs settings."));
		return false;
	}

	// 8. libobs.
	status(tr("Starting libobs"));
	if (!core_->startup(obs_locale())) {
		fail(tr("libobs failed to start."));
		return false;
	}

	// 9. Audio and video.
	status(tr("Starting video"));
	const int video = core_->resetVideo();
	if (video != OBS_VIDEO_SUCCESS) {
		fail(tr("Video could not start on Vulkan (error %1). Check that your GPU driver supports Vulkan.")
			     .arg(video));
		return false;
	}
	status(tr("Starting audio"));
	if (!core_->resetAudio()) {
		fail(tr("Audio could not start."));
		return false;
	}

	// 10. Hidden main window, then the frontend API, before any plugin loads.
	window_ = std::make_unique<MainWindow>(*core_);
	connect(window_.get(), &MainWindow::closeRequested, this, &Application::requestQuit);
	api::FrontendApi::install(*core_, *window_, options_.safeMode);

	// 11. Plugins, in parallel.
	status(tr("Loading plugins"));
	ModuleLoader loader;
	ModuleLoader::Options loadOptions;
	loadOptions.coreOnly = options_.safeMode;
	loadOptions.bundledOnly = options_.onlyBundledPlugins;
	const ModuleLoader::Result modules =
		loader.loadAll(paths_, loadOptions, [this](const QString &phase, int done, int total) {
			if (!splash_) {
				return;
			}
			splash_->setStatus(tr("%1 (%2 of %3)").arg(phase).arg(done).arg(total));
			splash_->setProgress(done, total);
		});
	if (!modules.coreModulesLoaded) {
		fail(tr("Core modules failed to load: %1. Please reinstall Knurbs.")
			     .arg(modules.failed.join(QStringLiteral(", "))));
		return false;
	}

	// 12. Scene collection and outputs.
	status(tr("Loading scene collection"));
	core_->loadSceneCollection();
	if (options_.studioMode) {
		core_->setStudioMode(true);
	}
	if (auto *api = api::FrontendApi::instance()) {
		api->emitEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED);
		api->emitEvent(OBS_FRONTEND_EVENT_FINISHED_LOADING);
	}

	// 13. Show the main window.
	splash_->close();
	window_->show();

	connect(this, &QGuiApplication::commitDataRequest, this, [this](QSessionManager &) { requestQuit(); });
	return true;
}

int Application::run()
{
	int result = 0;
	if (startup() && !quitRequested_) {
		result = exec();
	}
	shutdown();
	if (failed_) {
		result = 1;
	}
	return result;
}

void Application::requestQuit()
{
	if (shuttingDown_) {
		return;
	}
	quitRequested_ = true;
	quit();
}

void Application::shutdown()
{
	if (shuttingDown_) {
		return;
	}
	shuttingDown_ = true;

	if (splash_) {
		splash_->close();
	}

	if (auto *api = api::FrontendApi::instance()) {
		if (core_) {
			core_->save();
		}
		api->emitEvent(OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN);
		api->emitEvent(OBS_FRONTEND_EVENT_EXIT);
		obs_frontend_set_callbacks_internal(nullptr); // deletes the FrontendApi
	}

	if (window_) {
		window_->closeForShutdown();
		window_.reset();
	}

	if (core_) {
		core_->shutdown();
		core_.reset();
	}

	instance_.reset();
	log::close();
}

} // namespace knurbs
