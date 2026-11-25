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

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMetaObject>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <cstring>

namespace {

constexpr const char *kSourceSignals[] = {"source_create", "source_remove", "source_destroy", "source_rename"};

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString seconds_text(double seconds)
{
	return QString::number(seconds, 'f', 1);
}

} // namespace

LoopTrackDock::LoopTrackDock(QWidget *parent) : QWidget(parent)
{
	auto *layout = new QVBoxLayout(this);

	source_combo_ = new QComboBox(this);
	source_combo_->setObjectName("source_combo");
	layout->addWidget(source_combo_);

	auto *transport_row = new QHBoxLayout();
	play_button_ = new QPushButton(text("Play"), this);
	pause_button_ = new QPushButton(text("Pause"), this);
	stop_button_ = new QPushButton(text("Stop"), this);
	play_button_->setObjectName("play_button");
	pause_button_->setObjectName("pause_button");
	stop_button_->setObjectName("stop_button");
	transport_row->addWidget(play_button_);
	transport_row->addWidget(pause_button_);
	transport_row->addWidget(stop_button_);
	layout->addLayout(transport_row);

	auto *track_grid = new QGridLayout();
	for (int t = 0; t < static_cast<int>(track_buttons_.size()); t++) {
		auto *button = new QPushButton(this);
		button->setCheckable(true);
		button->setObjectName(QString("track_button_%1").arg(t + 1));
		track_grid->addWidget(button, t / 2, t % 2);
		track_buttons_[t] = button;
		connect(button, &QPushButton::clicked, this, [this, t](bool checked) { toggle_track(t, checked); });
	}
	layout->addLayout(track_grid);

	transport_label_ = new QLabel(this);
	position_label_ = new QLabel(this);
	status_label_ = new QLabel(this);
	status_label_->setWordWrap(true);
	transport_label_->setObjectName("transport_label");
	position_label_->setObjectName("position_label");
	status_label_->setObjectName("status_label");
	layout->addWidget(transport_label_);
	layout->addWidget(position_label_);
	layout->addWidget(status_label_);
	layout->addStretch();

	connect(play_button_, &QPushButton::clicked, this, [this] { call("play"); });
	connect(pause_button_, &QPushButton::clicked, this, [this] { call("pause"); });
	connect(stop_button_, &QPushButton::clicked, this, [this] { call("stop"); });
	connect(source_combo_, &QComboBox::currentIndexChanged, this, [this] { refresh(); });

	signal_handler_t *handler = obs_get_signal_handler();
	for (const char *name : kSourceSignals)
		signal_handler_connect(handler, name, on_source_signal, this);

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

void LoopTrackDock::refresh()
{
	obs_source_t *source = selected_source();
	const bool available = source != nullptr;
	play_button_->setEnabled(available);
	pause_button_->setEnabled(available);
	stop_button_->setEnabled(available);
	for (auto *button : track_buttons_)
		button->setEnabled(available);

	if (!source) {
		for (int t = 0; t < static_cast<int>(track_buttons_.size()); t++) {
			track_buttons_[t]->setChecked(false);
			track_buttons_[t]->setText(QString("%1 %2").arg(text("Track")).arg(t + 1));
		}
		transport_label_->clear();
		position_label_->clear();
		status_label_->clear();
		return;
	}

	calldata_t cd = {};
	proc_handler_call(obs_source_get_proc_handler(source), "get_state", &cd);
	obs_source_release(source);

	const int transport = static_cast<int>(calldata_int(&cd, "transport"));
	const int loop_status = static_cast<int>(calldata_int(&cd, "loop_status"));
	const long long mask = calldata_int(&cd, "enabled_mask");

	QStringList errors;
	for (int t = 0; t < static_cast<int>(track_buttons_.size()); t++) {
		const std::string index = std::to_string(t);
		const int state = static_cast<int>(calldata_int(&cd, ("state" + index).c_str()));
		const char *name = calldata_string(&cd, ("name" + index).c_str());
		QString label = QString("%1 %2").arg(text("Track")).arg(t + 1);
		if (state != TRACK_EMPTY && name && *name)
			label += QString(": %1").arg(QString::fromUtf8(name));
		if (state == TRACK_LOADING)
			label += QString(" (%1)").arg(text("Status.Loading"));
		track_buttons_[t]->setText(label);
		track_buttons_[t]->setChecked((mask >> t) & 1);
		if (state == TRACK_ERROR) {
			const int error = static_cast<int>(calldata_int(&cd, ("error" + index).c_str()));
			errors.append(QString("%1 %2: %3")
					      .arg(text("Track"))
					      .arg(t + 1)
					      .arg(QString::fromUtf8(wav_error_text(error))));
		}
	}

	transport_label_->setText(QString::fromUtf8(transport_text(transport)));
	if (loop_status == 0) {
		position_label_->setText(QString("%1 s  (%2: %3 - %4 s)")
						 .arg(seconds_text(calldata_float(&cd, "position")))
						 .arg(text("Loop"))
						 .arg(seconds_text(calldata_float(&cd, "loop_start")))
						 .arg(seconds_text(calldata_float(&cd, "loop_end"))));
	} else {
		position_label_->clear();
		errors.append(QString::fromUtf8(loop_status_text(loop_status)));
	}
	status_label_->setText(errors.join("\n"));
	calldata_free(&cd);
}
