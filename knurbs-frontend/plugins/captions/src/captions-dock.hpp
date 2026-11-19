#pragma once

#include "captions.hpp"

#include <QWidget>

class QCheckBox;
class QComboBox;

namespace knurbs::captions {

// The "Captions" dock: which source's captions to pass on, and where to.
class CaptionsDock final : public QWidget {
	Q_OBJECT

public:
	CaptionsDock(Captions &captions, QWidget *parent = nullptr);

	// Shows the current config and lists the sources that carry captions.
	void refresh();

private:
	void changed();

	Captions &captions_;
	QComboBox *source_ = nullptr;
	QCheckBox *stream_ = nullptr;
	QCheckBox *recording_ = nullptr;
};

} // namespace knurbs::captions
