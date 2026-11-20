#include "captions-dock.hpp"

#include <obs-module.h>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>
#include <vector>

namespace knurbs::captions {

namespace {

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

} // namespace

CaptionsDock::CaptionsDock(Captions &captions, QWidget *parent) : QWidget(parent), captions_(captions)
{
	setObjectName("com.xaymar.knurbs.captions");

	auto *layout = new QVBoxLayout(this);
	auto *form = new QFormLayout();

	source_ = new QComboBox(this);
	form->addRow(text("Source"), source_);
	layout->addLayout(form);

	stream_ = new QCheckBox(text("ToStream"), this);
	recording_ = new QCheckBox(text("ToRecording"), this);
	layout->addWidget(stream_);
	layout->addWidget(recording_);

	auto *hint = new QLabel(text("Hint"), this);
	hint->setWordWrap(true);
	layout->addWidget(hint);
	layout->addStretch();

	connect(source_, &QComboBox::currentIndexChanged, this, &CaptionsDock::changed);
	connect(stream_, &QCheckBox::toggled, this, &CaptionsDock::changed);
	connect(recording_, &QCheckBox::toggled, this, &CaptionsDock::changed);

	refresh();
}

void CaptionsDock::refresh()
{
	const CaptionsConfig &config = captions_.config();

	// Sources that carry CEA-708 captions, by name, remembered by UUID.
	std::vector<std::pair<QString, QString>> sources;
	auto add = [](void *param, obs_source_t *source) {
		if (obs_source_get_output_flags(source) & OBS_SOURCE_CEA_708) {
			static_cast<std::vector<std::pair<QString, QString>> *>(param)->emplace_back(
				QString::fromUtf8(obs_source_get_name(source)),
				QString::fromUtf8(obs_source_get_uuid(source)));
		}
		return true;
	};
	obs_enum_sources(add, &sources);
	std::sort(sources.begin(), sources.end());

	const QSignalBlocker blockSource(source_);
	const QSignalBlocker blockStream(stream_);
	const QSignalBlocker blockRecording(recording_);

	source_->clear();
	source_->addItem(text("None"), QString());
	for (const auto &[name, uuid] : sources) {
		source_->addItem(name, uuid);
	}
	const int index = source_->findData(QString::fromStdString(config.sourceUuid));
	source_->setCurrentIndex(index < 0 ? 0 : index);

	stream_->setChecked(config.stream);
	recording_->setChecked(config.recording);
}

void CaptionsDock::changed()
{
	CaptionsConfig config = captions_.config();
	config.sourceUuid = source_->currentData().toString().toStdString();
	config.stream = stream_->isChecked();
	config.recording = recording_->isChecked();
	captions_.apply(config);
}

} // namespace knurbs::captions
