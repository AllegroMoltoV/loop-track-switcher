// Unit test of the track engine and the audio loader. Uses neither OBS nor Qt.
//
// It guards the lines that later changes must not break (see the plan, work item 5):
// - reading out of range with short tracks, and using a stale bake (the plugin would crash or play wrong audio)
// - behavior that depends on the third-party libraries (MP3 delay removal, resampling length, decoding)
// - the key state transitions of docs/reference/playback-states.md
//
// Usage: engine-test <folder of tests/data>
// Exit codes: 0 on pass, 1 on a failed check.

#include "track-engine.hpp"
#include "track-loader.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const std::string &message)
{
	std::printf("%s: %s\n", condition ? "ok" : "FAILED", message.c_str());
	if (!condition)
		failures++;
}

// A track whose left channel holds the frame index modulo 10000 and whose right channel holds a constant
std::shared_ptr<lts::TrackData> make_track(uint64_t frames, int16_t right_value)
{
	auto track = std::make_shared<lts::TrackData>();
	track->frames = frames;
	track->samples.resize(frames * 2);
	for (uint64_t i = 0; i < frames; i++) {
		track->samples[i * 2] = static_cast<int16_t>(i % 10000);
		track->samples[i * 2 + 1] = right_value;
	}
	return track;
}

struct Rendered {
	std::vector<float> left;
	std::vector<float> right;
	std::vector<uint64_t> position_after_chunk;
};

Rendered render(lts::Engine &engine, size_t chunks, size_t chunk_frames = 480)
{
	Rendered out;
	std::vector<float> l(chunk_frames), r(chunk_frames);
	for (size_t c = 0; c < chunks; c++) {
		engine.render(l.data(), r.data(), chunk_frames);
		out.left.insert(out.left.end(), l.begin(), l.end());
		out.right.insert(out.right.end(), r.begin(), r.end());
		out.position_after_chunk.push_back(engine.position());
	}
	return out;
}

constexpr float kScale = 1.0f / 32768.0f;

float max_step(const std::vector<float> &samples, size_t from, size_t to)
{
	float step = 0.0f;
	for (size_t i = from + 1; i < std::min(to, samples.size()); i++)
		step = std::max(step, std::fabs(samples[i] - samples[i - 1]));
	return step;
}

// --- State transitions ---

void test_loop_wraps()
{
	lts::Engine engine;
	engine.replace_track(0, make_track(4800, 1000));
	engine.set_loop(1000, 3000);
	engine.set_crossfade_enabled(false);
	check(engine.loop_status() == lts::LoopStatus::Ok, "loop 1000-3000 is valid");
	engine.play();
	const Rendered out = render(engine, 40);
	bool in_range = true;
	for (uint64_t pos : out.position_after_chunk)
		if (pos >= 3000)
			in_range = false;
	check(in_range, "position stays below the loop end while the loop is on");
	bool found_jump = false;
	for (size_t i = 12000; i + 1 < out.left.size(); i++)
		if (std::fabs(out.left[i] / kScale - 2999.0f) < 0.5f &&
		    std::fabs(out.left[i + 1] / kScale - 1000.0f) < 0.5f)
			found_jump = true;
	check(found_jump, "sample 2999 is followed by sample 1000");
}

void test_outro_and_song_end()
{
	lts::Engine engine;
	engine.replace_track(0, make_track(9600, 1000));
	engine.set_loop(1000, 3000);
	engine.set_loop_enabled(false);
	engine.play();
	const Rendered out = render(engine, 25); // 12,000 frames, past the song end of 9,600
	bool passed_loop_end = false;
	for (uint64_t pos : out.position_after_chunk)
		if (pos > 3000)
			passed_loop_end = true;
	check(passed_loop_end, "with the loop off, the position goes on into the outro");
	check(engine.transport() == lts::Transport::Stopped && engine.position() == 0,
	      "the song end stops playback and returns the position to 0");
	check(!engine.loop_enabled(), "stopping does not change the loop switch");
	check(out.right[9599] != 0.0f && out.right[9600] == 0.0f, "the song end stops without a fade");

	// Without an outro, the loop off stops at the loop end
	lts::Engine no_outro;
	no_outro.replace_track(0, make_track(4800, 1000));
	no_outro.set_loop_enabled(false);
	no_outro.play();
	render(no_outro, 12);
	check(no_outro.transport() == lts::Transport::Stopped, "without an outro, the loop off stops at the loop end");
}

