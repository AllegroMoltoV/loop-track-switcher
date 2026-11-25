// Scenario test for the loop track source.
//
// Starts libobs without the OBS frontend and without graphics, loads the plugin module, creates the loop track
// source the way OBS does when a user adds it, and reads the audio it outputs through an audio capture callback.
// The test writes four short WAV files, each a sine at its own frequency, and tells the tracks apart by frequency.
//
// Usage: audio-test <plugin module> <plugin data folder> <scenario>
// Scenarios:
//   playback      Play, track switches, Pause, and Stop change the output as expected.
//   loop          Playback loops inside the loop range without gaps.
//   invalid-loop  A loop end beyond the tracks is reported, and Play stays silent.
//   outro         With the loop off, playback goes into the outro and stops at the song end. A seek into the
//                 outro also plays to the end and stops.
//   bake          Tracks at 44.1 kHz are baked into the module config folder. A new source with the same files
//                 reads the bakes. Removing the source by the user deletes them.
// Exit codes: 0 on pass, 1 on a failed check, 2 when the test cannot start.

#include <obs.h>
#if defined(__linux__)
#include <obs-nix-platform.h>
#include <X11/Xlib.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char *kSourceId = "loop_track_switcher_source";
constexpr uint32_t kSampleRate = 48000;
// Half a second of audio per measurement
constexpr size_t kMeasureFrames = kSampleRate / 2;
constexpr double kTrackSeconds = 2.0;
constexpr double kFrequencies[4] = {300.0, 500.0, 700.0, 900.0};
constexpr double kAmplitude = 0.2;
constexpr double kPi = 3.14159265358979323846;

struct Capture {
	std::mutex mutex;
	std::vector<float> left;
	std::vector<float> right;
};

void capture_audio(void *param, obs_source_t *, const struct audio_data *data, bool)
{
	auto *capture = static_cast<Capture *>(param);
	std::lock_guard<std::mutex> lock(capture->mutex);
	const auto *left = reinterpret_cast<const float *>(data->data[0]);
	const auto *right = reinterpret_cast<const float *>(data->data[1]);
	if (!left || !right)
		return;
	capture->left.insert(capture->left.end(), left, left + data->frames);
	capture->right.insert(capture->right.end(), right, right + data->frames);
}

int failures = 0;

void check(bool condition, const std::string &message)
{
	std::printf("%s: %s\n", condition ? "ok" : "FAILED", message.c_str());
	if (!condition)
		failures++;
}

void sleep_ms(int ms)
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Discards what was captured so far and collects the given number of new frames
bool collect(Capture &capture, size_t frames, std::vector<float> &left)
{
	{
		std::lock_guard<std::mutex> lock(capture.mutex);
		capture.left.clear();
		capture.right.clear();
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < deadline) {
		{
			std::lock_guard<std::mutex> lock(capture.mutex);
			if (capture.left.size() >= frames) {
				left.assign(capture.left.begin(), capture.left.begin() + frames);
				return true;
			}
		}
		sleep_ms(10);
	}
	std::fprintf(stderr, "the source did not output %zu frames in time\n", frames);
	failures++;
	return false;
}

double rms(const std::vector<float> &samples)
{
	double sum = 0.0;
	for (float s : samples)
		sum += double(s) * double(s);
	return samples.empty() ? 0.0 : std::sqrt(sum / double(samples.size()));
}

