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

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace lts {

constexpr int kTrackCount = 4;
constexpr uint32_t kSampleRate = 48000;
// Track switches and Play / Pause / Stop fade over 200 ms
constexpr uint32_t kFadeFrames = kSampleRate / 5;

// One track held in memory as interleaved 16-bit stereo. 16 bits halve the memory of float samples.
struct TrackData {
	std::vector<int16_t> samples;
	uint64_t frames = 0;
};

enum class Transport : int { Stopped = 0, Playing = 1, Paused = 2 };

enum class LoopStatus : int { Ok = 0, NoTracks = 1, StartNotBeforeEnd = 2, EndBeyondTrack = 3 };

// Plays up to four tracks from one shared position and loops [loop_start, loop_end).
//
// Control and state readout work from any thread. Track data and the loop range are guarded by one mutex, and the
// generator thread holds it while it renders one chunk. A replaced track is handed back to the caller, which frees
// it outside the lock, so the generator never reads freed memory or allocates.
class Engine {
public:
	Engine();

	void play();
	void pause();
	void stop();
	void set_track_enabled(int track, bool enabled);
	bool track_enabled(int track) const;

	// The requested transport. Pause and Stop take effect after the fade out.
	Transport transport() const;
	uint64_t position() const;
	uint64_t loop_start() const;
	uint64_t loop_end() const;
	LoopStatus loop_status() const;

	// Replaces the data of a track (nullptr clears it), validates the loop range against the new set of tracks,
	// and keeps the position inside it. If the range becomes invalid, playback stops.
	// Returns the previous data.
	std::unique_ptr<TrackData> replace_track(int track, std::unique_ptr<TrackData> data);

	// end_frames 0 means the end of the shortest loaded track
	void set_loop(uint64_t start_frames, uint64_t end_frames);

	// Renders the next chunk. Called by the generator thread only.
	void render(float *left, float *right, size_t frames);

private:
	void validate_locked();
	void publish_locked();

	std::mutex mutex_;
	std::array<std::unique_ptr<TrackData>, kTrackCount> tracks_;
	uint64_t requested_start_ = 0;
	uint64_t requested_end_ = 0;
	uint64_t loop_start_ = 0;
	uint64_t loop_end_ = 0;
	LoopStatus loop_status_ = LoopStatus::NoTracks;
	uint64_t position_ = 0;
	// True while the position advances, including the fade out after Pause or Stop
	bool running_ = false;
	float master_gain_ = 0.0f;
	std::array<float, kTrackCount> track_gain_{};

	std::atomic<int> requested_transport_{static_cast<int>(Transport::Stopped)};
	std::array<std::atomic<bool>, kTrackCount> enabled_;

	std::atomic<uint64_t> position_out_{0};
	std::atomic<uint64_t> loop_start_out_{0};
	std::atomic<uint64_t> loop_end_out_{0};
	std::atomic<int> loop_status_out_{static_cast<int>(LoopStatus::NoTracks)};
};

} // namespace lts
