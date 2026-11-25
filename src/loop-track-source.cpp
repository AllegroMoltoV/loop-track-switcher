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

#include "loop-track-source.hpp"

#include "track-engine.hpp"
#include "wav-loader.hpp"

#include <plugin-support.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/util_uint64.h>

#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace {

// Each chunk is 10 ms. The chunk for [t, t + 10 ms) is sent at time t, the same way as simple-white-noise.
constexpr size_t kChunkFrames = 480;

constexpr const char *kLoopStart = "loop_start";
constexpr const char *kLoopEnd = "loop_end";

std::string track_key(int track, const char *suffix)
{
	return "track" + std::to_string(track + 1) + "_" + suffix;
}

std::string base_name(const std::string &path)
{
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

uint64_t seconds_to_frames(double seconds)
{
	if (!(seconds > 0.0))
		return 0;
	return static_cast<uint64_t>(std::llround(seconds * lts::kSampleRate));
}

struct TrackStatus {
	TrackState state = TRACK_EMPTY;
	lts::WavError error = lts::WavError::None;
	std::string name;
};

struct LoopTrackSource {
	obs_source_t *source = nullptr;
	lts::Engine engine;

	std::thread generator;
	std::atomic<bool> stop_generator{false};

	// Loading runs on its own thread so that the settings dialog and the audio never wait for the disk.
	// pending holds the latest requested path of each track.
	std::thread loader;
	std::mutex job_mutex;
	std::condition_variable job_cv;
	std::array<std::optional<std::string>, lts::kTrackCount> pending;
	bool quit_loader = false;

	// Paths applied by update. Used only on the thread that calls update.
	std::array<std::string, lts::kTrackCount> paths;

	std::mutex status_mutex;
	std::array<TrackStatus, lts::kTrackCount> status;
};

void generator_thread(LoopTrackSource *lt)
{
	float left[kChunkFrames];
	float right[kChunkFrames];
	const uint64_t start_ns = os_gettime_ns();
	uint64_t frames_sent = 0;

	os_set_thread_name("loop-track-switcher");

	while (!lt->stop_generator.load()) {
		lt->engine.render(left, right, kChunkFrames);

		obs_source_audio audio = {};
		audio.data[0] = reinterpret_cast<const uint8_t *>(left);
		audio.data[1] = reinterpret_cast<const uint8_t *>(right);
		audio.frames = static_cast<uint32_t>(kChunkFrames);
		audio.speakers = SPEAKERS_STEREO;
		audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
		audio.samples_per_sec = lts::kSampleRate;
		audio.timestamp = start_ns + util_mul_div64(frames_sent, 1000000000ULL, lts::kSampleRate);
		obs_source_output_audio(lt->source, &audio);

		frames_sent += kChunkFrames;
		os_sleepto_ns(start_ns + util_mul_div64(frames_sent, 1000000000ULL, lts::kSampleRate));
	}
}

void set_status(LoopTrackSource *lt, int track, TrackState state, lts::WavError error, const std::string &path)
{
	std::lock_guard<std::mutex> lock(lt->status_mutex);
	lt->status[track] = {state, error, base_name(path)};
}

void loader_thread(LoopTrackSource *lt)
{
	os_set_thread_name("loop-track-switcher-loader");
	for (;;) {
		int track = -1;
		std::string path;
		{
			std::unique_lock<std::mutex> lock(lt->job_mutex);
			lt->job_cv.wait(lock, [lt] {
				if (lt->quit_loader)
					return true;
				for (const auto &job : lt->pending)
					if (job)
						return true;
				return false;
			});
			if (lt->quit_loader)
				return;
			for (int t = 0; t < lts::kTrackCount; t++) {
				if (lt->pending[t]) {
					track = t;
					path = std::move(*lt->pending[t]);
					lt->pending[t].reset();
					break;
				}
			}
		}

		std::unique_ptr<lts::TrackData> previous;
		if (path.empty()) {
			previous = lt->engine.replace_track(track, nullptr);
			set_status(lt, track, TRACK_EMPTY, lts::WavError::None, path);
		} else {
			set_status(lt, track, TRACK_LOADING, lts::WavError::None, path);
			lts::WavLoadResult result = lts::load_wav(path);
			if (result.data) {
				previous = lt->engine.replace_track(track, std::move(result.data));
				set_status(lt, track, TRACK_READY, lts::WavError::None, path);
				obs_log(LOG_INFO, "loaded track %d", track + 1);
			} else {
				previous = lt->engine.replace_track(track, nullptr);
				set_status(lt, track, TRACK_ERROR, result.error, path);
				obs_log(LOG_WARNING, "failed to load track %d (error %d)", track + 1,
					static_cast<int>(result.error));
			}
		}
		// The previous data is freed here, outside the engine lock
		previous.reset();
	}
}

const char *loop_track_get_name(void *)
{
	return obs_module_text("LoopTrackSwitcher");
}

void loop_track_update(void *data, obs_data_t *settings)
{
	auto *lt = static_cast<LoopTrackSource *>(data);
	bool queued = false;
	{
		std::lock_guard<std::mutex> lock(lt->job_mutex);
		for (int t = 0; t < lts::kTrackCount; t++) {
			std::string path = obs_data_get_string(settings, track_key(t, "path").c_str());
			if (path != lt->paths[t]) {
				lt->paths[t] = path;
				lt->pending[t] = path;
				queued = true;
			}
		}
	}
	if (queued)
		lt->job_cv.notify_one();

	// A loop change does not reload the files. It only validates the range against the loaded tracks.
	lt->engine.set_loop(seconds_to_frames(obs_data_get_double(settings, kLoopStart)),
			    seconds_to_frames(obs_data_get_double(settings, kLoopEnd)));
}

void loop_track_destroy(void *data)
{
	auto *lt = static_cast<LoopTrackSource *>(data);
	if (!lt)
		return;
	lt->stop_generator.store(true);
	if (lt->generator.joinable())
		lt->generator.join();
	{
		std::lock_guard<std::mutex> lock(lt->job_mutex);
		lt->quit_loader = true;
	}
	lt->job_cv.notify_one();
	if (lt->loader.joinable())
		lt->loader.join();
	delete lt;
}

void proc_play(void *data, calldata_t *)
{
	static_cast<LoopTrackSource *>(data)->engine.play();
}

void proc_pause(void *data, calldata_t *)
{
	static_cast<LoopTrackSource *>(data)->engine.pause();
}

void proc_stop(void *data, calldata_t *)
{
	static_cast<LoopTrackSource *>(data)->engine.stop();
}

void proc_set_track(void *data, calldata_t *cd)
{
	auto *lt = static_cast<LoopTrackSource *>(data);
	lt->engine.set_track_enabled(static_cast<int>(calldata_int(cd, "track")), calldata_bool(cd, "enabled"));
}

void proc_get_state(void *data, calldata_t *cd)
{
	auto *lt = static_cast<LoopTrackSource *>(data);
	const double rate = lts::kSampleRate;
	calldata_set_int(cd, "transport", static_cast<int>(lt->engine.transport()));
	calldata_set_float(cd, "position", static_cast<double>(lt->engine.position()) / rate);
	calldata_set_float(cd, "loop_start", static_cast<double>(lt->engine.loop_start()) / rate);
	calldata_set_float(cd, "loop_end", static_cast<double>(lt->engine.loop_end()) / rate);
	calldata_set_int(cd, "loop_status", static_cast<int>(lt->engine.loop_status()));
	long long mask = 0;
	for (int t = 0; t < lts::kTrackCount; t++)
		if (lt->engine.track_enabled(t))
			mask |= 1LL << t;
	calldata_set_int(cd, "enabled_mask", mask);

	std::lock_guard<std::mutex> lock(lt->status_mutex);
	for (int t = 0; t < lts::kTrackCount; t++) {
		const std::string index = std::to_string(t);
		calldata_set_int(cd, ("state" + index).c_str(), lt->status[t].state);
		calldata_set_int(cd, ("error" + index).c_str(), static_cast<int>(lt->status[t].error));
		calldata_set_string(cd, ("name" + index).c_str(), lt->status[t].name.c_str());
	}
}

void *loop_track_create(obs_data_t *settings, obs_source_t *source)
{
	auto *lt = new LoopTrackSource();
	lt->source = source;
	for (int t = 0; t < lts::kTrackCount; t++)
		lt->engine.set_track_enabled(t, obs_data_get_bool(settings, track_key(t, "on").c_str()));

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void play()", proc_play, lt);
	proc_handler_add(ph, "void pause()", proc_pause, lt);
	proc_handler_add(ph, "void stop()", proc_stop, lt);
	proc_handler_add(ph, "void set_track(int track, bool enabled)", proc_set_track, lt);
	proc_handler_add(ph,
			 "void get_state(out int transport, out float position, out float loop_start, "
			 "out float loop_end, out int loop_status, out int enabled_mask, "
			 "out int state0, out int state1, out int state2, out int state3, "
			 "out int error0, out int error1, out int error2, out int error3, "
			 "out string name0, out string name1, out string name2, out string name3)",
			 proc_get_state, lt);

	lt->loader = std::thread(loader_thread, lt);
	loop_track_update(lt, settings);
	lt->generator = std::thread(generator_thread, lt);
	return lt;
}

// The track switches are not settings shown in the properties. The properties dialog restores all settings when
// it is cancelled, which would undo switches made in the dock meanwhile. The engine holds them, and they are
// written to the settings only when the scene collection is saved.
void loop_track_save(void *data, obs_data_t *settings)
{
	auto *lt = static_cast<LoopTrackSource *>(data);
	for (int t = 0; t < lts::kTrackCount; t++)
		obs_data_set_bool(settings, track_key(t, "on").c_str(), lt->engine.track_enabled(t));
}

void loop_track_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, kLoopStart, 0.0);
	obs_data_set_default_double(settings, kLoopEnd, 0.0);
	for (int t = 0; t < lts::kTrackCount; t++)
		obs_data_set_default_bool(settings, track_key(t, "on").c_str(), true);
}