void test_stopped_position_and_seek()
{
	lts::Engine engine;
	engine.replace_track(0, make_track(48000, 1000));
	engine.seek(3000);
	render(engine, 3);
	check(engine.position() == 3000, "a seek while stopped is kept");
	engine.play();
	const Rendered out = render(engine, 25);
	const float expected = static_cast<float>((3000 + lts::kFadeFrames + 100) % 10000);
	check(std::fabs(out.left[lts::kFadeFrames + 100] / kScale - expected) < 1.0f,
	      "play starts from the position set while stopped");
	engine.pause();
	render(engine, 25);
	engine.stop();
	render(engine, 1);
	check(engine.position() == 0, "stop while not sounding returns the position to 0");

	// A seek while sounding fades out and in, so the output never jumps
	lts::Engine seeking;
	seeking.replace_track(0, make_track(48000, 1000));
	seeking.play();
	render(seeking, 25);
	seeking.seek(30000);
	const Rendered seek_out = render(seeking, 4);
	check(max_step(seek_out.left, 0, seek_out.left.size()) < 0.02f, "a seek while sounding does not jump");
	check(seeking.position() > 30000 && seeking.position() < 31500, "the seek arrives at the requested position");

	// A seek during the fade out after Pause still moves the position
	seeking.pause();
	render(seeking, 2);
	seeking.seek(10000);
	render(seeking, 25);
	// The position jumps after 10 ms and keeps moving until the 200 ms fade out ends
	check(seeking.position() >= 10000 && seeking.position() <= 10000 + lts::kFadeFrames,
	      "a seek during the pause fade out moves the position");
}

void test_fades_and_clip()
{
	lts::Engine engine;
	engine.replace_track(0, make_track(48000, 16384));
	engine.play();
	const Rendered out = render(engine, 25);
	bool monotonic = true;
	for (size_t i = 1; i < lts::kFadeFrames; i++)
		if (out.right[i] < out.right[i - 1])
			monotonic = false;
	check(monotonic, "fade in rises monotonically");
	check(std::fabs(out.right[lts::kFadeFrames + 10] - 0.5f) < 1e-4f, "fade in is complete after 200 ms");

	lts::Engine loud;
	loud.replace_track(0, make_track(48000, 30000));
	loud.replace_track(1, make_track(48000, 30000));
	loud.play();
	check(render(loud, 30).right.back() == 1.0f, "a sum above full scale is clipped to 1");
}

void test_disabled_track_keeps_position()
{
	lts::Engine engine;
	engine.replace_track(0, make_track(48000, 0));
	engine.replace_track(1, make_track(48000, 2000));
	engine.set_track_enabled(0, false);
	engine.play();
	render(engine, 30);
	engine.set_track_enabled(0, true);
	const Rendered out = render(engine, 30);
	bool ok = true;
	for (size_t i = 10000; i < 10100; i++)
		if (std::fabs(out.left[i] - 2.0f * static_cast<float>((14400 + i) % 10000) * kScale) > 1e-4f)
			ok = false;
	check(ok, "a track enabled later plays from the shared position");
}

void test_loop_end_moves_before_position()
{
	lts::Engine engine;
	engine.replace_track(0, make_track(48000, 1000));
	engine.set_loop(1000, 40000);
	engine.play();
	render(engine, 20); // position 9600, inside the loop
	engine.set_loop(1000, 5000);
	check(engine.position() == 1000, "moving the loop end before the position returns to the loop start");

	engine.set_loop(1000, 40000);
	engine.set_loop_enabled(false);
	render(engine, 90); // into the outro after 40000
	const uint64_t in_outro = engine.position();
	engine.set_loop(2000, 40000);
	check(in_outro > 40000 && engine.position() == in_outro,
	      "changing the loop does not move a position in the outro");
}

void test_seam_crossfade()
{
	// The left channel is a ramp, so the seam from 3999 to 1000 is a large step without a crossfade. The loop wraps
	// at output frames 4000, 7000, and 10000. The third wrap comes after the 200 ms fade in, at full level.
	auto measure = [](bool crossfade, bool loop_on, size_t from, size_t to) {
		lts::Engine engine;
		engine.replace_track(0, make_track(12000, 0));
		engine.set_loop(1000, 4000);
		engine.set_crossfade_enabled(crossfade);
		engine.set_loop_enabled(loop_on);
		engine.play();
		const Rendered out = render(engine, 25);
		return max_step(out.left, from, to) / kScale;
	};
	check(measure(true, true, 9900, 10100) < 30.0f, "the seam crossfade removes the step at the loop point");
	check(measure(false, true, 9900, 10100) > 2900.0f, "without the crossfade, the step at the loop point remains");
	check(measure(true, false, 3900, 4100) < 2.0f, "with the loop off, nothing is blended at the loop end");
}

