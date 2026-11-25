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

#include <atomic>
#include <string>

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
// Positions and lengths are in frames at 48 kHz.
//   void play()
//   void pause()
//   void stop()
//   void seek(int frames)
//   void set_track(int track, bool enabled)          track is 0 to 3
//   void set_loop(bool enabled)
//   void set_loop_range(int start, int end)          end 0 means the song end
//   void set_crossfade(bool enabled)
//   void get_state(out int transport, out int position, out int song_end, out int loop_start, out int loop_end,
//                  out int requested_start, out int requested_end, out int loop_status, out bool loop_enabled,
//                  out bool crossfade, out int enabled_mask, out int memory_bytes,
//                  and for each track N in 0 to 3: out int stateN, out int errorN, out string nameN,
//                  out int framesN, out int full_framesN, out int channelsN, out int rateN)
// transport is lts::Transport, loop_status is lts::LoopStatus, error is lts::LoadError.

void register_loop_track_source();

// Folder of the bake files, and whether OBS is removing all sources at once (exit or a scene collection change).
// During such a removal, the sources keep their bakes for the next load.
void set_bake_dir(const std::string &dir);
const std::string &bake_dir();
extern std::atomic<bool> bulk_teardown;

// Localized text for a transport, loop status, or load error code
const char *transport_text(int transport);
const char *loop_status_text(int status);
const char *load_error_text(int error);
