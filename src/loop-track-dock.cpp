/*
Loop Track Switcher
Copyright (C) 2026 AllegroMoltoV

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "loop-track-dock.hpp"

#include "loop-track-source.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace {

constexpr const char *kSourceSignals[] = {"source_create", "source_remove", "source_destroy", "source_rename"};
constexpr double kRate = 48000.0;

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

// m:ss.s
QString time_text(uint64_t frames)
{
	const double seconds = static_cast<double>(frames) / kRate;
	const int minutes = static_cast<int>(seconds / 60.0);
	return QString("%1:%2").arg(minutes).arg(seconds - minutes * 60.0, 4, 'f', 1, QChar('0'));
}

QLabel *make_warning(QWidget *parent, const QString &name)
{
	auto *label = new QLabel("[!]", parent);
	label->setObjectName(name);
	label->setVisible(false);
	return label;
}

void set_warning(QLabel *label, const QStringList &reasons)
{
	label->setVisible(!reasons.isEmpty());
	label->setToolTip(reasons.join("\n"));
}

} // namespace

// SeekBar

SeekBar::SeekBar(QWidget *parent) : QWidget(parent)
{
	setObjectName("seek_bar");
	setMinimumHeight(22);
	setCursor(Qt::PointingHandCursor);
}

QSize SeekBar::sizeHint() const
{
	return QSize(240, 22);
}

void SeekBar::set_state(uint64_t song_end, uint64_t loop_start, uint64_t loop_end, uint64_t position, bool valid)
{
	song_end_ = song_end;
	loop_start_ = loop_start;
	loop_end_ = loop_end;
	position_ = position;
	valid_ = valid;
	update();
}

uint64_t SeekBar::frames_at(int x) const
{
	if (song_end_ == 0 || width() <= 1)
		return 0;
	const double ratio = std::clamp(static_cast<double>(x) / static_cast<double>(width() - 1), 0.0, 1.0);
	return std::min<uint64_t>(static_cast<uint64_t>(ratio * static_cast<double>(song_end_)), song_end_ - 1);
}

int SeekBar::x_at(uint64_t frames) const
{
	if (song_end_ == 0)
		return 0;
	return static_cast<int>(static_cast<double>(frames) / static_cast<double>(song_end_) * (width() - 1));
}

void SeekBar::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	const QPalette &pal = palette();
	const int bar_top = height() / 2 - 4;
	const QRect bar(0, bar_top, width(), 8);

	// Intro and outro use a quiet color, the loop the highlight color of the OBS theme
	painter.fillRect(bar, pal.color(isEnabled() ? QPalette::Mid : QPalette::Dark));
	if (valid_ && song_end_ > 0) {
		QColor loop = pal.color(QPalette::Highlight);
		loop.setAlpha(170);
		const int x0 = x_at(loop_start_);
		const int x1 = x_at(loop_end_);
		painter.fillRect(QRect(x0, bar_top, std::max(1, x1 - x0), 8), loop);
		painter.setPen(QPen(pal.color(QPalette::HighlightedText), 2));
		painter.drawLine(x0, bar_top - 4, x0, bar_top + 12);
		painter.drawLine(x1, bar_top - 4, x1, bar_top + 12);
	}
	if (song_end_ > 0) {
		const int x = x_at(dragging_ ? drag_position_ : position_);
		painter.setPen(Qt::NoPen);
		painter.setBrush(pal.color(QPalette::ButtonText));
		painter.drawEllipse(QPoint(x, height() / 2), 6, 6);
	}
}

void SeekBar::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton || song_end_ == 0)
		return;
	dragging_ = true;
	drag_position_ = frames_at(static_cast<int>(event->position().x()));
	update();
}

void SeekBar::mouseMoveEvent(QMouseEvent *event)
{
	if (!dragging_)
		return;
	drag_position_ = frames_at(static_cast<int>(event->position().x()));
	update();
}

void SeekBar::mouseReleaseEvent(QMouseEvent *event)
{
	if (!dragging_ || event->button() != Qt::LeftButton)
		return;
	dragging_ = false;
	drag_position_ = frames_at(static_cast<int>(event->position().x()));
	position_ = drag_position_;
	update();
	if (on_seek)
		on_seek(drag_position_);
}

// LoopTrackDock

LoopTrackDock::LoopTrackDock(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);

	auto *top_row = new QHBoxLayout();
	source_combo_ = new QComboBox(this);
	source_combo_->setObjectName("source_combo");
	help_button_ = new QToolButton(this);
	help_button_->setObjectName("help_button");
	help_button_->setText("?");
	help_button_->setToolTip(text("Help"));
	top_row->addWidget(source_combo_, 1);
	top_row->addWidget(help_button_);
	layout->addLayout(top_row);

	// Two rows of two buttons keep the dock narrow. The button of the current state is checked, which the OBS
	// theme draws with its highlight, so no state text is needed.
	auto *transport = new QGridLayout();
	play_button_ = new QPushButton(text("Play"), this);
	pause_button_ = new QPushButton(text("Pause"), this);
	stop_button_ = new QPushButton(text("Stop"), this);
	loop_button_ = new QPushButton(text("Loop"), this);
	play_button_->setObjectName("play_button");
	pause_button_->setObjectName("pause_button");
	stop_button_->setObjectName("stop_button");
	loop_button_->setObjectName("loop_button");
	for (auto *button : {play_button_, pause_button_, stop_button_, loop_button_})
		button->setCheckable(true);
	transport->addWidget(play_button_, 0, 0);
	transport->addWidget(pause_button_, 0, 1);
	transport->addWidget(stop_button_, 1, 0);
	transport->addWidget(loop_button_, 1, 1);
	layout->addLayout(transport);

	seek_bar_ = new SeekBar(this);
	layout->addWidget(seek_bar_);
	time_label_ = new QLabel(this);
	time_label_->setObjectName("time_label");
	layout->addWidget(time_label_);

	auto *loop_grid = new QGridLayout();
	auto make_row = [this, loop_grid](int row, const char *label, const QString &name, QSlider *&slider,
					  QDoubleSpinBox *&spin, QLabel *&warning) {
		loop_grid->addWidget(new QLabel(text(label), this), row, 0);
		slider = new QSlider(Qt::Horizontal, this);
		slider->setObjectName(name + "_slider");
		spin = new QDoubleSpinBox(this);
		spin->setObjectName(name + "_spin");
		spin->setKeyboardTracking(false);
		warning = make_warning(this, name + "_warning");
		loop_grid->addWidget(slider, row, 1);
		loop_grid->addWidget(spin, row, 2);
		loop_grid->addWidget(warning, row, 3);
	};
	make_row(0, "LoopStart", "loop_start", start_slider_, start_spin_, start_warning_);
	make_row(1, "LoopEnd", "loop_end", end_slider_, end_spin_, end_warning_);
	loop_grid->setColumnStretch(1, 1);
	layout->addLayout(loop_grid);

	auto *tracks = new QGridLayout();
	for (int t = 0; t < static_cast<int>(track_buttons_.size()); t++) {
		auto *button = new QPushButton(this);
		button->setCheckable(true);
		button->setObjectName(QString("track_button_%1").arg(t + 1));
		track_buttons_[t] = button;
		track_warnings_[t] = make_warning(this, QString("track_warning_%1").arg(t + 1));
		tracks->addWidget(button, t, 0);
		tracks->addWidget(track_warnings_[t], t, 1);
		connect(button, &QPushButton::clicked, this, [this, t](bool checked) { toggle_track(t, checked); });
	}
	tracks->setColumnStretch(0, 1);
	layout->addLayout(tracks);

	auto *unit_row = new QHBoxLayout();
	unit_row->addWidget(new QLabel(text("Unit"), this));
	unit_combo_ = new QComboBox(this);
	unit_combo_->setObjectName("unit_combo");
	unit_combo_->addItem(text("Unit.Seconds"), static_cast<int>(Unit::Seconds));
	unit_combo_->addItem(text("Unit.Milliseconds"), static_cast<int>(Unit::Milliseconds));
	unit_combo_->addItem(text("Unit.Samples"), static_cast<int>(Unit::Samples));
	unit_row->addWidget(unit_combo_, 1);
	layout->addLayout(unit_row);
	crossfade_check_ = new QCheckBox(text("Crossfade"), this);
	crossfade_check_->setObjectName("crossfade_check");
	layout->addWidget(crossfade_check_);
	layout->addStretch();

	connect(help_button_, &QToolButton::clicked, this, [this] {
		QToolTip::showText(help_button_->mapToGlobal(help_button_->rect().bottomLeft()), text("Help"),
				   help_button_);
	});
	connect(play_button_, &QPushButton::clicked, this, [this] { call("play"); });
	connect(pause_button_, &QPushButton::clicked, this, [this] { call("pause"); });
	connect(stop_button_, &QPushButton::clicked, this, [this] { call("stop"); });
	connect(loop_button_, &QPushButton::clicked, this, [this](bool checked) { call_bool("set_loop", checked); });
	connect(crossfade_check_, &QCheckBox::clicked, this,
		[this](bool checked) { call_bool("set_crossfade", checked); });
	connect(source_combo_, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
	connect(unit_combo_, &QComboBox::currentIndexChanged, this, [this] {
		unit_ = static_cast<Unit>(unit_combo_->currentData().toInt());
		apply_unit();
		save_settings();
		refresh();
	});
	seek_bar_->on_seek = [this](uint64_t frames) {
		obs_source_t *source = selected_source();
		if (!source)
			return;
		calldata_t cd = {};
		calldata_set_int(&cd, "frames", static_cast<long long>(frames));
		proc_handler_call(obs_source_get_proc_handler(source), "seek", &cd);
		calldata_free(&cd);
		obs_source_release(source);
	};

	// The slider shows its value in the number field while dragging and applies it when released. A click on the
	// groove or a key press changes the value without a drag and applies it at once. The number field applies
	// its value when editing finishes (Enter or losing focus).
	for (auto *slider : {start_slider_, end_slider_}) {
		auto *spin = slider == start_slider_ ? start_spin_ : end_spin_;
		connect(slider, &QSlider::valueChanged, this, [this, slider, spin](int value) {
			spin->blockSignals(true);
			spin->setValue(to_unit(static_cast<uint64_t>(value)));
			spin->blockSignals(false);
			if (!slider->isSliderDown())
				apply_loop_range();
		});
		connect(slider, &QSlider::sliderReleased, this, [this] { apply_loop_range(); });
		connect(spin, &QDoubleSpinBox::editingFinished, this, [this, slider, spin] {
			slider->blockSignals(true);
			slider->setValue(static_cast<int>(from_unit(spin->value())));
			slider->blockSignals(false);
			apply_loop_range();
		});
	}

	signal_handler_t *handler = obs_get_signal_handler();
	for (const char *name : kSourceSignals)
		signal_handler_connect(handler, name, on_source_signal, this);

	load_settings();
	apply_unit();

	timer_ = new QTimer(this);
	connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
	timer_->start(100);

	rebuild_sources();
}

LoopTrackDock::~LoopTrackDock()
{
	signal_handler_t *handler = obs_get_signal_handler();
	if (handler) {
		for (const char *name : kSourceSignals)
			signal_handler_disconnect(handler, name, on_source_signal, this);
	}
}

// The global source signals come from other threads. Only a queued call to the UI thread is made here.
void LoopTrackDock::on_source_signal(void *data, calldata_t *)
{
	QMetaObject::invokeMethod(static_cast<LoopTrackDock *>(data), "rebuild_sources", Qt::QueuedConnection);
}

void LoopTrackDock::load_settings()
{
	char *path = obs_module_config_path("dock.json");
	if (!path)
		return;
	obs_data_t *data = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);
	if (!data)
		return;
	const long long unit = obs_data_get_int(data, "unit");
	obs_data_release(data);
	if (unit >= 0 && unit <= 2) {
		unit_ = static_cast<Unit>(unit);
		unit_combo_->blockSignals(true);
		unit_combo_->setCurrentIndex(static_cast<int>(unit));
		unit_combo_->blockSignals(false);
	}
}

void LoopTrackDock::save_settings()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path("dock.json");
	if (!path)
		return;
	obs_data_t *data = obs_data_create();
	obs_data_set_int(data, "unit", static_cast<int>(unit_));
	obs_data_save_json_safe(data, path, "tmp", "bak");
	obs_data_release(data);
	bfree(path);
}

double LoopTrackDock::to_unit(uint64_t frames) const
{
	switch (unit_) {
	case Unit::Milliseconds:
		return static_cast<double>(frames) / 48.0;
	case Unit::Samples:
		return static_cast<double>(frames);
	default:
		return static_cast<double>(frames) / kRate;
	}
}

uint64_t LoopTrackDock::from_unit(double value) const
{
	double frames = value;
	if (unit_ == Unit::Seconds)
		frames = value * kRate;
	else if (unit_ == Unit::Milliseconds)
		frames = value * 48.0;
	return frames > 0.0 ? static_cast<uint64_t>(std::llround(frames)) : 0;
}

void LoopTrackDock::apply_unit()
{
	for (auto *spin : {start_spin_, end_spin_}) {
		spin->blockSignals(true);
		switch (unit_) {
		case Unit::Milliseconds:
			spin->setDecimals(0);
			spin->setSingleStep(1.0);
			spin->setSuffix(" ms");
			break;
		case Unit::Samples:
			spin->setDecimals(0);
			spin->setSingleStep(1.0);
			spin->setSuffix("");
			break;
		default:
			// Five decimals reach every sample at 48 kHz after rounding
			spin->setDecimals(5);
			spin->setSingleStep(0.001);
			spin->setSuffix(" s");
			break;
		}
		spin->blockSignals(false);
	}
}

void LoopTrackDock::rebuild_sources()
{
	const QString current = source_combo_->currentData().toString();

	QStringList names;
	QStringList uuids;
	struct Lists {
		QStringList *names;
		QStringList *uuids;
	} lists{&names, &uuids};
	obs_enum_sources(
		[](void *param, obs_source_t *source) {
			auto *l = static_cast<Lists *>(param);
			if (obs_source_removed(source))
				return true;
			if (strcmp(obs_source_get_unversioned_id(source), kLoopTrackSourceId) != 0)
				return true;
			l->names->append(QString::fromUtf8(obs_source_get_name(source)));
			l->uuids->append(QString::fromUtf8(obs_source_get_uuid(source)));
			return true;
		},
		&lists);

	source_combo_->blockSignals(true);
	source_combo_->clear();
	for (int i = 0; i < names.size(); i++)
		source_combo_->addItem(names[i], uuids[i]);
	if (names.isEmpty())
		source_combo_->addItem(text("NoSource"), QString());
	const int index = source_combo_->findData(current);
	source_combo_->setCurrentIndex(index >= 0 ? index : 0);
	source_combo_->blockSignals(false);
	refresh();
}

obs_source_t *LoopTrackDock::selected_source() const
{
	const QString uuid = source_combo_->currentData().toString();
	if (uuid.isEmpty())
		return nullptr;
	return obs_get_source_by_uuid(uuid.toUtf8().constData());
}

void LoopTrackDock::call(const char *procedure)
{
	obs_source_t *source = selected_source();
	if (!source)
		return;
	calldata_t cd = {};
	proc_handler_call(obs_source_get_proc_handler(source), procedure, &cd);
	calldata_free(&cd);
	obs_source_release(source);
	refresh();
}

void LoopTrackDock::call_bool(const char *procedure, bool value)
{
	obs_source_t *source = selected_source();
	if (!source)
		return;
	calldata_t cd = {};
	calldata_set_bool(&cd, "enabled", value);
	proc_handler_call(obs_source_get_proc_handler(source), procedure, &cd);
	calldata_free(&cd);
	obs_source_release(source);
	refresh();
}

void LoopTrackDock::toggle_track(int track, bool enabled)
{
	obs_source_t *source = selected_source();
	if (!source)
		return;
	calldata_t cd = {};
	calldata_set_int(&cd, "track", track);
	calldata_set_bool(&cd, "enabled", enabled);
	proc_handler_call(obs_source_get_proc_handler(source), "set_track", &cd);
	calldata_free(&cd);
	obs_source_release(source);
	refresh();
}

void LoopTrackDock::apply_loop_range()
{
	obs_source_t *source = selected_source();
	if (!source || song_end_ == 0) {
		obs_source_release(source);
		return;
	}
	const uint64_t start = static_cast<uint64_t>(start_slider_->value());
	const uint64_t end = static_cast<uint64_t>(end_slider_->value());
	calldata_t cd = {};
	calldata_set_int(&cd, "start", static_cast<long long>(start));
	// The song end is stored as 0, so that the loop keeps ending at the song end when tracks change
	calldata_set_int(&cd, "end", end >= song_end_ ? 0 : static_cast<long long>(end));
	proc_handler_call(obs_source_get_proc_handler(source), "set_loop_range", &cd);
	calldata_free(&cd);
	obs_source_release(source);
	refresh();
}

void LoopTrackDock::refresh()
{
	obs_source_t *source = selected_source();
	const bool available = source != nullptr;
	if (!source) {
		for (QWidget *w : std::initializer_list<QWidget *>{play_button_, pause_button_, stop_button_,
								   loop_button_, start_slider_, end_slider_,
								   start_spin_, end_spin_, crossfade_check_})
			w->setEnabled(false);
		for (int t = 0; t < static_cast<int>(track_buttons_.size()); t++) {
			track_buttons_[t]->setEnabled(false);
			track_buttons_[t]->setChecked(false);
			track_buttons_[t]->setText(QString("%1 %2").arg(text("Track")).arg(t + 1));
			set_warning(track_warnings_[t], {});
		}
		set_warning(start_warning_, {});
		set_warning(end_warning_, {});
		seek_bar_->set_state(0, 0, 0, 0, false);
		time_label_->clear();
		return;
	}

	calldata_t cd = {};
	proc_handler_call(obs_source_get_proc_handler(source), "get_state", &cd);
	obs_source_release(source);

	auto get = [&cd](const char *name) {
		return static_cast<uint64_t>(std::max(0LL, calldata_int(&cd, name)));
	};
	const int transport = static_cast<int>(calldata_int(&cd, "transport"));
	const int loop_status = static_cast<int>(calldata_int(&cd, "loop_status"));
	const uint64_t position = get("position");
	song_end_ = get("song_end");
	loop_start_ = get("requested_start");
	const uint64_t requested_end = get("requested_end");
	loop_end_ = requested_end == 0 ? song_end_ : requested_end;
	const bool has_tracks = loop_status != 1;
	const long long mask = calldata_int(&cd, "enabled_mask");

	auto set_checked = [](QAbstractButton *button, bool checked) {
		button->blockSignals(true);
		button->setChecked(checked);
		button->blockSignals(false);
	};
	set_checked(play_button_, transport == 1);
	set_checked(pause_button_, transport == 2);
	set_checked(stop_button_, transport == 0);
	set_checked(loop_button_, calldata_bool(&cd, "loop_enabled"));
	set_checked(crossfade_check_, calldata_bool(&cd, "crossfade"));
	play_button_->setEnabled(available && has_tracks);
	pause_button_->setEnabled(available && has_tracks);
	stop_button_->setEnabled(available);
	loop_button_->setEnabled(available);
	crossfade_check_->setEnabled(available);

	seek_bar_->setEnabled(has_tracks);
	seek_bar_->set_state(song_end_, get("loop_start"), get("loop_end"), position, loop_status == 0);
	time_label_->setText(QString("%1 / %2").arg(time_text(position), time_text(song_end_)));

	// The two loop rows limit each other, so that the start can never reach the end
	const bool editing = start_slider_->isSliderDown() || end_slider_->isSliderDown() || start_spin_->hasFocus() ||
			     end_spin_->hasFocus();
	for (QWidget *w : std::initializer_list<QWidget *>{start_slider_, end_slider_, start_spin_, end_spin_})
		w->setEnabled(has_tracks);
	if (!editing && has_tracks) {
		const uint64_t end_shown = std::min(loop_end_, song_end_);
		const uint64_t start_max = end_shown > 0 ? end_shown - 1 : 0;
		const uint64_t end_min = std::min(loop_start_ + 1, song_end_);
		auto set_row = [this](QSlider *slider, QDoubleSpinBox *spin, uint64_t min, uint64_t max,
				      uint64_t value) {
			slider->blockSignals(true);
			slider->setRange(static_cast<int>(min), static_cast<int>(max));
			slider->setValue(static_cast<int>(std::clamp(value, min, max)));
			slider->blockSignals(false);
			spin->blockSignals(true);
			spin->setRange(to_unit(min), to_unit(max));
			spin->setValue(to_unit(std::clamp(value, min, max)));
			spin->blockSignals(false);
		};
		set_row(start_slider_, start_spin_, 0, start_max, loop_start_);
		set_row(end_slider_, end_spin_, end_min, song_end_, end_shown);
	}

	QStringList start_reasons;
	QStringList end_reasons;
	if (loop_status == 2) {
		start_reasons << QString::fromUtf8(loop_status_text(loop_status));
		end_reasons << QString::fromUtf8(loop_status_text(loop_status));
	} else if (loop_status == 3) {
		end_reasons << text("Loop.EndBeyondSong.Detail").arg(time_text(loop_end_), time_text(song_end_));
	}
	set_warning(start_warning_, start_reasons);
	set_warning(end_warning_, end_reasons);

	for (int t = 0; t < static_cast<int>(track_buttons_.size()); t++) {
		const std::string i = std::to_string(t);
		const int state = static_cast<int>(calldata_int(&cd, ("state" + i).c_str()));
		const char *name = calldata_string(&cd, ("name" + i).c_str());
		const uint64_t frames = get(("frames" + i).c_str());
		const uint64_t full = get(("full_frames" + i).c_str());
		const long long channels = calldata_int(&cd, ("channels" + i).c_str());

		QString label = QString("%1 %2").arg(text("Track")).arg(t + 1);
		if (state != TRACK_EMPTY && name && *name)
			label = QString("%1: %2").arg(t + 1).arg(QString::fromUtf8(name));
		if (state == TRACK_LOADING)
			label += QString(" (%1)").arg(text("Status.Loading"));
		track_buttons_[t]->setText(label);
		track_buttons_[t]->setEnabled(available);
		set_checked(track_buttons_[t], (mask >> t) & 1);

		QStringList reasons;
		if (state == TRACK_ERROR) {
			reasons << QString::fromUtf8(
				load_error_text(static_cast<int>(calldata_int(&cd, ("error" + i).c_str()))));
		} else if (state == TRACK_READY) {
			if (full > frames)
				reasons << text("Warning.Trimmed").arg(time_text(frames), time_text(full));
			if (channels > 2)
				reasons << text("Warning.Channels").arg(channels);
			if (frames < song_end_)
				reasons << text("Warning.Shorter").arg(time_text(frames), time_text(song_end_));
		}
		set_warning(track_warnings_[t], reasons);
	}
	calldata_free(&cd);
}
