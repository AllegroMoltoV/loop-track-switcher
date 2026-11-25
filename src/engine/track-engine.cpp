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
#include <limits>

namespace lts {

namespace {

constexpr float kSampleScale = 1.0f / 32768.0f;
constexpr float kFadeStep = 1.0f / static_cast<float>(kFadeFrames);

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

Transport Engine::transport() const
{
	return static_cast<Transport>(requested_transport_.load());
}

uint64_t Engine::position() const
{
	return position_out_.load();
}

uint64_t Engine::loop_start() const
{
	return loop_start_out_.load();
}

uint64_t Engine::loop_end() const
{
	return loop_end_out_.load();
}

LoopStatus Engine::loop_status() const
{
	return static_cast<LoopStatus>(loop_status_out_.load());
}

std::unique_ptr<TrackData> Engine::replace_track(int track, std::unique_ptr<TrackData> data)
{
	if (track < 0 || track >= kTrackCount)
		return data;
	std::lock_guard<std::mutex> lock(mutex_);
	std::unique_ptr<TrackData> previous = std::move(tracks_[track]);
	tracks_[track] = std::move(data);
	validate_locked();
	return previous;
}

void Engine::set_loop(uint64_t start_frames, uint64_t end_frames)
{
	std::lock_guard<std::mutex> lock(mutex_);
	requested_start_ = start_frames;
	requested_end_ = end_frames;
	validate_locked();
}

void Engine::validate_locked()
{
	uint64_t shortest = std::numeric_limits<uint64_t>::max();
	bool any = false;
	for (const auto &track : tracks_) {
		if (track) {
			any = true;
			shortest = std::min(shortest, track->frames);
		}
	}

	LoopStatus status = LoopStatus::Ok;
	uint64_t end = requested_end_ == 0 ? shortest : requested_end_;
	if (!any)
		status = LoopStatus::NoTracks;
	else if (requested_start_ >= end)
		status = LoopStatus::StartNotBeforeEnd;
	else if (end > shortest)
		status = LoopStatus::EndBeyondTrack;

	loop_status_ = status;
	if (status == LoopStatus::Ok) {
		loop_start_ = requested_start_;
		loop_end_ = end;
		if (position_ >= loop_end_)
			position_ = loop_start_;
	} else {
		// Nothing can be played safely. Stop at once, without a fade, so that no track is read out of range.
		loop_start_ = 0;
		loop_end_ = 0;
		running_ = false;
		master_gain_ = 0.0f;
		position_ = 0;
		requested_transport_.store(static_cast<int>(Transport::Stopped));
	}
	publish_locked();
}

void Engine::publish_locked()
{
	position_out_.store(position_);
	loop_start_out_.store(loop_start_);
	loop_end_out_.store(loop_end_);
	loop_status_out_.store(static_cast<int>(loop_status_));
}

void Engine::render(float *left, float *right, size_t frames)
{
	std::lock_guard<std::mutex> lock(mutex_);

	auto requested = static_cast<Transport>(requested_transport_.load());
	if (loop_status_ != LoopStatus::Ok && requested != Transport::Stopped) {
		requested = Transport::Stopped;
		requested_transport_.store(static_cast<int>(Transport::Stopped));
	}
	const float master_target = requested == Transport::Playing ? 1.0f : 0.0f;

	std::array<float, kTrackCount> targets{};
	for (int t = 0; t < kTrackCount; t++)
		targets[t] = enabled_[t].load() ? 1.0f : 0.0f;

	if (requested == Transport::Playing && !running_) {
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

		for (; done < frames; done++) {
			master_gain_ = approach(master_gain_, master_target);
			float l = 0.0f;
			float r = 0.0f;
			for (int i = 0; i < active_count; i++) {
				const int t = active[i];
				track_gain_[t] = approach(track_gain_[t], targets[t]);
				const int16_t *sample = tracks_[t]->samples.data() + position_ * 2;
				l += static_cast<float>(sample[0]) * track_gain_[t];
				r += static_cast<float>(sample[1]) * track_gain_[t];
			}
			const float gain = master_gain_ * kSampleScale;
			left[done] = std::clamp(l * gain, -1.0f, 1.0f);
			right[done] = std::clamp(r * gain, -1.0f, 1.0f);

			if (++position_ >= loop_end_)
				position_ = loop_start_;

			if (master_gain_ == 0.0f && master_target == 0.0f) {
				running_ = false;
				if (requested == Transport::Stopped)
					position_ = 0;
				done++;
				break;
			}
		}
	} else {
		for (int t = 0; t < kTrackCount; t++)
			track_gain_[t] = targets[t];
		if (requested == Transport::Stopped)
			position_ = 0;
	}

	if (done < frames) {
		std::memset(left + done, 0, (frames - done) * sizeof(float));
		std::memset(right + done, 0, (frames - done) * sizeof(float));
	}
	publish_locked();
}

} // namespace lts
