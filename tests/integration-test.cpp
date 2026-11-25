// Scenario test for the loop track source.
//
// Starts libobs without the OBS frontend and without graphics, loads the plugin module, creates the loop track
// source the way OBS does when a user adds it, and reads the audio it outputs through an audio capture callback.
//
// Usage: audio-test <plugin module> <plugin data folder> <scenario>
// Scenarios:
//   scaffold-tone  The scaffold source outputs its placeholder tone on both channels.
// Exit codes: 0 on pass, 1 on a failed check, 2 when the test cannot start.

#include <obs.h>
#if defined(__linux__)
#include <obs-nix-platform.h>
#include <X11/Xlib.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char *kSourceId = "loop_track_switcher_source";
constexpr uint32_t kSampleRate = 48000;
// Half a second of audio per measurement
constexpr size_t kMeasureFrames = kSampleRate / 2;

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

struct Stats {
	double mean = 0.0;
	double rms = 0.0;
	double peak = 0.0;
	size_t distinct = 0;
};

Stats compute_stats(const std::vector<float> &samples)
{
	Stats stats;
	double sum = 0.0;
	double sum_squares = 0.0;
	std::set<float> distinct;
	for (float s : samples) {
		sum += s;
		sum_squares += double(s) * double(s);
		stats.peak = std::max(stats.peak, double(std::fabs(s)));
		if (distinct.size() < 1000)
			distinct.insert(s);
	}
	stats.mean = sum / double(samples.size());
	stats.rms = std::sqrt(sum_squares / double(samples.size()));
	stats.distinct = distinct.size();
	return stats;
}

// Discards what was captured so far, waits for the source to apply its settings, and collects new audio.
bool measure(Capture &capture, std::vector<float> &left, std::vector<float> &right)
{
	// The generator applies a settings change from the next 10 ms chunk. Skip 100 ms to be safe.
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	{
		std::lock_guard<std::mutex> lock(capture.mutex);
		capture.left.clear();
		capture.right.clear();
	}
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (std::chrono::steady_clock::now() < deadline) {
		{
			std::lock_guard<std::mutex> lock(capture.mutex);
			if (capture.left.size() >= kMeasureFrames) {
				left.assign(capture.left.begin(), capture.left.begin() + kMeasureFrames);
				right.assign(capture.right.begin(), capture.right.begin() + kMeasureFrames);
				return true;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	std::fprintf(stderr, "the source did not output %zu frames within five seconds\n", kMeasureFrames);
	return false;
}

int failures = 0;

void check(bool condition, const std::string &message)
{
	std::printf("%s: %s\n", condition ? "ok" : "FAILED", message.c_str());
	if (!condition)
		failures++;
}

// The scaffold source outputs a 440 Hz sine with amplitude 0.1 on both channels. A sine with amplitude A has an
// RMS of A / sqrt(2).
void check_tone(const std::vector<float> &left, const std::vector<float> &right)
{
	const double amplitude = 0.1;
	const double expected_rms = amplitude / std::sqrt(2.0);
	for (int ch = 0; ch < 2; ch++) {
		const Stats stats = compute_stats(ch == 0 ? left : right);
		const std::string name = ch == 0 ? "left" : "right";
		char buf[256];
		std::snprintf(buf, sizeof(buf), "%s RMS %.6f within 5%% of %.6f", name.c_str(), stats.rms,
			      expected_rms);
		check(std::fabs(stats.rms - expected_rms) <= expected_rms * 0.05, buf);
		std::snprintf(buf, sizeof(buf), "%s peak %.6f does not exceed %.6f", name.c_str(), stats.peak,
			      amplitude * 1.001);
		check(stats.peak <= amplitude * 1.001, buf);
	}
}

bool start_obs()
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
	if (!obs_startup("en-US", nullptr, nullptr)) {
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

int run_scenario(const std::string &scenario)
{
	// Creating the source by id is what OBS does when the user picks it from the source list
	obs_source_t *source = obs_source_create(kSourceId, "loop track test", nullptr, nullptr);
	if (!source) {
		std::fprintf(stderr, "failed to create source %s\n", kSourceId);
		return 2;
	}
	check((obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0, "source declares audio output");

	Capture capture;
	obs_source_add_audio_capture_callback(source, capture_audio, &capture);

	std::vector<float> left, right;
	if (scenario == "scaffold-tone") {
		if (measure(capture, left, right))
			check_tone(left, right);
		else
			failures++;
	} else {
		std::fprintf(stderr, "unknown scenario %s\n", scenario.c_str());
		obs_source_remove_audio_capture_callback(source, capture_audio, &capture);
		obs_source_release(source);
		return 2;
	}

	obs_source_remove_audio_capture_callback(source, capture_audio, &capture);
	obs_source_release(source);
	return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char **argv)
{
	if (argc != 4) {
		std::fprintf(stderr, "usage: audio-test <plugin module> <plugin data folder> <scenario>\n");
		return 2;
	}
	if (!start_obs())
		return 2;
	if (!load_plugin(argv[1], argv[2])) {
		obs_shutdown();
		return 2;
	}
	const int result = run_scenario(argv[3]);
	obs_shutdown();
	std::printf("%s %s\n", result == 0 ? "PASS" : "FAIL", argv[3]);
	return result;
}