std::string status_line(const TrackStatus &status)
{
	switch (status.state) {
	case TRACK_LOADING:
		return std::string(obs_module_text("Status.Loading"));
	case TRACK_READY:
		return std::string(obs_module_text("Status.Ready"));
	case TRACK_ERROR:
		return std::string(wav_error_text(static_cast<int>(status.error)));
	default:
		return std::string(obs_module_text("Status.Empty"));
	}
}

obs_properties_t *loop_track_get_properties(void *data)
{
	auto *lt = static_cast<LoopTrackSource *>(data);
	obs_properties_t *props = obs_properties_create();
	// Apply the settings once when OK is pressed, not on every keystroke in a path field
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	for (int t = 0; t < lts::kTrackCount; t++) {
		const std::string label = std::string(obs_module_text("Track")) + " " + std::to_string(t + 1);
		obs_properties_add_path(props, track_key(t, "path").c_str(), label.c_str(), OBS_PATH_FILE,
					"WAV (*.wav)", nullptr);
	}

	obs_property_t *start =
		obs_properties_add_float(props, kLoopStart, obs_module_text("LoopStart"), 0.0, 86400.0, 0.001);
	obs_property_float_set_suffix(start, " s");
	obs_property_t *end =
		obs_properties_add_float(props, kLoopEnd, obs_module_text("LoopEnd"), 0.0, 86400.0, 0.001);
	obs_property_float_set_suffix(end, " s");
	obs_property_set_long_description(end, obs_module_text("LoopEnd.Description"));

	if (lt) {
		std::string text;
		{
			std::lock_guard<std::mutex> lock(lt->status_mutex);
			for (int t = 0; t < lts::kTrackCount; t++) {
				text += std::string(obs_module_text("Track")) + " " + std::to_string(t + 1) + ": " +
					status_line(lt->status[t]) + "\n";
			}
		}
		text += std::string(obs_module_text("Loop")) + ": " +
			loop_status_text(static_cast<int>(lt->engine.loop_status()));
		obs_properties_add_text(props, "status", text.c_str(), OBS_TEXT_INFO);
	}
	return props;
}

} // namespace

