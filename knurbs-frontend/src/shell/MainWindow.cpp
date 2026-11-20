#include "MainWindow.hpp"

#include "core/Core.hpp"

#include <DockAreaWidget.h>
#include <DockManager.h>
#include <DockWidget.h>

#include <QCloseEvent>
#include <QDockWidget>
#include <QLabel>
#include <QMenuBar>

#include <obs.h>

namespace knurbs {

MainWindow::MainWindow(Core &core) : QMainWindow(nullptr), core_(core)
{
	setObjectName(QStringLiteral("mainWindow"));
	setWindowTitle(QStringLiteral("Knurbs"));
	resize(1280, 800);

	ads::CDockManager::setConfigFlag(ads::CDockManager::OpaqueSplitterResize, true);
	ads::CDockManager::setConfigFlag(ads::CDockManager::FocusHighlighting, true);
	dockManager_ = new ads::CDockManager(this);

	// Preview placeholder. The real preview is a QRhiWidget showing frames
	// libobs renders offscreen; it needs the libobs-vulkan interop exports.
	auto *preview = new QLabel(tr("Preview"), this);
	preview->setObjectName(QStringLiteral("preview"));
	preview->setAlignment(Qt::AlignCenter);
	auto *previewDock = new ads::CDockWidget(tr("Preview"));
	// Dock ids are stored in the saved layout; ours use reverse-DNS ids.
	previewDock->setObjectName(QStringLiteral("com.xaymar.knurbs.dock.preview"));
	previewDock->setWidget(preview);
	previewDock->setFeature(ads::CDockWidget::DockWidgetClosable, false);
	dockManager_->setCentralWidget(previewDock);

	QMenu *fileMenu = menuBar()->addMenu(tr("&File"));
	fileMenu->setObjectName(QStringLiteral("fileMenu"));
	fileMenu->addAction(tr("E&xit"), this, &QWidget::close);

	docksMenu_ = menuBar()->addMenu(tr("&Docks"));
	docksMenu_->setObjectName(QStringLiteral("docksMenu"));

	toolsMenu_ = menuBar()->addMenu(tr("&Tools"));
	toolsMenu_->setObjectName(QStringLiteral("toolsMenu"));
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent *event)
{
	if (!closing_) {
		// Shutdown is driven by Application, which then closes us for real.
		event->ignore();
		emit closeRequested();
		return;
	}
	QMainWindow::closeEvent(event);
}

void MainWindow::closeForShutdown()
{
	closing_ = true;
	close();
}

ads::CDockWidget *MainWindow::wrap(const QString &id, const QString &title, QWidget *widget)
{
	auto *dock = new ads::CDockWidget(title);
	dock->setObjectName(id);
	dock->setWidget(widget, ads::CDockWidget::ForceNoScrollArea);
	dockManager_->addDockWidget(ads::RightDockWidgetArea, dock);
	docksMenu_->addAction(dock->toggleViewAction());
	pluginDocks_.insert(id, dock);
	return dock;
}

bool MainWindow::addPluginDock(const QString &id, const QString &title, QWidget *widget)
{
	if (!widget || id.isEmpty() || pluginDocks_.contains(id)) {
		blog(LOG_WARNING, "[knurbs] Dock '%s' was not added (missing widget or duplicate id)",
		     id.toUtf8().constData());
		return false;
	}
	wrap(id, title, widget);
	return true;
}

void MainWindow::removePluginDock(const QString &id)
{
	QPointer<ads::CDockWidget> dock = pluginDocks_.take(id);
	if (!dock) {
		return;
	}
	docksMenu_->removeAction(dock->toggleViewAction());
	dockManager_->removeDockWidget(dock);
	dock->deleteLater();
}

bool MainWindow::addCustomQDock(const QString &id, QDockWidget *qdock)
{
	if (!qdock || id.isEmpty() || pluginDocks_.contains(id)) {
		blog(LOG_WARNING, "[knurbs] Custom dock '%s' was not added (missing dock or duplicate id)",
		     id.toUtf8().constData());
		return false;
	}

	// ADS docks are not QDockWidgets. Move the plugin's content into a
	// CDockWidget and keep the QDockWidget as a hidden proxy, mirroring its
	// title and visibility so calls the plugin makes on it still have effect.
	QWidget *content = qdock->widget();
	if (!content) {
		content = new QWidget();
	}
	qdock->setWidget(nullptr);
	qdock->setParent(this);
	qdock->hide();

	ads::CDockWidget *dock = wrap(id, qdock->windowTitle(), content);
	connect(qdock, &QDockWidget::windowTitleChanged, dock, &ads::CDockWidget::setWindowTitle);
	connect(qdock->toggleViewAction(), &QAction::toggled, dock, &ads::CDockWidget::toggleView);
	connect(qdock, &QObject::destroyed, this, [this, id]() { removePluginDock(id); });
	return true;
}

} // namespace knurbs
