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
#include "track-loader.hpp"

#include <plugin-support.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/util_uint64.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

std::atomic<bool> bulk_teardown{false};

namespace {

// Each chunk is 10 ms. The chunk for [t, t + 10 ms) is sent at about t - 10 ms.
constexpr size_t kChunkFrames = 480;

// Settings keys. The track paths are shown in the properties. The others are written by save and read by create,
// because the properties dialog restores all settings when it is cancelled, which would undo changes made in the
// dock meanwhile.
constexpr const char *kLoopStart = "loop_start_frames";
constexpr const char *kLoopEnd = "loop_end_frames";
constexpr const char *kLoopEnabled = "loop_enabled";
constexpr const char *kCrossfade = "crossfade_enabled";

std::string bake_folder;

std::string track_key(int track, const char *suffix)
{
	return "track" + std::to_string(track + 1) + "_" + suffix;
}

std::string base_name(const std::string &path)
{
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

struct TrackStatus {
	TrackState state = TRACK_EMPTY;
	lts::LoadError error = lts::LoadError::None;
	std::string name;
	lts::LoadInfo info;
	uint64_t frames = 0;
	std::string bake_path;
	std::shared_ptr<const lts::TrackData> data;
};

struct LoopTrackSource {
	obs_source_t *source = nullptr;
	lts::Engine engine;

	std::thread generator;
	std::atomic<bool> stop_generator{false};

	// Loading runs on its own thread, one track at a time, so that the settings dialog and the audio never wait
	// for decoding. pending holds the latest requested path of each track.
	std::thread loader;
	std::mutex job_mutex;
	std::condition_variable job_cv;
	std::array<std::optional<std::string>, lts::kTrackCount> pending;
	bool quit_loader = false;
	std::atomic<bool> cancel_load{false};

	// Paths applied by update. Used only on the thread that calls update.
	std::array<std::string, lts::kTrackCount> paths;

	std::mutex status_mutex;
	std::array<TrackStatus, lts::kTrackCount> status;

	// Set when the user removes the source, as opposed to OBS removing all sources at exit
	std::atomic<bool> removed_by_user{false};
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
		// Stays one chunk ahead of the wall clock. os_sleepto_ns_fast only sleeps, so it may wake up a little late,
		// and the extra chunk covers that. os_sleepto_ns would spin for the last millisecond of every chunk on
		// Windows, which cost about 14% of a CPU core per source.
		os_sleepto_ns_fast(start_ns +
				   util_mul_div64(frames_sent - kChunkFrames, 1000000000ULL, lts::kSampleRate));
	}
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

		std::string old_bake;
		{
			std::lock_guard<std::mutex> lock(lt->status_mutex);
			old_bake = lt->status[track].bake_path;
			lt->status[track] = {};
			lt->status[track].state = path.empty() ? TRACK_EMPTY : TRACK_LOADING;
			lt->status[track].name = base_name(path);
		}

		TrackStatus loaded;
		loaded.name = base_name(path);
		if (!path.empty()) {
			const uint64_t start = os_gettime_ns();
			lts::LoadResult result = lts::load_track(path, bake_folder, lt->cancel_load);
			if (result.error == lts::LoadError::Cancelled)
				return;
			loaded.info = result.info;
			loaded.bake_path = result.bake_path;
			if (result.data) {
				loaded.state = TRACK_READY;
				loaded.frames = result.data->frames;
				loaded.data = result.data;
				obs_log(LOG_INFO, "loaded track %d in %.0f ms (%u Hz, %u ch%s%s)", track + 1,
					static_cast<double>(os_gettime_ns() - start) / 1e6, result.info.source_rate,
					result.info.source_channels, result.info.from_bake ? ", from bake" : "",
					result.info.trimmed ? ", trimmed" : "");
			} else {
				loaded.state = TRACK_ERROR;
				loaded.error = result.error;
				obs_log(LOG_WARNING, "failed to load track %d (error %d)", track + 1,
					static_cast<int>(result.error));
			}
			if (!result.detail.empty())
				obs_log(LOG_WARNING, "track %d: %s", track + 1, result.detail.c_str());
		}

		// The previous data is released here, outside the engine lock
		std::shared_ptr<const lts::TrackData> previous = lt->engine.replace_track(track, loaded.data);
		{
			std::lock_guard<std::mutex> lock(lt->status_mutex);
			lt->status[track] = std::move(loaded);
		}
		previous.reset();
		if (!old_bake.empty())
			lts::remove_bake_if_unused(old_bake);
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
}

void on_source_remove(void *data, calldata_t *)
{
	auto *lt = static_cast<LoopTrackSource *>(data);
	if (!bulk_teardown.load())
		lt->removed_by_user.store(true);
}

void loop_track_destroy(void *data)
{
	auto *lt = static_cast<LoopTrackSource *>(data);
	if (!lt)
		return;
	signal_handler_disconnect(obs_source_get_signal_handler(lt->source), "remove", on_source_remove, lt);
	lt->stop_generator.store(true);
	if (lt->generator.joinable())
		lt->generator.join();
	lt->cancel_load.store(true);
	{
		std::lock_guard<std::mutex> lock(lt->job_mutex);
		lt->quit_loader = true;
	}
	lt->job_cv.notify_one();
	if (lt->loader.joinable())
		lt->loader.join();

	// The bakes of a source the user removed are deleted unless another source uses the same file.
	// At exit or a scene collection change, they are kept for the next load.
	std::set<std::string> bakes;
	if (lt->removed_by_user.load()) {
		for (int t = 0; t < lts::kTrackCount; t++)
			if (!lt->status[t].bake_path.empty())
				bakes.insert(lt->status[t].bake_path);
	}
	for (int t = 0; t < lts::kTrackCount; t++) {
		lt->engine.replace_track(t, nullptr);
		lt->status[t].data.reset();
	}
	delete lt;
	for (const auto &bake : bakes)
		lts::remove_bake_if_unused(bake);
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

void proc_seek(void *data, calldata_t *cd)
{
	const long long frames = calldata_int(cd, "frames");
	static_cast<LoopTrackSource *>(data)->engine.seek(frames > 0 ? static_cast<uint64_t>(frames) : 0);
}

void proc_set_track(void *data, calldata_t *cd)
{
	auto *lt = static_cast<LoopTrackSource *>(data);
	lt->engine.set_track_enabled(static_cast<int>(calldata_int(cd, "track")), calldata_bool(cd, "enabled"));
}

void proc_set_loop(void *data, calldata_t *cd)
{
	static_cast<LoopTrackSource *>(data)->engine.set_loop_enabled(calldata_bool(cd, "enabled"));
}

void proc_set_loop_range(void *data, calldata_t *cd)
{
	const long long start = calldata_int(cd, "start");
	const long long end = calldata_int(cd, "end");
	static_cast<LoopTrackSource *>(data)->engine.set_loop(start > 0 ? static_cast<uint64_t>(start) : 0,
							      end > 0 ? static_cast<uint64_t>(end) : 0);
}

void proc_set_crossfade(void *data, calldata_t *cd)
{
	static_cast<LoopTrackSource *>(data)->engine.set_crossfade_enabled(calldata_bool(cd, "enabled"));
}

// Memory used by the tracks of this source. A track shared with another source is counted in both.
long long memory_bytes(LoopTrackSource *lt)
{
	std::set<const lts::TrackData *> seen;
	long long bytes = 0;
	for (const auto &status : lt->status) {
		if (status.data && seen.insert(status.data.get()).second)
			bytes += static_cast<long long>(status.data->samples.size() * sizeof(int16_t));
	}
	return bytes;
}

void proc_get_state(void *data, calldata_t *cd)
{
	auto *lt = static_cast<LoopTrackSource *>(data);
	lts::Engine &e = lt->engine;
	calldata_set_int(cd, "transport", static_cast<int>(e.transport()));
	calldata_set_int(cd, "position", static_cast<long long>(e.position()));
	calldata_set_int(cd, "song_end", static_cast<long long>(e.song_end()));
	calldata_set_int(cd, "loop_start", static_cast<long long>(e.loop_start()));
	calldata_set_int(cd, "loop_end", static_cast<long long>(e.loop_end()));
	calldata_set_int(cd, "requested_start", static_cast<long long>(e.requested_loop_start()));
	calldata_set_int(cd, "requested_end", static_cast<long long>(e.requested_loop_end()));
	calldata_set_int(cd, "loop_status", static_cast<int>(e.loop_status()));
	calldata_set_bool(cd, "loop_enabled", e.loop_enabled());
	calldata_set_bool(cd, "crossfade", e.crossfade_enabled());
	long long mask = 0;
	for (int t = 0; t < lts::kTrackCount; t++)
		if (e.track_enabled(t))
			mask |= 1LL << t;
	calldata_set_int(cd, "enabled_mask", mask);

	std::lock_guard<std::mutex> lock(lt->status_mutex);
	calldata_set_int(cd, "memory_bytes", memory_bytes(lt));
	for (int t = 0; t < lts::kTrackCount; t++) {
		const std::string i = std::to_string(t);
		const TrackStatus &s = lt->status[t];
		calldata_set_int(cd, ("state" + i).c_str(), s.state);
		calldata_set_int(cd, ("error" + i).c_str(), static_cast<int>(s.error));
		calldata_set_string(cd, ("name" + i).c_str(), s.name.c_str());
		calldata_set_int(cd, ("frames" + i).c_str(), static_cast<long long>(s.frames));
		calldata_set_int(cd, ("full_frames" + i).c_str(), static_cast<long long>(s.info.full_frames));
		calldata_set_int(cd, ("channels" + i).c_str(), s.info.source_channels);
		calldata_set_int(cd, ("rate" + i).c_str(), s.info.source_rate);
	}
}

void *loop_track_create(obs_data_t *settings, obs_source_t *source)
{
	auto *lt = new LoopTrackSource();
	lt->source = source;
	for (int t = 0; t < lts::kTrackCount; t++)
		lt->engine.set_track_enabled(t, obs_data_get_bool(settings, track_key(t, "on").c_str()));
	lt->engine.set_loop_enabled(obs_data_get_bool(settings, kLoopEnabled));
	lt->engine.set_crossfade_enabled(obs_data_get_bool(settings, kCrossfade));
	const long long start = obs_data_get_int(settings, kLoopStart);
	const long long end = obs_data_get_int(settings, kLoopEnd);
	lt->engine.set_loop(start > 0 ? static_cast<uint64_t>(start) : 0, end > 0 ? static_cast<uint64_t>(end) : 0);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void play()", proc_play, lt);
	proc_handler_add(ph, "void pause()", proc_pause, lt);
	proc_handler_add(ph, "void stop()", proc_stop, lt);
	proc_handler_add(ph, "void seek(int frames)", proc_seek, lt);
	proc_handler_add(ph, "void set_track(int track, bool enabled)", proc_set_track, lt);
	proc_handler_add(ph, "void set_loop(bool enabled)", proc_set_loop, lt);
	proc_handler_add(ph, "void set_loop_range(int start, int end)", proc_set_loop_range, lt);
	proc_handler_add(ph, "void set_crossfade(bool enabled)", proc_set_crossfade, lt);
	std::string decl = "void get_state(out int transport, out int position, out int song_end, out int loop_start, "
			   "out int loop_end, out int requested_start, out int requested_end, out int loop_status, "
			   "out bool loop_enabled, out bool crossfade, out int enabled_mask, out int memory_bytes";
	for (int t = 0; t < lts::kTrackCount; t++) {
		const std::string i = std::to_string(t);
		decl += ", out int state" + i + ", out int error" + i + ", out string name" + i + ", out int frames" +
			i + ", out int full_frames" + i + ", out int channels" + i + ", out int rate" + i;
	}
	decl += ")";
	proc_handler_add(ph, decl.c_str(), proc_get_state, lt);

	signal_handler_connect(obs_source_get_signal_handler(source), "remove", on_source_remove, lt);

	lt->loader = std::thread(loader_thread, lt);
	loop_track_update(lt, settings);
	lt->generator = std::thread(generator_thread, lt);
	return lt;
}

void loop_track_save(void *data, obs_data_t *settings)
{
	auto *lt = static_cast<LoopTrackSource *>(data);
	for (int t = 0; t < lts::kTrackCount; t++)
		obs_data_set_bool(settings, track_key(t, "on").c_str(), lt->engine.track_enabled(t));
	obs_data_set_bool(settings, kLoopEnabled, lt->engine.loop_enabled());
	obs_data_set_bool(settings, kCrossfade, lt->engine.crossfade_enabled());
	obs_data_set_int(settings, kLoopStart, static_cast<long long>(lt->engine.requested_loop_start()));
	obs_data_set_int(settings, kLoopEnd, static_cast<long long>(lt->engine.requested_loop_end()));
}

void loop_track_get_defaults(obs_data_t *settings)
{
	for (int t = 0; t < lts::kTrackCount; t++)
		obs_data_set_default_bool(settings, track_key(t, "on").c_str(), true);
	obs_data_set_default_bool(settings, kLoopEnabled, true);
	obs_data_set_default_bool(settings, kCrossfade, true);
	obs_data_set_default_int(settings, kLoopStart, 0);
	obs_data_set_default_int(settings, kLoopEnd, 0);
}

std::string status_line(const TrackStatus &status)
{
	switch (status.state) {
	case TRACK_LOADING:
		return std::string(obs_module_text("Status.Loading"));
	case TRACK_READY:
		return std::string(obs_module_text("Status.Ready"));
	case TRACK_ERROR:
		return std::string(load_error_text(static_cast<int>(status.error)));
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

	const std::string filter = std::string(obs_module_text("AudioFiles")) + " (*.wav *.mp3 *.flac *.ogg);;" +
				   obs_module_text("AllFiles") + " (*.*)";
	for (int t = 0; t < lts::kTrackCount; t++) {
		const std::string label = std::string(obs_module_text("Track")) + " " + std::to_string(t + 1);
		obs_properties_add_path(props, track_key(t, "path").c_str(), label.c_str(), OBS_PATH_FILE,
					filter.c_str(), nullptr);
	}

	if (lt) {
		std::string text;
		long long bytes = 0;
		{
			std::lock_guard<std::mutex> lock(lt->status_mutex);
			for (int t = 0; t < lts::kTrackCount; t++) {
				text += std::string(obs_module_text("Track")) + " " + std::to_string(t + 1) + ": " +
					status_line(lt->status[t]) + "\n";
			}
			bytes = memory_bytes(lt);
		}
		char memory[64];
		snprintf(memory, sizeof(memory), "%.0f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
		text += std::string(obs_module_text("Memory")) + ": " + memory;
		obs_properties_add_text(props, "status", text.c_str(), OBS_TEXT_INFO);
	}
	return props;
}

} // namespace

void set_bake_dir(const std::string &dir)
{
	bake_folder = dir;
}

const std::string &bake_dir()
{
	return bake_folder;
}

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
		return "";
	case lts::LoopStatus::StartNotBeforeEnd:
		return obs_module_text("Loop.StartNotBeforeEnd");
	case lts::LoopStatus::EndBeyondSong:
		return obs_module_text("Loop.EndBeyondSong");
	default:
		return obs_module_text("Loop.NoTracks");
	}
}

const char *load_error_text(int error)
{
	switch (static_cast<lts::LoadError>(error)) {
	case lts::LoadError::OpenFailed:
		return obs_module_text("Error.OpenFailed");
	case lts::LoadError::UnsupportedFormat:
		return obs_module_text("Error.UnsupportedFormat");
	case lts::LoadError::ReadFailed:
		return obs_module_text("Error.ReadFailed");
	case lts::LoadError::OutOfMemory:
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