// --- Out of range and errors ---

void test_short_tracks_never_read_out_of_range()
{
	lts::Engine engine;
	engine.replace_track(0, make_track(4800, 1000));
	engine.replace_track(1, make_track(2000, 2000));
	engine.set_loop(500, 4000);
	check(engine.song_end() == 4800, "the song end is the end of the longest track");
	engine.play();
	render(engine, 25); // loops several times through the end of the short track
	engine.set_loop_enabled(false);
	engine.seek(2500);
	render(engine, 3); // the 10 ms fade out, the jump, and the 10 ms fade in
	const Rendered out = render(engine, 10);
	// From 2500 on, past the end of track 2, only track 1 (right 1000) is heard
	bool silent_after_end = true;
	for (size_t i = 0; i < out.right.size(); i++)
		if (out.right[i] > (1000.0f + 1.0f) * kScale)
			silent_after_end = false;
	check(silent_after_end, "a short track is silent after its own end");
	check(engine.transport() == lts::Transport::Stopped, "the outro with a short track ends at the song end");

	// Replacing a track with a shorter one while past its end
	lts::Engine replace;
	replace.replace_track(0, make_track(9600, 1000));
	replace.set_loop(0, 0);
	replace.play();
	render(replace, 15); // position 7200
	replace.replace_track(0, make_track(4000, 1000));
	check(replace.transport() == lts::Transport::Stopped && replace.position() == 0,
	      "a song shortened below the position counts as reaching the song end");
	render(replace, 4);
}

void test_loop_errors()
{
	lts::Engine engine;
	check(engine.loop_status() == lts::LoopStatus::NoTracks, "no track is an error");
	engine.play();
	render(engine, 1);
	check(engine.transport() == lts::Transport::Stopped, "play without tracks stays stopped");
	engine.replace_track(0, make_track(1000, 0));
	engine.set_loop(800, 600);
	check(engine.loop_status() == lts::LoopStatus::StartNotBeforeEnd, "start after end is an error");
	engine.set_loop(0, 2000);
	check(engine.loop_status() == lts::LoopStatus::EndBeyondSong, "a loop end beyond the song is an error");
	engine.set_loop(0, 0);
	check(engine.loop_status() == lts::LoopStatus::Ok && engine.loop_end() == 1000, "loop end 0 is the song end");
	engine.replace_track(1, make_track(3000, 0));
	check(engine.loop_end() == 3000, "loop end 0 follows the longest track");
}

// --- Loader ---

void write_wav(const fs::path &path, uint16_t format, uint16_t channels, uint32_t rate, uint16_t bits,
	       const std::vector<std::vector<double>> &frames)
{
	std::vector<uint8_t> data;
	for (const auto &frame : frames) {
		for (double v : frame) {
			if (format == 3) {
				const float f = static_cast<float>(v);
				uint8_t bytes[4];
				std::memcpy(bytes, &f, 4);
				data.insert(data.end(), bytes, bytes + 4);
			} else {
				const int64_t full = (int64_t(1) << (bits - 1)) - 1;
				const int64_t q = static_cast<int64_t>(std::llround(v * static_cast<double>(full)));
				for (uint16_t b = 0; b < bits / 8; b++)
					data.push_back(static_cast<uint8_t>((q >> (8 * b)) & 0xff));
			}
		}
	}
	auto u32 = [](std::ofstream &f, uint32_t v) {
		for (int i = 0; i < 4; i++)
			f.put(static_cast<char>((v >> (8 * i)) & 0xff));
	};
	auto u16 = [](std::ofstream &f, uint16_t v) {
		f.put(static_cast<char>(v & 0xff));
		f.put(static_cast<char>(v >> 8));
	};
	std::ofstream f(path, std::ios::binary);
	f.write("RIFF", 4);
	u32(f, static_cast<uint32_t>(36 + data.size()));
	f.write("WAVEfmt ", 8);
	u32(f, 16);
	u16(f, format);
	u16(f, channels);
	u32(f, rate);
	u32(f, rate * channels * bits / 8);
	u16(f, static_cast<uint16_t>(channels * bits / 8));
	u16(f, bits);
	f.write("data", 4);
	u32(f, static_cast<uint32_t>(data.size()));
	f.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
}

