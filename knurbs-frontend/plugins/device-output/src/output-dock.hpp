#pragma once

#include "device-output.hpp"
#include "settings.hpp"

#include <QGroupBox>
#include <QPointer>
#include <QWidget>

#include <array>
#include <memory>

class OBSPropertiesView;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QVBoxLayout;

namespace knurbs::device_output {

// One slot in the dock: type, properties, auto start and start/stop.
class SlotPanel final : public QGroupBox {
	Q_OBJECT

public:
	SlotPanel(Slot slot, std::vector<const OutputType *> types, QWidget *parent);
	~SlotPanel() override;

	Slot slot() const { return output_.slot(); }

	// Replaces what the panel shows and edits. Stops a running output.
	void load(const SlotConfig &config);
	const SlotConfig &config() const { return config_; }

	void autoStart();
	void stopOutput();

	void handleFrontendEvent(obs_frontend_event event) { output_.handleFrontendEvent(event); }

signals:
	// The config changed and should be saved.
	void configChanged();

private:
	void typeSelected(int index);
	void autoStartToggled(bool checked);
	void startStopClicked();
	void propertiesChanged();
	void rebuildProperties();
	void outputStateChanged(bool running);
	void updateState();

	const OutputType *selectedType() const;

	DeviceOutput output_;
	std::vector<const OutputType *> types_;
	SlotConfig config_;
	// What the running output last got, to tell which change needs what.
	OBSData applied_;

	QComboBox *typeBox_ = nullptr;
	QCheckBox *autoStartBox_ = nullptr;
	QPushButton *startStop_ = nullptr;
	QLabel *hint_ = nullptr;
	QVBoxLayout *propertiesLayout_ = nullptr;
	QPointer<OBSPropertiesView> properties_;
};

// The "Device Outputs" dock.
class OutputDock final : public QWidget {
	Q_OBJECT

public:
	explicit OutputDock(QWidget *parent = nullptr);

	// Loads the current profile's config and starts the outputs set to
	// start automatically.
	void loadProfile();
	// Stops every output and saves the config.
	void stopAll();
	void save();

	void handleFrontendEvent(obs_frontend_event event);

private:
	std::string configPath() const;

	std::vector<SlotPanel *> panels_;
};

} // namespace knurbs::device_output
