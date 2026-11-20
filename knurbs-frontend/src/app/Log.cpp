#include "Log.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QTextStream>

#include <util/base.h>

#include <cstdarg>
#include <cstdio>

namespace knurbs::log {

namespace {
QMutex mutex;
QFile file;
QString path;
bool verboseLog = false;

const char *level_name(int level)
{
	switch (level) {
	case LOG_ERROR:
		return "error";
	case LOG_WARNING:
		return "warning";
	case LOG_INFO:
		return "info";
	default:
		return "debug";
	}
}

void log_handler(int level, const char *format, va_list args, void *)
{
	if (level == LOG_DEBUG && !verboseLog) {
		return;
	}

	char buffer[8192];
	va_list copy;
	va_copy(copy, args);
	vsnprintf(buffer, sizeof(buffer), format, copy);
	va_end(copy);

	const QByteArray line = QDateTime::currentDateTime().toString(Qt::ISODateWithMs).toUtf8() + " [" +
				level_name(level) + "] " + buffer + '\n';

	QMutexLocker lock(&mutex);
	if (file.isOpen()) {
		file.write(line);
		file.flush();
	}
#ifndef NDEBUG
	fwrite(line.constData(), 1, size_t(line.size()), stderr);
#endif
}

void qt_message_handler(QtMsgType type, const QMessageLogContext &, const QString &message)
{
	int level = LOG_DEBUG;
	switch (type) {
	case QtWarningMsg:
		level = LOG_WARNING;
		break;
	case QtCriticalMsg:
	case QtFatalMsg:
		level = LOG_ERROR;
		break;
	case QtInfoMsg:
		level = LOG_INFO;
		break;
	default:
		break;
	}
	blog(level, "[Qt] %s", message.toUtf8().constData());
}

void prune(const QString &directory, int keep)
{
	QDir dir(directory);
	const QStringList logs = dir.entryList({QStringLiteral("*.log")}, QDir::Files, QDir::Name | QDir::Reversed);
	for (int i = keep; i < logs.size(); ++i) {
		dir.remove(logs.at(i));
	}
}
} // namespace

bool open(const QString &directory, bool verbose, int keep)
{
	verboseLog = verbose;
	prune(directory, keep - 1);

	path = QDir(directory).filePath(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH-mm-ss")) +
					QStringLiteral(".log"));
	{
		QMutexLocker lock(&mutex);
		file.setFileName(path);
		if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
			return false;
		}
	}

	base_set_log_handler(log_handler, nullptr);
	qInstallMessageHandler(qt_message_handler);
	return true;
}

QString currentFile()
{
	return path;
}

void close()
{
	qInstallMessageHandler(nullptr);
	QMutexLocker lock(&mutex);
	file.close();
}

} // namespace knurbs::log
