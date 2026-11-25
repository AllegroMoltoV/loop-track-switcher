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

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>
#include <QWidget>

#include "loop-track-source.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

constexpr const char *kDockId = "loop-track-switcher-dock";

// OBS owns the dock widget after obs_frontend_add_dock_by_id succeeds and destroys it on exit
QPointer<QWidget> dock_widget;

void add_dock()
{
	auto *widget = new QWidget();
	auto *layout = new QVBoxLayout(widget);
	layout->addWidget(new QLabel(obs_module_text("LoopTrackSwitcher"), widget));
	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("LoopTrackSwitcher"), widget)) {
		obs_log(LOG_ERROR, "failed to add the dock");
		delete widget;
		return;
	}
	dock_widget = widget;
}

void on_frontend_event(enum obs_frontend_event event, void *)
{
	// The main window exists only after loading finishes. Without the OBS frontend, as in the tests,
	// this event never arrives and no dock is created.
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		add_dock();
}

} // namespace

bool obs_module_load(void)
{
	obs_register_source(&loop_track_source_info);
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	dock_widget = nullptr;
	obs_log(LOG_INFO, "plugin unloaded");
}
