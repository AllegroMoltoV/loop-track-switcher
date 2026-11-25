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
#include <string>

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

// Operates one Loop Track Switcher source chosen from a list. Closing the dock only hides it, and the source keeps
// playing. The dock reads the state of the source every 100 ms through its get_state procedure.
class LoopTrackDock : public QWidget {
	Q_OBJECT

public:
	explicit LoopTrackDock(QWidget *parent = nullptr);
	~LoopTrackDock() override;

public slots:
	void rebuild_sources();

private:
	void refresh();
	void call(const char *procedure);
	void toggle_track(int track, bool enabled);
	obs_source_t *selected_source() const;

	static void on_source_signal(void *data, calldata_t *cd);

	QComboBox *source_combo_ = nullptr;
	QPushButton *play_button_ = nullptr;
	QPushButton *pause_button_ = nullptr;
	QPushButton *stop_button_ = nullptr;
	std::array<QPushButton *, 4> track_buttons_{};
	QLabel *transport_label_ = nullptr;
	QLabel *position_label_ = nullptr;
	QLabel *status_label_ = nullptr;
	QTimer *timer_ = nullptr;
};
