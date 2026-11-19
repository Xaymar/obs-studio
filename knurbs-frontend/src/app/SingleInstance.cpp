#include "SingleInstance.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QLocalSocket>

namespace knurbs {

SingleInstance::SingleInstance(const QString &lockDirectory, QObject *parent)
	: QObject(parent),
	  lock_(QDir(lockDirectory).filePath(QStringLiteral("knurbs.lock")))
{
	lock_.setStaleLockTime(0);
}

QString SingleInstance::serverName() const
{
	// Unique per lock file, and so per user and per portable install.
	const QByteArray hash = QCryptographicHash::hash(lock_.fileName().toUtf8(), QCryptographicHash::Sha1).toHex();
	return QStringLiteral("com.xaymar.knurbs.instance.") + QString::fromLatin1(hash.left(16));
}

bool SingleInstance::acquire(const QStringList &arguments)
{
	if (lock_.tryLock(100)) {
		server_ = std::make_unique<QLocalServer>();
		server_->setSocketOptions(QLocalServer::UserAccessOption);
		QLocalServer::removeServer(serverName());
		server_->listen(serverName());
		connect(server_.get(), &QLocalServer::newConnection, this, [this]() {
			while (QLocalSocket *socket = server_->nextPendingConnection()) {
				connect(socket, &QLocalSocket::readyRead, this, [this, socket]() {
					QDataStream stream(socket);
					QStringList received;
					stream >> received;
					emit argumentsReceived(received);
					socket->deleteLater();
				});
			}
		});
		return true;
	}

	QLocalSocket socket;
	socket.connectToServer(serverName());
	if (socket.waitForConnected(1000)) {
		QDataStream stream(&socket);
		stream << arguments;
		socket.waitForBytesWritten(1000);
	}
	return false;
}

} // namespace knurbs
