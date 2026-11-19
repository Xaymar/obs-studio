#include "output-dock.hpp"

#include <properties-view.hpp>

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/util.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QThread>
#include <QVBoxLayout>

#include <string>

namespace knurbs::device_output {

namespace {

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

const char *slot_title(Slot slot)
{
	switch (slot) {
	case Slot::Program:
		return "Slot.Program";
	case Slot::Preview:
		return "Slot.Preview";
	case Slot::Audio:
		return "Slot.Audio";
	}
	return "Slot.Program";
}

bool registered(const char *id)
{
	// Unknown output ids have no flags; every registered one has at least
	// one of video, audio, AV or encoded.
	return obs_get_output_flags(id) != 0;
}

} // namespace

// ---------------------------------------------------------------- SlotPanel

SlotPanel::SlotPanel(Slot slot, std::vector<const OutputType *> types, QWidget *parent)
	: QGroupBox(text(slot_title(slot)), parent),
	  output_(slot),
	  types_(std::move(types))
{
	auto *layout = new QVBoxLayout(this);

	auto *form = new QFormLayout();
	typeBox_ = new QComboBox(this);
	typeBox_->addItem(text("Type.None"), QString());
	for (const OutputType *type : types_) {
		typeBox_->addItem(text(type->label), QString::fromUtf8(type->id));
	}
	form->addRow(text("Device"), typeBox_);
	layout->addLayout(form);

	propertiesLayout_ = new QVBoxLayout();
	propertiesLayout_->setContentsMargins(0, 0, 0, 0);
	layout->addLayout(propertiesLayout_);

	hint_ = new QLabel(text("RestartToApply"), this);
	hint_->setWordWrap(true);
	hint_->setVisible(false);
	layout->addWidget(hint_);

	auto *controls = new QHBoxLayout();
	autoStartBox_ = new QCheckBox(text("AutoStart"), this);
	startStop_ = new QPushButton(text("Start"), this);
	startStop_->setCheckable(true);
	controls->addWidget(autoStartBox_);
	controls->addStretch();
	controls->addWidget(startStop_);
	layout->addLayout(controls);

	connect(typeBox_, &QComboBox::currentIndexChanged, this, &SlotPanel::typeSelected);
	connect(autoStartBox_, &QCheckBox::toggled, this, &SlotPanel::autoStartToggled);
	connect(startStop_, &QPushButton::clicked, this, &SlotPanel::startStopClicked);

	QPointer<SlotPanel> self(this);
	output_.setStateCallback([self](bool running, uint64_t generation) {
		if (!self) {
			return;
		}
		if (QThread::currentThread() == self->thread()) {
			self->outputStateChanged(running);
			return;
		}
		// The output stopped by itself, on its own thread.
		QMetaObject::invokeMethod(
			self,
			[self, generation]() {
				if (self && self->output_.generation() == generation && self->output_.running()) {
					self->output_.stop();
				}
			},
			Qt::QueuedConnection);
	});

	load(SlotConfig{});
}

SlotPanel::~SlotPanel()
{
	// Stop before the widgets the state callback touches go away.
	output_.setStateCallback(nullptr);
	output_.stop();
}

const OutputType *SlotPanel::selectedType() const
{
	return find_type(typeBox_->currentData().toString().toStdString());
}

void SlotPanel::load(const SlotConfig &config)
{
	output_.stop();

	config_ = config;
	if (!config_.settings) {
		OBSDataAutoRelease empty = obs_data_create();
		config_.settings = empty.Get();
	}

	int index = typeBox_->findData(QString::fromStdString(config_.type));
	if (index < 0) {
		// Saved with a device plugin that is not loaded now. Keep the
		// config as saved, but show none.
		index = 0;
	}

	const QSignalBlocker blockType(typeBox_);
	const QSignalBlocker blockAutoStart(autoStartBox_);
	typeBox_->setCurrentIndex(index);
	autoStartBox_->setChecked(config_.autoStart);

	rebuildProperties();
	updateState();
}

void SlotPanel::autoStart()
{
	if (config_.autoStart && selectedType() && !output_.running()) {
		startStopClicked();
	}
}

void SlotPanel::stopOutput()
{
	output_.stop();
}

void SlotPanel::typeSelected(int)
{
	output_.stop();

	const OutputType *type = selectedType();
	config_.type = type ? type->id : "";
	// Settings belong to one output type; start the new one from its
	// defaults.
	OBSDataAutoRelease empty = obs_data_create();
	config_.settings = empty.Get();

	rebuildProperties();
	updateState();
	emit configChanged();
}

void SlotPanel::autoStartToggled(bool checked)
{
	config_.autoStart = checked;
	emit configChanged();
}

void SlotPanel::startStopClicked()
{
	if (output_.running()) {
		output_.stop();
		return;
	}

	const OutputType *type = selectedType();
	if (!type) {
		updateState();
		return;
	}

	if (output_.start(*type, config_.settings)) {
		applied_ = copy_data(config_.settings);
	}
	updateState();
}

void SlotPanel::propertiesChanged()
{
	if (output_.running()) {
		if (output_.apply(applied_, config_.settings)) {
			applied_ = copy_data(config_.settings);
		}
	}
	updateState();
	emit configChanged();
}

void SlotPanel::rebuildProperties()
{
	if (properties_) {
		delete properties_;
	}

	const OutputType *type = selectedType();
	if (!type) {
		return;
	}

	properties_ = new OBSPropertiesView(config_.settings, type->id,
					    (PropertiesReloadCallback)obs_get_output_properties, 170);
	propertiesLayout_->addWidget(properties_);
	connect(properties_, &OBSPropertiesView::Changed, this, &SlotPanel::propertiesChanged);
}

void SlotPanel::outputStateChanged(bool)
{
	updateState();
}

void SlotPanel::updateState()
{
	const bool running = output_.running();
	const OutputType *type = selectedType();

	const QSignalBlocker block(startStop_);
	startStop_->setChecked(running);
	startStop_->setText(text(running ? "Stop" : "Start"));
	startStop_->setEnabled(type != nullptr);
	typeBox_->setEnabled(!running);

	// Settings of types that cannot update live reach the device on the
	// next start; say so instead of silently ignoring them.
	bool pending = false;
	if (running && type && !type->live_update && applied_ && config_.settings) {
		const std::string applied = obs_data_get_json(applied_);
		pending = applied != obs_data_get_json(config_.settings);
	}
	hint_->setVisible(pending);
}

// ---------------------------------------------------------------- OutputDock

OutputDock::OutputDock(QWidget *parent) : QWidget(parent)
{
	setObjectName("com.xaymar.knurbs.device-output");

	auto *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);

