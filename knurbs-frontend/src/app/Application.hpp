#pragma once

#include "CommandLine.hpp"
#include "Paths.hpp"

#include <QApplication>
#include <QPointer>

#include <memory>

namespace knurbs {

class Core;
class MainWindow;
class SingleInstance;
class SplashScreen;

// Owns startup and shutdown order. See the design doc: QApplication first,
// splash right away, then arguments, paths, log, single instance, config,
// libobs, the hidden main window, parallel plugin loading, scene collection,
// and finally the main window.
class Application : public QApplication {
	Q_OBJECT

public:
	Application(int &argc, char **argv);
	~Application() override;

	int run();

	// The one way to quit: signals, the window and session end all call this.
	void requestQuit();

private:
	bool startup();
	void shutdown();
	void fail(const QString &message);
	void status(const QString &text);
	void installSignalHandlers();

	CommandLine options_;
	Paths paths_;
	QPointer<SplashScreen> splash_;
	std::unique_ptr<SingleInstance> instance_;
	std::unique_ptr<Core> core_;
	std::unique_ptr<MainWindow> window_;
	bool shuttingDown_ = false;
	bool quitRequested_ = false;
	bool failed_ = false;
};

} // namespace knurbs
