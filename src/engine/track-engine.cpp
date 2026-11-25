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

#include "track-engine.hpp"

#include <algorithm>
#include <cstring>

namespace lts {

namespace {

constexpr float kSampleScale = 1.0f / 32768.0f;
constexpr float kFadeStep = 1.0f / static_cast<float>(kFadeFrames);
constexpr float kSeekStep = 1.0f / static_cast<float>(kSeekFadeFrames);

inline float approach(float value, float target)
{
	if (value < target)
		return std::min(value + kFadeStep, target);
	if (value > target)
		return std::max(value - kFadeStep, target);
	return value;
}

} // namespace

Engine::Engine()
{
	for (auto &enabled : enabled_)
		enabled.store(true);
}

void Engine::play()
{
	requested_transport_.store(static_cast<int>(Transport::Playing));
}

void Engine::pause()
{
	requested_transport_.store(static_cast<int>(Transport::Paused));
}

void Engine::stop()
{
	requested_transport_.store(static_cast<int>(Transport::Stopped));
	stop_count_.fetch_add(1);
}

void Engine::seek(uint64_t frames)
{
	seek_request_.store(frames);
	seek_count_.fetch_add(1);
}

void Engine::set_track_enabled(int track, bool enabled)
{
	if (track >= 0 && track < kTrackCount)
		enabled_[track].store(enabled);
}

bool Engine::track_enabled(int track) const
{
	return track >= 0 && track < kTrackCount && enabled_[track].load();
}

void Engine::set_loop_enabled(bool enabled)
{
	loop_enabled_.store(enabled);
}

bool Engine::loop_enabled() const
{
	return loop_enabled_.load();
}

void Engine::set_crossfade_enabled(bool enabled)
{
	crossfade_enabled_.store(enabled);
}

bool Engine::crossfade_enabled() const
{
	return crossfade_enabled_.load();
}

Transport Engine::transport() const
{
	return static_cast<Transport>(requested_transport_.load());
}

uint64_t Engine::position() const
{
	return position_out_.load();
}

uint64_t Engine::song_end() const
{
	return song_end_out_.load();
}

uint64_t Engine::loop_start() const
{
	return loop_start_out_.load();
}

uint64_t Engine::loop_end() const
{
	return loop_end_out_.load();
}

uint64_t Engine::requested_loop_start() const
{
	return requested_start_out_.load();
}

uint64_t Engine::requested_loop_end() const
{
	return requested_end_out_.load();
}

LoopStatus Engine::loop_status() const
{
	return static_cast<LoopStatus>(loop_status_out_.load());
}

std::shared_ptr<const TrackData> Engine::replace_track(int track, std::shared_ptr<const TrackData> data)
{
	if (track < 0 || track >= kTrackCount)
		return data;
	std::lock_guard<std::mutex> lock(mutex_);
	std::shared_ptr<const TrackData> previous = std::move(tracks_[track]);
	tracks_[track] = std::move(data);
	validate_locked(false);
	return previous;
}

void Engine::set_loop(uint64_t start_frames, uint64_t end_frames)
{
	std::lock_guard<std::mutex> lock(mutex_);
	requested_start_ = start_frames;
	requested_end_ = end_frames;
	validate_locked(true);
}

void Engine::stop_now_locked()
{
	running_ = false;
	master_gain_ = 0.0f;
	seek_phase_ = 0;
	seek_gain_ = 1.0f;
	position_ = 0;
	requested_transport_.store(static_cast<int>(Transport::Stopped));
}

void Engine::validate_locked(bool loop_changed)
{
	uint64_t longest = 0;
	bool any = false;
	for (const auto &track : tracks_) {
		if (track) {
			any = true;
			longest = std::max(longest, track->frames);
		}
	}
	song_end_ = longest;

	LoopStatus status = LoopStatus::Ok;
	const uint64_t end = requested_end_ == 0 ? longest : requested_end_;
	if (!any || longest == 0)
		status = LoopStatus::NoTracks;
	else if (requested_start_ >= end)
		status = LoopStatus::StartNotBeforeEnd;
	else if (end > longest)
		status = LoopStatus::EndBeyondSong;

	const bool was_ok = loop_status_ == LoopStatus::Ok;
	const uint64_t old_start = loop_start_;
	const uint64_t old_end = loop_end_;
	loop_status_ = status;

	if (status == LoopStatus::Ok) {
		loop_start_ = requested_start_;
		loop_end_ = end;
		// The check keeps the position except in two cases (docs/reference/playback-states.md)
		if (position_ >= song_end_) {
			// The song became shorter than the position: treated as reaching the song end
			stop_now_locked();
		} else if (loop_changed && was_ok && loop_enabled_.load() && position_ >= old_start &&
			   position_ < old_end && position_ >= loop_end_) {
			// The loop end moved to or before the position while playing the loop
			position_ = loop_start_;
		}
	} else {
		// Nothing can be played safely. Stop at once, without a fade.
		loop_start_ = 0;
		loop_end_ = 0;
		stop_now_locked();
	}
	publish_locked();
}

void Engine::publish_locked()
{
	position_out_.store(position_);
	song_end_out_.store(song_end_);
	loop_start_out_.store(loop_start_);
	loop_end_out_.store(loop_end_);
	requested_start_out_.store(requested_start_);
	requested_end_out_.store(requested_end_);
	loop_status_out_.store(static_cast<int>(loop_status_));
}

// A track shorter than the song is silent after its own end
inline float Engine::sample_left(int track, uint64_t position) const
{
	const TrackData &data = *tracks_[track];
	return position < data.frames ? static_cast<float>(data.samples[position * 2]) : 0.0f;
}

inline float Engine::sample_right(int track, uint64_t position) const
{
	const TrackData &data = *tracks_[track];
	return position < data.frames ? static_cast<float>(data.samples[position * 2 + 1]) : 0.0f;
}

void Engine::render(float *left, float *right, size_t frames)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto requested = static_cast<Transport>(requested_transport_.load());
	if (loop_status_ != LoopStatus::Ok && requested != Transport::Stopped) {
		requested = Transport::Stopped;
		requested_transport_.store(static_cast<int>(Transport::Stopped));
	}

