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

#pragma once

#include <obs.h>

#include <QWidget>

#include <array>
#include <cstdint>
#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSlider;
class QTimer;
class QToolButton;

// Seek bar: shows the song from 0 to its end, the intro, loop, and outro, the loop markers, and the position.
// A click, or releasing a drag, asks for a seek. While dragging, the handle follows the mouse and position
// updates are ignored.
class SeekBar : public QWidget {
public:
	explicit SeekBar(QWidget *parent = nullptr);
	void set_state(uint64_t song_end, uint64_t loop_start, uint64_t loop_end, uint64_t position, bool valid);
	std::function<void(uint64_t)> on_seek;
	QSize sizeHint() const override;

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;

private:
	uint64_t frames_at(int x) const;
	int x_at(uint64_t frames) const;

	uint64_t song_end_ = 0;
	uint64_t loop_start_ = 0;
	uint64_t loop_end_ = 0;
	uint64_t position_ = 0;
	bool valid_ = false;
	bool dragging_ = false;
	uint64_t drag_position_ = 0;
};

// Operates one Loop Track Switcher source chosen from a list. Closing the dock only hides it, and the source keeps
// playing. The dock reads the state of the source every 100 ms through its get_state procedure.
// The layout follows docs/reference/playback-states.md.
class LoopTrackDock : public QWidget {
	Q_OBJECT

public:
	explicit LoopTrackDock(QWidget *parent = nullptr);
	~LoopTrackDock() override;

public slots:
	void rebuild_sources();

private:
	enum class Unit : int { Seconds = 0, Milliseconds = 1, Samples = 2 };

	void refresh();
	void call(const char *procedure);
	void call_bool(const char *procedure, bool value);
	void toggle_track(int track, bool enabled);
	void apply_loop_range();
	void apply_unit();
	double to_unit(uint64_t frames) const;
	uint64_t from_unit(double value) const;
	void load_settings();
	void save_settings();
	obs_source_t *selected_source() const;

	static void on_source_signal(void *data, calldata_t *cd);

	QComboBox *source_combo_ = nullptr;
	QToolButton *help_button_ = nullptr;
	QPushButton *play_button_ = nullptr;
	QPushButton *pause_button_ = nullptr;
	QPushButton *stop_button_ = nullptr;
	QPushButton *loop_button_ = nullptr;
	SeekBar *seek_bar_ = nullptr;
	QLabel *time_label_ = nullptr;
	QSlider *start_slider_ = nullptr;
	QSlider *end_slider_ = nullptr;
	QDoubleSpinBox *start_spin_ = nullptr;
	QDoubleSpinBox *end_spin_ = nullptr;
	QLabel *start_warning_ = nullptr;
	QLabel *end_warning_ = nullptr;
	std::array<QPushButton *, 4> track_buttons_{};
	std::array<QLabel *, 4> track_warnings_{};
	QComboBox *unit_combo_ = nullptr;
	QCheckBox *crossfade_check_ = nullptr;
	QTimer *timer_ = nullptr;

	Unit unit_ = Unit::Seconds;
	uint64_t song_end_ = 0;
	uint64_t loop_start_ = 0;
	uint64_t loop_end_ = 0;
};
