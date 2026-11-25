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
// A seek while sound is playing fades out and in over 10 ms each
constexpr uint32_t kSeekFadeFrames = kSampleRate / 100;
// The crossfade at the loop seam is 10 ms long
constexpr uint32_t kSeamFrames = kSampleRate / 100;

// One track held in memory as interleaved 16-bit stereo. 16 bits halve the memory of float samples.
// Tracks are shared between sources that use the same file, so the data never changes after loading.
struct TrackData {
	std::vector<int16_t> samples;
	uint64_t frames = 0;
};

enum class Transport : int { Stopped = 0, Playing = 1, Paused = 2 };

// Errors that stop playback. Warnings (tracks of different lengths, load failures) are judged by the dock.
enum class LoopStatus : int { Ok = 0, NoTracks = 1, StartNotBeforeEnd = 2, EndBeyondSong = 3 };

// Plays up to four tracks from one shared position, with an intro, a loop range, and an outro.
// The behavior follows docs/reference/playback-states.md.
//
// Control and state readout work from any thread. Track data and the loop range are guarded by one mutex, and the
// generator thread holds it while it renders one chunk. A replaced track is handed back to the caller, which
// releases it outside the lock, so the generator never frees memory.
class Engine {
public:
	Engine();

	void play();
	void pause();
	void stop();
	// Position in frames. Clamped to [0, song end).
	void seek(uint64_t frames);
	void set_track_enabled(int track, bool enabled);
	bool track_enabled(int track) const;
	void set_loop_enabled(bool enabled);
	bool loop_enabled() const;
	void set_crossfade_enabled(bool enabled);
	bool crossfade_enabled() const;

	// The requested transport. Pause and Stop take effect after the fade out.
	Transport transport() const;
	uint64_t position() const;
	// End of the longest loaded track, 0 without tracks
	uint64_t song_end() const;
	// Effective loop range, 0 and 0 while the loop status is not Ok
	uint64_t loop_start() const;
	uint64_t loop_end() const;
	// Loop range as requested. requested_loop_end 0 means the song end.
	uint64_t requested_loop_start() const;
	uint64_t requested_loop_end() const;
	LoopStatus loop_status() const;

	// Replaces the data of a track (nullptr clears it) and validates the loop range against the new set of tracks.
	// Returns the previous data.
	std::shared_ptr<const TrackData> replace_track(int track, std::shared_ptr<const TrackData> data);

	// end_frames 0 means the song end
	void set_loop(uint64_t start_frames, uint64_t end_frames);

	// Renders the next chunk. Called by the generator thread only.
	void render(float *left, float *right, size_t frames);

private:
	void validate_locked(bool loop_changed);
	void stop_now_locked();
	void publish_locked();
	float sample_left(int track, uint64_t position) const;
	float sample_right(int track, uint64_t position) const;

	std::mutex mutex_;
	std::array<std::shared_ptr<const TrackData>, kTrackCount> tracks_;
	uint64_t requested_start_ = 0;
	uint64_t requested_end_ = 0;
	uint64_t song_end_ = 0;
	uint64_t loop_start_ = 0;
	uint64_t loop_end_ = 0;
	LoopStatus loop_status_ = LoopStatus::NoTracks;
	uint64_t position_ = 0;
	// True while sound is produced, including the fade out after Pause or Stop
	bool running_ = false;
	float master_gain_ = 0.0f;
	std::array<float, kTrackCount> track_gain_{};
	// Seek fade: 0 none, 1 fading out before the jump, 2 fading in after it
	int seek_phase_ = 0;
	float seek_gain_ = 1.0f;
	uint64_t seek_target_ = 0;
	uint32_t seen_stop_count_ = 0;
	uint32_t seen_seek_count_ = 0;

	std::atomic<int> requested_transport_{static_cast<int>(Transport::Stopped)};
	// Stop is an event, so that Stop while stopped can still return the position to 0
	std::atomic<uint32_t> stop_count_{0};
	std::atomic<uint32_t> seek_count_{0};
	std::atomic<uint64_t> seek_request_{0};
	std::array<std::atomic<bool>, kTrackCount> enabled_;
	std::atomic<bool> loop_enabled_{true};
	std::atomic<bool> crossfade_enabled_{true};

	std::atomic<uint64_t> position_out_{0};
	std::atomic<uint64_t> song_end_out_{0};
	std::atomic<uint64_t> loop_start_out_{0};
	std::atomic<uint64_t> loop_end_out_{0};
	std::atomic<uint64_t> requested_start_out_{0};
	std::atomic<uint64_t> requested_end_out_{0};
	std::atomic<int> loop_status_out_{static_cast<int>(LoopStatus::NoTracks)};
};

} // namespace lts
