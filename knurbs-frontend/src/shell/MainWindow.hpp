#pragma once

#include <QHash>
#include <QMainWindow>
#include <QPointer>

class QDockWidget;
class QMenu;

namespace ads {
class CDockManager;
class CDockWidget;
} // namespace ads

namespace knurbs {

class Core;

// The main window. A thin shell over Core: it shows state and sends commands,
// and owns no libobs objects itself. Docking uses Qt Advanced Docking; docks
// from plugins are wrapped into ads::CDockWidget.
class MainWindow : public QMainWindow {
	Q_OBJECT

public:
	explicit MainWindow(Core &core);
	~MainWindow() override;

	QMenu *toolsMenu() const { return toolsMenu_; }

	// Dock support for obs-frontend-api.
	bool addPluginDock(const QString &id, const QString &title, QWidget *widget);
	void removePluginDock(const QString &id);
	bool addCustomQDock(const QString &id, QDockWidget *dock);

	// Closes the window for real; called by Application during shutdown.
	void closeForShutdown();

signals:
	void closeRequested();

protected:
	void closeEvent(QCloseEvent *event) override;

private:
	ads::CDockWidget *wrap(const QString &id, const QString &title, QWidget *widget);

	Core &core_;
	ads::CDockManager *dockManager_ = nullptr;
	QMenu *toolsMenu_ = nullptr;
	QMenu *docksMenu_ = nullptr;
	QHash<QString, QPointer<ads::CDockWidget>> pluginDocks_;
	bool closing_ = false;
};

} // namespace knurbs