	const uint32_t stops = stop_count_.load();
	if (stops != seen_stop_count_) {
		seen_stop_count_ = stops;
		// Stop while no sound is produced returns the position to 0 at once. While sound is produced,
		// it returns after the fade out below.
		if (!running_)
			position_ = 0;
	}

	const uint32_t seeks = seek_count_.load();
	if (seeks != seen_seek_count_) {
		seen_seek_count_ = seeks;
		const uint64_t target = song_end_ == 0 ? 0 : std::min(seek_request_.load(), song_end_ - 1);
		if (running_) {
			seek_target_ = target;
			seek_phase_ = 1;
		} else {
			position_ = target;
		}
	}

	const float master_target = requested == Transport::Playing ? 1.0f : 0.0f;
	std::array<float, kTrackCount> targets{};
	for (int t = 0; t < kTrackCount; t++)
		targets[t] = enabled_[t].load() ? 1.0f : 0.0f;

	if (requested == Transport::Playing && !running_ && loop_status_ == LoopStatus::Ok) {
		// Starting from silence: only the master gain fades in, so the tracks start at their switch state
		running_ = true;
		track_gain_ = targets;
	}

	size_t done = 0;
	if (running_) {
		// Tracks that are silent and stay silent are neither read nor mixed. The shared position still
		// advances, so a track enabled later starts in sync.
		std::array<int, kTrackCount> active{};
		int active_count = 0;
		for (int t = 0; t < kTrackCount; t++) {
			if (tracks_[t] && (track_gain_[t] > 0.0f || targets[t] > 0.0f))
				active[active_count++] = t;
			else
				track_gain_[t] = targets[t];
		}

		const bool loop_on = loop_enabled_.load();
		const uint64_t seam = crossfade_enabled_.load() ? std::min<uint64_t>(kSeamFrames, loop_start_) : 0;

		for (; done < frames; done++) {
			master_gain_ = approach(master_gain_, master_target);
			if (seek_phase_ == 1) {
				seek_gain_ -= kSeekStep;
				if (seek_gain_ <= 0.0f) {
					seek_gain_ = 0.0f;
					position_ = seek_target_;
					seek_phase_ = 2;
				}
			} else if (seek_phase_ == 2) {
				seek_gain_ += kSeekStep;
				if (seek_gain_ >= 1.0f) {
					seek_gain_ = 1.0f;
					seek_phase_ = 0;
				}
			}

			// In the last 10 ms of the loop, blend in the audio just before the loop start
			const bool in_seam = loop_on && seam > 0 && position_ >= loop_start_ && position_ < loop_end_ &&
					     position_ >= loop_end_ - seam;
			float weight = 0.0f;
			uint64_t other = 0;
			if (in_seam) {
				weight = static_cast<float>(position_ - (loop_end_ - seam) + 1) /
					 static_cast<float>(seam + 1);
				other = position_ - (loop_end_ - loop_start_);
			}

			float l = 0.0f;
			float r = 0.0f;
			for (int i = 0; i < active_count; i++) {
				const int t = active[i];
				track_gain_[t] = approach(track_gain_[t], targets[t]);
				float sl = sample_left(t, position_);
				float sr = sample_right(t, position_);
				if (in_seam) {
					sl += (sample_left(t, other) - sl) * weight;
					sr += (sample_right(t, other) - sr) * weight;
				}
				l += sl * track_gain_[t];
				r += sr * track_gain_[t];
			}
			const float gain = master_gain_ * seek_gain_ * kSampleScale;
			left[done] = std::clamp(l * gain, -1.0f, 1.0f);
			right[done] = std::clamp(r * gain, -1.0f, 1.0f);

			const uint64_t previous = position_++;
			if (loop_on && previous < loop_end_ && position_ >= loop_end_) {
				position_ = loop_start_;
			} else if (position_ >= song_end_) {
				// The song end stops without a fade, and the next chunk produces no sound
				stop_now_locked();
				done++;
				break;
			}

			if (master_gain_ == 0.0f && master_target == 0.0f) {
				running_ = false;
				// A seek requested during the fade out still moves the position
				if (seek_phase_ == 1)
					position_ = seek_target_;
				seek_phase_ = 0;
				seek_gain_ = 1.0f;
				if (requested == Transport::Stopped)
					position_ = 0;
				done++;
				break;
			}
		}
	} else {
		for (int t = 0; t < kTrackCount; t++)
			track_gain_[t] = targets[t];
	}

	if (done < frames) {
		std::memset(left + done, 0, (frames - done) * sizeof(float));
		std::memset(right + done, 0, (frames - done) * sizeof(float));
	}
	publish_locked();
}

} // namespace lts
