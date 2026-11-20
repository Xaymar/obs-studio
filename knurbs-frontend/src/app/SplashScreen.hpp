#pragma once

#include <QWidget>

class QLabel;
class QProgressBar;

namespace knurbs {

// Small startup window that shows what Knurbs is doing. It is a plain widget
// and needs nothing from libobs, so it is shown right after QApplication.
class SplashScreen : public QWidget {
	Q_OBJECT

public:
	SplashScreen();

public slots:
	// Sets the line that says what is happening now. Repaints at once.
	void setStatus(const QString &status);

	// Sets overall progress. A negative total shows a busy bar.
	void setProgress(int value, int total);

private:
	QLabel *status_;
	QProgressBar *progress_;
};

} // namespace knurbs
