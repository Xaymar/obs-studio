#include "Bootstrap.hpp"

#include <QApplication>
#include <QGuiApplication>

#include <util/base.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace knurbs::bootstrap {

void harden_process()
{
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS);
	SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT);
	SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	SetDllDirectoryW(L"");

	// Close before child processes such as CEF when the session ends.
	SetProcessShutdownParameters(0x300, SHUTDOWN_NORETRY);
#endif
}

static void crash_handler(const char *format, va_list args, void *)
{
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	fflush(stderr);
	std::abort();
}

void install_crash_handler()
{
	base_set_crash_handler(crash_handler, nullptr);
}

void set_application_attributes()
{
	QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);

	// Preview and projectors are drawn by Qt from offscreen frames, so no
	// widget needs a native window of its own.
	QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);

#if !defined(_WIN32) && !defined(__APPLE__)
	// Linux is Wayland only; X11 is not supported. A Wayland variant or
	// offscreen (headless tests) may be chosen explicitly; anything else,
	// including xcb, is replaced with wayland.
	const QByteArray platform = qgetenv("QT_QPA_PLATFORM");
	if (!platform.startsWith("wayland") && platform != "offscreen") {
		qputenv("QT_QPA_PLATFORM", "wayland");
	}
#endif

	QCoreApplication::setOrganizationName(QStringLiteral("xaymar"));
	QCoreApplication::setOrganizationDomain(QStringLiteral("xaymar.com"));
	QCoreApplication::setApplicationName(QStringLiteral("knurbs"));
	QCoreApplication::setApplicationVersion(QStringLiteral("0.0.0"));
	// Reverse-DNS id; on Wayland this is the app_id compositors match against.
	QGuiApplication::setDesktopFileName(QStringLiteral("com.xaymar.knurbs"));
}

} // namespace knurbs::bootstrap