	auto *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	outer->addWidget(scroll);

	auto *content = new QWidget(scroll);
	auto *layout = new QVBoxLayout(content);
	scroll->setWidget(content);

	for (Slot slot : {Slot::Program, Slot::Preview, Slot::Audio}) {
		std::vector<const OutputType *> types = types_for(slot, registered);
		if (types.empty()) {
			continue;
		}
		auto *panel = new SlotPanel(slot, std::move(types), content);
		connect(panel, &SlotPanel::configChanged, this, &OutputDock::save);
		layout->addWidget(panel);
		panels_.push_back(panel);
	}

	if (panels_.empty()) {
		auto *none = new QLabel(text("NoDevices"), content);
		none->setWordWrap(true);
		layout->addWidget(none);
	}
	layout->addStretch();
}

std::string OutputDock::configPath() const
{
	BPtr<char> profile = obs_frontend_get_current_profile_path();
	if (profile && *profile) {
		return std::string(profile) + "/" + kConfigFileName;
	}

	// No profile folder (should not happen): fall back to the module's own.
	BPtr<char> dir = obs_module_get_config_path(obs_current_module(), "");
	if (!dir) {
		return {};
	}
	os_mkdirs(dir);
	BPtr<char> path = obs_module_get_config_path(obs_current_module(), kConfigFileName);
	return path ? std::string(path) : std::string();
}

void OutputDock::loadProfile()
{
	const Config config = load_config(configPath());
	for (SlotPanel *panel : panels_) {
		panel->load(config.at(panel->slot()));
	}
	for (SlotPanel *panel : panels_) {
		panel->autoStart();
	}
}

void OutputDock::stopAll()
{
	for (SlotPanel *panel : panels_) {
		panel->stopOutput();
	}
	save();
}

void OutputDock::save()
{
	const std::string path = configPath();
	// Keep what is saved for slots this machine cannot show (no device
	// plugin loaded), so moving a profile between machines loses nothing.
	Config config = load_config(path);
	for (SlotPanel *panel : panels_) {
		config.at(panel->slot()) = panel->config();
	}
	if (!save_config(config, path)) {
		blog(LOG_WARNING, "[knurbs-device-output] Could not save '%s'", path.c_str());
	}
}

void OutputDock::handleFrontendEvent(obs_frontend_event event)
{
	for (SlotPanel *panel : panels_) {
		panel->handleFrontendEvent(event);
	}
}

} // namespace knurbs::device_output
