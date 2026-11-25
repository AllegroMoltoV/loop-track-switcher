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

#include "track-engine.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace lts {

// Tracks longer than this are cut here and get a warning
constexpr uint64_t kMaxTrackFrames = 10ull * 60ull * kSampleRate;

enum class LoadError : int {
	None = 0,
	OpenFailed = 1,
	UnsupportedFormat = 2,
	ReadFailed = 3,
	OutOfMemory = 4,
	Cancelled = 5,
};

struct LoadInfo {
	uint32_t source_rate = 0;
	uint32_t source_channels = 0;
	// Length of the whole file at 48 kHz, before cutting at kMaxTrackFrames
	uint64_t full_frames = 0;
	bool trimmed = false;
	bool from_bake = false;
};

struct LoadResult {
	std::shared_ptr<const TrackData> data;
	LoadError error = LoadError::None;
	LoadInfo info;
	// The bake file used or written, empty when the file needed no bake
	std::string bake_path;
};

// Reads a WAV, FLAC, MP3, or Ogg Vorbis file into 16-bit stereo at 48 kHz. The path is UTF-8.
//
// A 48 kHz WAV is read directly. Any other file is decoded and resampled in chunks of a few thousand frames, and the
// result is written to a bake file in bake_dir, named after the source path, size, and modification time. The next
// load reads the bake instead. An empty bake_dir disables baking. Mono is copied to both channels. For three or more
// channels, the front left and front right channels are used. Sources that load the same file share one TrackData.
LoadResult load_track(const std::string &path, const std::string &bake_dir, const std::atomic<bool> &cancel);

// Deletes the bake file unless a loaded track still uses it. Returns true when it was deleted.
bool remove_bake_if_unused(const std::string &bake_path);

// Deletes bake files that were not used for max_age_days, and leftover temporary files.
void clean_old_bakes(const std::string &bake_dir, int max_age_days);

} // namespace lts
