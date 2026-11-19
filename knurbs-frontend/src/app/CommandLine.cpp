#include "CommandLine.hpp"

#include <QCommandLineParser>
#include <QCoreApplication>

namespace knurbs {

bool CommandLine::parse(const QStringList &arguments, CommandLine &out, QString &message)
{
	QCommandLineParser parser;
	parser.setApplicationDescription(QStringLiteral("Knurbs"));
	auto help = parser.addHelpOption();
	auto version = parser.addVersionOption();

	QCommandLineOption portable({"p", "portable"}, "Keep all settings, logs and caches next to the program.");
	QCommandLineOption multi({"m", "multi"}, "Allow more than one running instance.");
	QCommandLineOption verbose("verbose", "Write more detail to the log.");
	QCommandLineOption safeMode("safe-mode", "Load only core modules; no plugins.");
	QCommandLineOption onlyBundled("only-bundled-plugins", "Load only plugins shipped with Knurbs.");
	QCommandLineOption studioMode("studio-mode", "Start in studio mode.");
	QCommandLineOption profile("profile", "Start with this profile.", "name");
	QCommandLineOption collection("collection", "Start with this scene collection.", "name");
	QCommandLineOption scene("scene", "Start with this scene.", "name");

	parser.addOptions({portable, multi, verbose, safeMode, onlyBundled, studioMode, profile, collection, scene});

	if (!parser.parse(arguments)) {
		message = parser.errorText() + QStringLiteral("\n\n") + parser.helpText();
		return false;
	}
	if (parser.isSet(help)) {
		message = parser.helpText();
		return false;
	}
	if (parser.isSet(version)) {
		message =
			QCoreApplication::applicationName() + QLatin1Char(' ') + QCoreApplication::applicationVersion();
		return false;
	}

	out.portable = parser.isSet(portable);
	out.multi = parser.isSet(multi);
	out.verbose = parser.isSet(verbose);
	out.safeMode = parser.isSet(safeMode);
	out.onlyBundledPlugins = parser.isSet(onlyBundled);
	out.studioMode = parser.isSet(studioMode);
	out.profile = parser.value(profile);
	out.collection = parser.value(collection);
	out.scene = parser.value(scene);
	return true;
}

} // namespace knurbs
