#pragma once

#include <QString>
#include <QStringList>

namespace knurbs {

struct CommandLine {
	bool portable = false;
	bool multi = false;
	bool verbose = false;
	bool safeMode = false;
	bool onlyBundledPlugins = false;
	bool studioMode = false;
	QString profile;
	QString collection;
	QString scene;

	// Parses QCoreApplication::arguments(). Prints help or version and
	// returns false when the application should exit right away.
	static bool parse(const QStringList &arguments, CommandLine &out, QString &message);
};

} // namespace knurbs
