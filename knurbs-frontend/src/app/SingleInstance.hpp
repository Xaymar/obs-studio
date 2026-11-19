#pragma once

#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QStringList>

#include <memory>

namespace knurbs {

// Makes sure only one Knurbs runs per user, the same way on every platform.
// A second launch hands its arguments to the running instance and exits.
class SingleInstance : public QObject {
	Q_OBJECT

public:
	explicit SingleInstance(const QString &lockDirectory, QObject *parent = nullptr);

	// Returns true if this process is the only instance. Otherwise sends
	// `arguments` to the running instance and returns false.
	bool acquire(const QStringList &arguments);

signals:
	void argumentsReceived(const QStringList &arguments);

private:
	QString serverName() const;

	QLockFile lock_;
	std::unique_ptr<QLocalServer> server_;
};

} // namespace knurbs
