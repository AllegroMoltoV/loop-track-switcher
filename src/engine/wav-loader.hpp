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

#include <memory>
#include <string>

namespace lts {

enum class WavError : int {
	None = 0,
	OpenFailed = 1,
	NotStereo = 2,
	Not48k = 3,
	UnsupportedFormat = 4,
	ReadFailed = 5,
	OutOfMemory = 6,
};

struct WavLoadResult {
	std::unique_ptr<TrackData> data;
	WavError error = WavError::None;
};

// Reads a whole WAV file into 16-bit stereo. Accepts 48 kHz stereo integer PCM (16, 24, or 32 bits) and 32-bit
// float. Wider samples are converted to 16 bits while reading, without a float copy of the whole file.
// The path is UTF-8.
WavLoadResult load_wav(const std::string &path);

} // namespace lts
