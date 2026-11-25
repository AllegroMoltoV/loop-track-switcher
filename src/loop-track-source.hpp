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

#include <obs-module.h>

// Source id. Saved in scene collections, so it must not change.
constexpr const char *kLoopTrackSourceId = "loop_track_switcher_source";

// Load state of a track, as returned by the get_state procedure in state0 to state3
enum TrackState : int {
	TRACK_EMPTY = 0,
	TRACK_LOADING = 1,
	TRACK_READY = 2,
	TRACK_ERROR = 3,
};

// The source registers these procedures on its proc handler. The dock and the tests call them.
//   void play()
//   void pause()
//   void stop()
//   void set_track(int track, bool enabled)          track is 0 to 3
//   void get_state(out int transport, out float position, out float loop_start, out float loop_end,
//                  out int loop_status, out int enabled_mask,
//                  out int state0..3, out int error0..3, out string name0..3)
// transport is lts::Transport, loop_status is lts::LoopStatus, error is lts::WavError. Times are in seconds.

void register_loop_track_source();

// Localized text for a transport, loop status, or WAV error code
const char *transport_text(int transport);
const char *loop_status_text(int status);
const char *wav_error_text(int error);