lts::LoadResult load(const fs::path &path, const fs::path &bake_dir = {})
{
	static std::atomic<bool> never{false};
	return lts::load_track(path.u8string(), bake_dir.u8string(), never);
}

// Frame of the loudest sample in a channel
uint64_t peak_frame(const lts::TrackData &data, int channel)
{
	uint64_t at = 0;
	int peak = 0;
	for (uint64_t i = 0; i < data.frames; i++) {
		const int v = std::abs(static_cast<int>(data.samples[i * 2 + channel]));
		if (v > peak) {
			peak = v;
			at = i;
		}
	}
	return at;
}

// Amplitude of one frequency component in a frame range (Goertzel)
double tone(const lts::TrackData &data, int channel, double frequency, uint64_t from, uint64_t count)
{
	const double coefficient = 2.0 * std::cos(2.0 * 3.14159265358979323846 * frequency / 48000.0);
	double s1 = 0.0, s2 = 0.0;
	for (uint64_t i = from; i < std::min(from + count, data.frames); i++) {
		const double s0 = data.samples[i * 2 + channel] / 32768.0 + coefficient * s1 - s2;
		s2 = s1;
		s1 = s0;
	}
	return 2.0 * std::sqrt(std::max(s1 * s1 + s2 * s2 - coefficient * s1 * s2, 0.0)) / static_cast<double>(count);
}