// Amplitude of one frequency component (Goertzel)
double tone_amplitude(const std::vector<float> &samples, double frequency)
{
	const double coefficient = 2.0 * std::cos(2.0 * kPi * frequency / kSampleRate);
	double s1 = 0.0, s2 = 0.0;
	for (float x : samples) {
		const double s0 = double(x) + coefficient * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
	return 2.0 * std::sqrt(std::max(power, 0.0)) / double(samples.size());
}

void write_tone_wav(const std::filesystem::path &path, double frequency, uint32_t rate)
{
	const uint32_t frames = static_cast<uint32_t>(kTrackSeconds * rate);
	std::vector<int16_t> data(frames * 2);
	for (uint32_t i = 0; i < frames; i++) {
		const auto v = static_cast<int16_t>(
			std::lround(kAmplitude * 32767.0 * std::sin(2.0 * kPi * frequency * i / rate)));
		data[i * 2] = v;
		data[i * 2 + 1] = v;
	}
	auto u32 = [](std::ofstream &f, uint32_t v) {
		for (int i = 0; i < 4; i++)
			f.put(static_cast<char>((v >> (8 * i)) & 0xff));
	};
	auto u16 = [](std::ofstream &f, uint16_t v) {
		f.put(static_cast<char>(v & 0xff));
		f.put(static_cast<char>(v >> 8));
	};
	const uint32_t bytes = frames * 4;
	std::ofstream f(path, std::ios::binary);
	f.write("RIFF", 4);
	u32(f, 36 + bytes);
	f.write("WAVEfmt ", 8);
	u32(f, 16);
	u16(f, 1);
	u16(f, 2);
	u32(f, rate);
	u32(f, rate * 4);
	u16(f, 4);
	u16(f, 16);
	f.write("data", 4);
	u32(f, bytes);
	f.write(reinterpret_cast<const char *>(data.data()), bytes);
}

struct State {
	long long transport = 0;
	double position = 0.0;
	long long loop_status = 0;
	long long ready_count = 0;
};

State get_state(obs_source_t *source)
{
	calldata_t cd = {};
	proc_handler_call(obs_source_get_proc_handler(source), "get_state", &cd);
	State state;
	state.transport = calldata_int(&cd, "transport");
	state.position = static_cast<double>(calldata_int(&cd, "position")) / kSampleRate;
	state.loop_status = calldata_int(&cd, "loop_status");
	for (int t = 0; t < 4; t++)
		if (calldata_int(&cd, ("state" + std::to_string(t)).c_str()) == 2)
			state.ready_count++;
	calldata_free(&cd);
	return state;
}

void call(obs_source_t *source, const char *procedure)
{
	calldata_t cd = {};
	proc_handler_call(obs_source_get_proc_handler(source), procedure, &cd);
	calldata_free(&cd);
}

void set_track(obs_source_t *source, int track, bool enabled)
{
	calldata_t cd = {};
	calldata_set_int(&cd, "track", track);
	calldata_set_bool(&cd, "enabled", enabled);
	proc_handler_call(obs_source_get_proc_handler(source), "set_track", &cd);
	calldata_free(&cd);
}

void set_loop_range(obs_source_t *source, double start_seconds, double end_seconds)
{
	calldata_t cd = {};
	calldata_set_int(&cd, "start", std::llround(start_seconds * kSampleRate));
	calldata_set_int(&cd, "end", std::llround(end_seconds * kSampleRate));
	proc_handler_call(obs_source_get_proc_handler(source), "set_loop_range", &cd);
	calldata_free(&cd);
}

void call_bool(obs_source_t *source, const char *procedure, bool value)
{
	calldata_t cd = {};
	calldata_set_bool(&cd, "enabled", value);
	proc_handler_call(obs_source_get_proc_handler(source), procedure, &cd);
	calldata_free(&cd);
}

void seek(obs_source_t *source, double seconds)
{
	calldata_t cd = {};
	calldata_set_int(&cd, "frames", std::llround(seconds * kSampleRate));
	proc_handler_call(obs_source_get_proc_handler(source), "seek", &cd);
	calldata_free(&cd);
}

// The fade is 200 ms. Waiting 300 ms lets a change settle before measuring.
void settle()
{
	sleep_ms(300);
}

void check_tones(const std::vector<float> &samples, const bool expected[4], const std::string &label)
{
	for (int t = 0; t < 4; t++) {
		const double amplitude = tone_amplitude(samples, kFrequencies[t]);
		char buf[256];
		if (expected[t]) {
			std::snprintf(buf, sizeof(buf), "%s: track %d (%.0f Hz) is heard, amplitude %.4f",
				      label.c_str(), t + 1, kFrequencies[t], amplitude);
			check(std::fabs(amplitude - kAmplitude) < kAmplitude * 0.1, buf);
		} else {
			std::snprintf(buf, sizeof(buf), "%s: track %d (%.0f Hz) is silent, amplitude %.4f",
				      label.c_str(), t + 1, kFrequencies[t], amplitude);
			check(amplitude < kAmplitude * 0.02, buf);
		}
	}
}

void check_silent(const std::vector<float> &samples, const std::string &label)
{
	const double level = rms(samples);
	char buf[128];
	std::snprintf(buf, sizeof(buf), "%s: output is silent, RMS %.6f", label.c_str(), level);
	check(level < 1e-6, buf);
}

void scenario_playback(obs_source_t *source, Capture &capture)
{
	std::vector<float> samples;
	if (collect(capture, kMeasureFrames, samples))
		check_silent(samples, "before play");

	set_track(source, 0, true);
	for (int t = 1; t < 4; t++)
		set_track(source, t, false);
	call(source, "play");
	settle();
	const bool only_first[4] = {true, false, false, false};
	if (collect(capture, kMeasureFrames, samples))
		check_tones(samples, only_first, "track 1 on");

	set_track(source, 2, true);
	settle();
	const bool first_and_third[4] = {true, false, true, false};
	if (collect(capture, kMeasureFrames, samples))
		check_tones(samples, first_and_third, "track 3 added");

	call(source, "pause");
	settle();
	if (collect(capture, kMeasureFrames, samples))
		check_silent(samples, "paused");
	const double paused_at = get_state(source).position;
	sleep_ms(200);
	check(get_state(source).position == paused_at && paused_at > 0.0, "pause keeps the position");
	check(get_state(source).transport == 2, "transport reports Paused");

	call(source, "play");
	settle();
	if (collect(capture, kMeasureFrames, samples))
		check_tones(samples, first_and_third, "resumed");

	call(source, "stop");
	settle();
	if (collect(capture, kMeasureFrames, samples))
		check_silent(samples, "stopped");
	check(get_state(source).position == 0.0, "stop returns the position to 0");
}

void scenario_loop(obs_source_t *source, Capture &capture)
{
	call(source, "play");
	settle();
	// Collect three seconds while the position passes the loop end (1.5 s) at least once
	std::vector<float> samples;
	double max_position = 0.0;
	bool returned = false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
	double previous = get_state(source).position;
	while (std::chrono::steady_clock::now() < deadline) {
		const double position = get_state(source).position;
		max_position = std::max(max_position, position);
		if (position < previous)
			returned = position >= 0.5 - 0.05;
		previous = position;
		sleep_ms(20);
	}
	check(max_position < 1.5, "position stays below the loop end of 1.5 s");
	check(returned, "position returns to the loop start of 0.5 s");

	if (collect(capture, kSampleRate * 2, samples)) {
		// Every 10 ms block keeps the level of four sines; a gap would show as a quiet block
		size_t quiet = 0;
		for (size_t i = 0; i + 480 <= samples.size(); i += 480) {
			std::vector<float> block(samples.begin() + i, samples.begin() + i + 480);
			if (rms(block) < 0.05)
				quiet++;
		}
		check(quiet == 0, "no 10 ms block is quiet across loop points (" + std::to_string(quiet) + " quiet)");
	}
	call(source, "stop");
}

void scenario_invalid_loop(obs_source_t *source, Capture &capture)
{
	set_loop_range(source, 0.0, 3.0);
	check(get_state(source).loop_status == 3, "a loop end beyond the 2 s tracks is reported");

	call(source, "play");
	settle();
	std::vector<float> samples;
	if (collect(capture, kMeasureFrames, samples))
		check_silent(samples, "play with an invalid loop");
	check(get_state(source).transport == 0, "transport stays Stopped");

	set_loop_range(source, 0.0, 0.0);
	check(get_state(source).loop_status == 0, "loop end 0 makes the loop valid again");
}

void scenario_outro(obs_source_t *source, Capture &capture)
{
	// The loop is 0.5 s to 1.5 s and the song ends at 2 s
	call_bool(source, "set_loop", false);
	call(source, "play");
	sleep_ms(2400);
	State state = get_state(source);
	check(state.transport == 0 && state.position == 0.0, "with the loop off, playback stops at the song end");
	std::vector<float> samples;
	if (collect(capture, kMeasureFrames, samples))
		check_silent(samples, "after the song end");

	call_bool(source, "set_loop", true);
	call(source, "play");
	settle();
	seek(source, 1.7);
	sleep_ms(600);
	state = get_state(source);
	check(state.transport == 0, "a seek into the outro plays to the song end and stops, even with the loop on");
}

void scenario_bake(obs_source_t *source, Capture &, const std::filesystem::path &config_dir, obs_data_t *settings)
{
	namespace fs = std::filesystem;
	const fs::path bakes = config_dir / "loop-track-switcher" / "bakes";
	auto count_bakes = [&bakes] {
		size_t count = 0;
		std::error_code ec;
		for (fs::directory_iterator it(bakes, ec), end; !ec && it != end; it.increment(ec))
			if (it->path().extension() == ".ltsbake")
				count++;
		return count;
	};
	check(count_bakes() == 4, "four tracks at 44.1 kHz are baked (" + std::to_string(count_bakes()) + ")");

	// Releasing without removal (as at exit) keeps the bakes. A new source loads the same files again.
	obs_source_release(source);
	obs_source_t *again = obs_source_create(kSourceId, "loop track test again", settings, nullptr);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (get_state(again).ready_count < 4 && std::chrono::steady_clock::now() < deadline)
		sleep_ms(20);
	check(get_state(again).ready_count == 4, "a new source loads the same files");
	check(count_bakes() == 4, "the bakes are kept when a source is released without removal");

	// Removing the source by the user deletes its bakes
	obs_source_remove(again);
	obs_source_release(again);
	const auto removed_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (count_bakes() > 0 && std::chrono::steady_clock::now() < removed_deadline)
		sleep_ms(20);
	check(count_bakes() == 0, "removing the source by the user deletes its bakes");
}

bool start_obs(const std::filesystem::path &config_dir)
{
#if defined(__linux__)
	// libobs on Linux expects a display for its hotkey handling. In CI, the display comes from xvfb-run.
	Display *display = XOpenDisplay(nullptr);
	if (!display) {
		std::fprintf(stderr, "failed to open X display\n");
		return false;
	}
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(display);
#endif
	// The module config folder holds the bakes. A temporary folder keeps the test independent of the user's OBS.
	const std::string config = config_dir.u8string() + "/";
	if (!obs_startup("en-US", config.c_str(), nullptr)) {
		std::fprintf(stderr, "failed to start OBS\n");
		return false;
	}
	struct obs_audio_info audio = {};
	audio.samples_per_sec = kSampleRate;
	audio.speakers = SPEAKERS_STEREO;
	if (!obs_reset_audio(&audio)) {
		std::fprintf(stderr, "failed to initialize OBS audio\n");
		obs_shutdown();
		return false;
	}
	return true;
}

bool load_plugin(const char *plugin_path, const char *module_data)
{
	obs_module_t *module = nullptr;
	if (obs_open_module(&module, plugin_path, module_data) != MODULE_SUCCESS || !obs_init_module(module)) {
		std::fprintf(stderr, "failed to load plugin module\n");
		return false;
	}
	return true;
}

int run_scenario(const std::string &scenario, const std::filesystem::path &config_dir)
{
	const auto dir = std::filesystem::temp_directory_path() / ("lts-integration-" + scenario);
	std::filesystem::create_directories(dir);
	// The bake scenario uses 44.1 kHz files, which are resampled and baked. The others use 48 kHz WAV.
	const uint32_t rate = scenario == "bake" ? 44100 : kSampleRate;
	obs_data_t *settings = obs_data_create();
	for (int t = 0; t < 4; t++) {
		const auto path = dir / ("track" + std::to_string(t + 1) + ".wav");
		write_tone_wav(path, kFrequencies[t], rate);
		obs_data_set_string(settings, ("track" + std::to_string(t + 1) + "_path").c_str(),
				    path.u8string().c_str());
	}

	// Creating the source by id is what OBS does when the user picks it from the source list
	obs_source_t *source = obs_source_create(kSourceId, "loop track test", settings, nullptr);
	if (!source) {
		obs_data_release(settings);
		std::fprintf(stderr, "failed to create source %s\n", kSourceId);
		return 2;
	}
	check((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0, "source declares audio output");

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (get_state(source).ready_count < 4 && std::chrono::steady_clock::now() < deadline)
		sleep_ms(20);
	check(get_state(source).ready_count == 4, "all four tracks are loaded");
	set_loop_range(source, 0.5, 1.5);
	check(get_state(source).loop_status == 0, "the loop range is valid");

	Capture capture;
	obs_source_add_audio_capture_callback(source, capture_audio, &capture);

	int result = 0;
	if (scenario == "playback")
		scenario_playback(source, capture);
	else if (scenario == "loop")
		scenario_loop(source, capture);
	else if (scenario == "invalid-loop")
		scenario_invalid_loop(source, capture);
	else if (scenario == "outro")
		scenario_outro(source, capture);
	else if (scenario == "bake") {
		obs_source_remove_audio_capture_callback(source, capture_audio, &capture);
		// The scenario releases the source itself
		scenario_bake(source, capture, config_dir, settings);
		source = nullptr;
	} else {
		std::fprintf(stderr, "unknown scenario %s\n", scenario.c_str());
		result = 2;
	}

	if (source) {
		obs_source_remove_audio_capture_callback(source, capture_audio, &capture);
		obs_source_release(source);
	}
	obs_data_release(settings);
	std::error_code ignored;
	std::filesystem::remove_all(dir, ignored);
	if (result != 0)
		return result;
	return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc != 4) {
		std::fprintf(stderr, "usage: audio-test <plugin module> <plugin data folder> <scenario>\n");
		return 2;
	}
	const auto config_dir =
		std::filesystem::temp_directory_path() / ("lts-integration-config-" + std::string(argv[3]));
	std::error_code ignored;
	std::filesystem::remove_all(config_dir, ignored);
	std::filesystem::create_directories(config_dir);
	if (!start_obs(config_dir))
		return 2;
	if (!load_plugin(argv[1], argv[2])) {
		obs_shutdown();
		return 2;
	}
	const int result = run_scenario(argv[3], config_dir);
	obs_shutdown();
	std::filesystem::remove_all(config_dir, ignored);
	std::printf("%s %s\n", result == 0 ? "PASS" : "FAIL", argv[3]);
	return result;
}