const char *transport_text(int transport)
{
	switch (static_cast<lts::Transport>(transport)) {
	case lts::Transport::Playing:
		return obs_module_text("Transport.Playing");
	case lts::Transport::Paused:
		return obs_module_text("Transport.Paused");
	default:
		return obs_module_text("Transport.Stopped");
	}
}

const char *loop_status_text(int status)
{
	switch (static_cast<lts::LoopStatus>(status)) {
	case lts::LoopStatus::Ok:
		return obs_module_text("Loop.Ok");
	case lts::LoopStatus::StartNotBeforeEnd:
		return obs_module_text("Loop.StartNotBeforeEnd");
	case lts::LoopStatus::EndBeyondTrack:
		return obs_module_text("Loop.EndBeyondTrack");
	default:
		return obs_module_text("Loop.NoTracks");
	}
}

const char *wav_error_text(int error)
{
	switch (static_cast<lts::WavError>(error)) {
	case lts::WavError::OpenFailed:
		return obs_module_text("Error.OpenFailed");
	case lts::WavError::NotStereo:
		return obs_module_text("Error.NotStereo");
	case lts::WavError::Not48k:
		return obs_module_text("Error.Not48k");
	case lts::WavError::UnsupportedFormat:
		return obs_module_text("Error.UnsupportedFormat");
	case lts::WavError::ReadFailed:
		return obs_module_text("Error.ReadFailed");
	case lts::WavError::OutOfMemory:
		return obs_module_text("Error.OutOfMemory");
	default:
		return "";
	}
}

void register_loop_track_source()
{
	obs_source_info info = {};
	info.id = kLoopTrackSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.icon_type = OBS_ICON_TYPE_AUDIO_INPUT;
	info.get_name = loop_track_get_name;
	info.create = loop_track_create;
	info.destroy = loop_track_destroy;
	info.update = loop_track_update;
	info.save = loop_track_save;
	info.get_defaults = loop_track_get_defaults;
	info.get_properties = loop_track_get_properties;
	obs_register_source(&info);
}