void test_loader(const fs::path &fixtures)
{
	const fs::path dir = fs::temp_directory_path() / "lts-engine-test";
	fs::remove_all(dir);
	fs::create_directories(dir);

	// 48 kHz WAV of every sample format is read directly into 16 bits, without a bake
	const std::vector<std::vector<double>> four = {{0.0, 0.0}, {0.5, 0.5}, {-0.5, -0.5}, {0.25, 0.25}};
	struct Case {
		const char *name;
		uint16_t format;
		uint16_t bits;
	} cases[] = {{"pcm16", 1, 16}, {"pcm24", 1, 24}, {"pcm32", 1, 32}, {"float32", 3, 32}};
	for (const auto &c : cases) {
		const fs::path path = dir / (std::string(c.name) + ".wav");
		write_wav(path, c.format, 2, 48000, c.bits, four);
		const lts::LoadResult r = load(path, dir / "bakes");
		const bool loaded = r.data && r.data->frames == 4;
		check(loaded, std::string(c.name) + " loads 4 frames");
		if (loaded) {
			const int16_t half = r.data->samples[2];
			check(half >= 16382 && half <= 16384, std::string(c.name) + " is converted to 16 bits");
		}
		check(r.bake_path.empty(), std::string(c.name) + " at 48 kHz is not baked");
	}

	// Mono is copied to both channels; three or more channels use front left and front right
	write_wav(dir / "mono.wav", 1, 1, 48000, 16, {{0.5}, {0.25}});
	const lts::LoadResult mono = load(dir / "mono.wav");
	check(mono.data && mono.data->samples[0] == mono.data->samples[1] && mono.data->samples[0] > 16000,
	      "mono is copied to both channels");
	write_wav(dir / "six.wav", 1, 6, 48000, 16, {{0.1, 0.2, 0.3, 0.4, 0.5, 0.6}});
	const lts::LoadResult six = load(dir / "six.wav");
	check(six.data && std::abs(six.data->samples[0] - 3277) < 3 && std::abs(six.data->samples[1] - 6553) < 3,
	      "a 6-channel WAV uses its first two channels");

	// Encoded files: the burst at 0.5 s stays at 0.5 s after removing the MP3 delay and resampling 44.1 -> 48 kHz
	for (const char *name : {"tone-44k.mp3", "tone-44k.ogg", "tone-44k.flac"}) {
		const lts::LoadResult r = load(fixtures / name);
		if (!r.data) {
			check(false, std::string(name) + " loads");
			continue;
		}
		check(r.data->frames == 48000, std::string(name) +
						       " has floor(44100 x 48000 / 44100) = 48000 frames (" +
						       std::to_string(r.data->frames) + ")");
		const uint64_t left_peak = peak_frame(*r.data, 0);
		check(left_peak >= 24000 && left_peak < 24000 + 960,
		      std::string(name) + " keeps the burst at 0.5 s (peak at " + std::to_string(left_peak) + ")");
		check(tone(*r.data, 0, 1000.0, 24000, 960) > 0.3 && tone(*r.data, 1, 2000.0, 24000, 960) > 0.3,
		      std::string(name) + " keeps the frequencies and the channel order");
	}
	for (const char *name : {"tone-6ch.ogg", "tone-6ch.flac"}) {
		const lts::LoadResult r = load(fixtures / name);
		check(r.data && tone(*r.data, 0, 1000.0, 24000, 960) > 0.3 &&
			      tone(*r.data, 1, 2000.0, 24000, 960) > 0.3,
		      std::string(name) + " uses front left and front right");
		check(r.info.source_channels == 6, std::string(name) + " reports 6 channels");
	}

	// A path outside ASCII loads and bakes for every format
	for (const char *name : {"tone-44k.mp3", "tone-44k.ogg", "tone-44k.flac"}) {
		const fs::path japanese = dir / fs::u8path(std::string(u8"日本語 ") + name);
		fs::copy_file(fixtures / name, japanese, fs::copy_options::overwrite_existing);
		const lts::LoadResult r = load(japanese, dir / fs::u8path(u8"ベイク"));
		check(r.data != nullptr && !r.bake_path.empty(),
		      std::string("a UTF-8 path outside ASCII loads and bakes ") + name);
	}

	// Bake: written once, reused, rebuilt when cut short or when the source changes
	const fs::path bakes = dir / "bakes";
	const fs::path mp3 = dir / "song.mp3";
	fs::copy_file(fixtures / "tone-44k.mp3", mp3, fs::copy_options::overwrite_existing);
	std::string bake_path;
	{
		const lts::LoadResult first = load(mp3, bakes);
		bake_path = first.bake_path;
		check(first.data && !first.info.from_bake && !bake_path.empty() && fs::exists(fs::u8path(bake_path)),
		      "the first load writes a bake");
	}
	{
		const lts::LoadResult second = load(mp3, bakes);
		check(second.data && second.info.from_bake && second.data->frames == 48000,
		      "the second load reads the bake");
	}
	fs::resize_file(fs::u8path(bake_path), fs::file_size(fs::u8path(bake_path)) - 1000);
	{
		const lts::LoadResult cut = load(mp3, bakes);
		check(cut.data && !cut.info.from_bake && cut.data->frames == 48000, "a bake cut short is rebuilt");
	}
	{
		const lts::LoadResult shared_a = load(mp3, bakes);
		const lts::LoadResult shared_b = load(mp3, bakes);
		check(shared_a.data && shared_a.data == shared_b.data,
		      "two loads of the same file share one copy in memory");
		check(!lts::remove_bake_if_unused(shared_a.bake_path), "a bake in use is not removed");
	}
	fs::last_write_time(mp3, fs::last_write_time(mp3) + std::chrono::hours(1));
	{
		const lts::LoadResult changed = load(mp3, bakes);
		check(changed.data && !changed.info.from_bake && changed.bake_path != bake_path,
		      "a changed source gets a new bake");
	}
	check(lts::remove_bake_if_unused(bake_path), "an unused bake is removed");

	// Errors
	check(load(dir / "missing.wav").error == lts::LoadError::OpenFailed, "a missing file is reported");
	{
		std::ofstream junk(dir / "junk.wav", std::ios::binary);
		junk << "RIFF this is not a wave file at all";
	}
	check(load(dir / "junk.wav").error == lts::LoadError::OpenFailed, "a broken WAV is reported");

	std::error_code ec;
	fs::remove_all(dir, ec);
}

} // namespace

int main(int argc, char **argv)
{
	if (argc != 2) {
		std::fprintf(stderr, "usage: engine-test <folder of tests/data>\n");
		return 1;
	}
	test_loop_wraps();
	test_outro_and_song_end();
	test_stopped_position_and_seek();
	test_fades_and_clip();
	test_disabled_track_keeps_position();
	test_loop_end_moves_before_position();
	test_seam_crossfade();
	test_short_tracks_never_read_out_of_range();
	test_loop_errors();
	test_loader(fs::u8path(argv[1]));
	std::printf("%s engine\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
