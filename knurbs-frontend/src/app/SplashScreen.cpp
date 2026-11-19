#include "SplashScreen.hpp"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QLabel>
#include <QProgressBar>
#include <QScreen>
#include <QVBoxLayout>

namespace knurbs {

SplashScreen::SplashScreen() : QWidget(nullptr, Qt::SplashScreen | Qt::FramelessWindowHint)
{
	setObjectName(QStringLiteral("splashScreen"));
	setAttribute(Qt::WA_DeleteOnClose);
	setFixedSize(420, 140);

	auto *title = new QLabel(QStringLiteral("Knurbs"), this);
	title->setObjectName(QStringLiteral("splashTitle"));
	QFont font = title->font();
	font.setPointSizeF(font.pointSizeF() * 1.8);
	font.setBold(true);
	title->setFont(font);

	status_ = new QLabel(this);
	status_->setObjectName(QStringLiteral("splashStatus"));
	status_->setWordWrap(true);

	progress_ = new QProgressBar(this);
	progress_->setObjectName(QStringLiteral("splashProgress"));
	progress_->setTextVisible(false);
	progress_->setRange(0, 0);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(20, 16, 20, 16);
	layout->addWidget(title);
	layout->addStretch();
	layout->addWidget(status_);
	layout->addWidget(progress_);

	if (QScreen *screen = QGuiApplication::primaryScreen()) {
		move(screen->availableGeometry().center() - rect().center());
	}
}

void SplashScreen::setStatus(const QString &status)
{
	status_->setText(status);
	status_->repaint();
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void SplashScreen::setProgress(int value, int total)
{
	if (total < 0) {
		progress_->setRange(0, 0);
	} else {
		progress_->setRange(0, total);
		progress_->setValue(value);
	}
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

} // namespace knurbs
