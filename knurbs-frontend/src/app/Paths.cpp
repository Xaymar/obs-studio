#include "Paths.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace knurbs {

static QString join(const QString &base, const char *child)
{
	return QDir(base).filePath(QString::fromUtf8(child));
}

Paths Paths::resolve(bool portable)
{
	Paths p;

	if (portable) {
		const QString base = join(QCoreApplication::applicationDirPath(), "data");
		p.roaming_ = join(base, "config");
		p.local_ = join(base, "local");
		p.cache_ = join(base, "cache");
		return p;
	}

	// The organization and application names (xaymar, knurbs) are set in
	// Bootstrap, so Qt appends "xaymar/knurbs" to every location below.
#if defined(_WIN32)
	p.roaming_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);    // %APPDATA%
	p.local_ = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation); // %LOCALAPPDATA%
	p.cache_ = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);        // %LOCALAPPDATA%\...\cache
#elif defined(__APPLE__)
	p.roaming_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation); // Application Support
	p.local_ = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
	p.cache_ = QStandardPaths::writableLocation(QStandardPaths::CacheLocation); // Caches
#else
	p.roaming_ = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation); // ~/.config
	p.cache_ = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);       // ~/.cache
	// XDG state (logs, crash reports). Qt only gained StateLocation in 6.7.
	QString state = qEnvironmentVariable("XDG_STATE_HOME");
	if (state.isEmpty()) {
		state = QDir::home().filePath(QStringLiteral(".local/state"));
	}
	p.local_ = QDir(state).filePath(QCoreApplication::organizationName() + QLatin1Char('/') +
					QCoreApplication::applicationName());
#endif
	return p;
}

bool Paths::create() const
{
	for (const QString &dir : {roaming_, local_, cache_, logs(), pluginConfig(), profiles(), sceneCollections()}) {
		if (!QDir().mkpath(dir)) {
			return false;
		}
	}
	return true;
}

QString Paths::logs() const
{
	return join(local_, "logs");
}

QString Paths::pluginConfig() const
{
	return join(roaming_, "plugin_config");
}

QString Paths::profiles() const
{
	return join(roaming_, "profiles");
}

QString Paths::sceneCollections() const
{
	return join(roaming_, "scene_collections");
}

} // namespace knurbs
