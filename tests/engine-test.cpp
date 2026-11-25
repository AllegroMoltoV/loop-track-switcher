// Unit test of the track engine and the WAV loader. Uses neither OBS nor Qt.
// Exit codes: 0 on pass, 1 on a failed check.

#include "track-engine.hpp"
#include "wav-loader.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string &message)
{
	std::printf("%s: %s\n", condition ? "ok" : "FAILED", message.c_str());
	if (!condition)
		failures++;
}

// A track whose left channel holds the frame index modulo 10000 and whose right channel holds a constant
std::unique_ptr<lts::TrackData> make_track(uint64_t frames, int16_t right_value)
{
	auto track = std::make_unique<lts::TrackData>();
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

void test_loop_wraps()
{
	lts::Engine engine;
	engine.replace_track(0, make_track(4800, 1000));
	engine.set_loop(1000, 3000);
	check(engine.loop_status() == lts::LoopStatus::Ok, "loop 1000-3000 is valid");
	engine.play();
	const Rendered out = render(engine, 40);
	bool in_range = true;
	bool wrapped = false;
	for (uint64_t pos : out.position_after_chunk) {
		if (pos >= 3000)
			in_range = false;
		if (pos >= 1000 && pos < 1480)
			wrapped = true;
	}
	check(in_range, "position stays below the loop end");
	check(wrapped, "position returns to the loop start");
	// After the fade in, the left channel follows the frame index: 2999 is followed by 1000
	bool found_jump = false;
	for (size_t i = 12000; i + 1 < out.left.size(); i++) {
		const float a = out.left[i] / kScale;
		const float b = out.left[i + 1] / kScale;
		if (std::fabs(a - 2999.0f) < 0.5f && std::fabs(b - 1000.0f) < 0.5f)
			found_jump = true;
	}
	check(found_jump, "sample 2999 is followed by sample 1000");
}

void test_disabled_track_keeps_position()
{
	lts::Engine engine;
	engine.replace_track(0, make_track(48000, 0));
	engine.replace_track(1, make_track(48000, 2000));
	engine.set_track_enabled(0, false);
	engine.set_track_enabled(1, true);
	engine.play();
	render(engine, 30); // 14,400 frames, past the 9,600-frame fade in
	engine.set_track_enabled(0, true);
	const Rendered out = render(engine, 30);
	// Track 0 is mixed in from the shared position. Its left value equals the frame index, as does track 1's.
	// After the fade, the left output is twice the index at the current position.
	const uint64_t start = 14400;
	bool ok = true;
	for (size_t i = 10000; i < 10100; i++) {
		const float expected = 2.0f * static_cast<float>((start + i) % 10000) * kScale;
		if (std::fabs(out.left[i] - expected) > 1e-4f)
			ok = false;
	}
	check(ok, "a track enabled later plays from the shared position");
}

void test_fade_in_length_and_monotonic()
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
	check(out.right[lts::kFadeFrames / 2] > 0.2f && out.right[lts::kFadeFrames / 2] < 0.3f,
	      "fade in is about half way after 100 ms");
	check(std::fabs(out.right[lts::kFadeFrames + 10] - 0.5f) < 1e-4f, "fade in is complete after 200 ms");

	engine.pause();
	const Rendered paused = render(engine, 25);
	check(std::fabs(paused.right[lts::kFadeFrames + 10]) < 1e-6f, "pause fades out within 200 ms");
	const uint64_t frozen = engine.position();
	render(engine, 5);
	check(engine.position() == frozen && frozen != 0, "pause keeps the position");

	engine.play();
	render(engine, 25);
	engine.stop();
	render(engine, 25);
	check(engine.position() == 0, "stop returns the position to 0 after the fade out");
}

void test_clip()
{
	lts::Engine engine;
	engine.replace_track(0, make_track(48000, 30000));
	engine.replace_track(1, make_track(48000, 30000));
	engine.play();
	const Rendered out = render(engine, 30);
	check(out.right.back() == 1.0f, "a sum above full scale is clipped to 1");
}

void test_short_track_replacement()
{
	{
		lts::Engine engine;
		engine.replace_track(0, make_track(4800, 1000));
		engine.set_loop(0, 4000);
		engine.play();
		render(engine, 8); // position 3840
		engine.replace_track(0, make_track(2000, 1000));
		check(engine.loop_status() == lts::LoopStatus::EndBeyondTrack,
		      "an explicit loop end beyond a new shorter track is reported");
		check(engine.transport() == lts::Transport::Stopped, "playback stops when the loop becomes invalid");
		const Rendered out = render(engine, 4);
		bool silent = true;
		for (float s : out.left)
			if (s != 0.0f)
				silent = false;
		check(silent, "nothing is rendered after the loop became invalid");
	}
	{
		lts::Engine engine;
		engine.replace_track(0, make_track(4800, 1000));
		engine.set_loop(500, 0);
		engine.play();
		render(engine, 8); // position 3840
		engine.replace_track(0, make_track(2000, 1000));
		check(engine.loop_status() == lts::LoopStatus::Ok && engine.loop_end() == 2000,
		      "loop end 0 follows the new shortest track");
		check(engine.position() == 500, "a position beyond the new loop end returns to the loop start");
		const Rendered out = render(engine, 10);
		bool in_range = true;
		for (uint64_t pos : out.position_after_chunk)
			if (pos >= 2000)
				in_range = false;
		check(in_range, "position stays inside the new shorter track");
	}
}

void test_invalid_loops()
{
	lts::Engine engine;
	check(engine.loop_status() == lts::LoopStatus::NoTracks, "no track is reported");
	engine.play();
	render(engine, 1);
	check(engine.transport() == lts::Transport::Stopped, "play without tracks stays stopped");
	engine.replace_track(0, make_track(1000, 0));
	engine.set_loop(800, 600);
	check(engine.loop_status() == lts::LoopStatus::StartNotBeforeEnd, "start after end is reported");
	engine.set_loop(0, 0);
	check(engine.loop_status() == lts::LoopStatus::Ok && engine.loop_end() == 1000,
	      "loop end 0 means the end of the track");
}

// Writes a WAV file with the given format. Samples are given as values in [-1, 1].
void write_wav(const std::filesystem::path &path, uint16_t format, uint16_t channels, uint32_t rate, uint16_t bits,
	       const std::vector<double> &values)
{
	std::vector<uint8_t> data;
	for (double v : values) {
		for (uint16_t c = 0; c < channels; c++) {
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

void test_wav_loader()
{
	const auto dir = std::filesystem::temp_directory_path() / "lts-engine-test";
	std::filesystem::create_directories(dir);
	const std::vector<double> values = {0.0, 0.5, -0.5, 0.25};

	struct Case {
		const char *name;
		uint16_t format;
		uint16_t bits;
	} cases[] = {{"pcm16", 1, 16}, {"pcm24", 1, 24}, {"pcm32", 1, 32}, {"float32", 3, 32}};
	for (const auto &c : cases) {
		const auto path = dir / (std::string(c.name) + ".wav");
		write_wav(path, c.format, 2, 48000, c.bits, values);
		const lts::WavLoadResult result = lts::load_wav(path.u8string());
		const bool loaded = result.data && result.data->frames == values.size();
		check(loaded, std::string(c.name) + " loads 4 frames");
		if (loaded) {
			// 0.5 becomes about 16383 in 16 bits; the exact rounding depends on the source format
			const int16_t half = result.data->samples[2];
			check(half >= 16382 && half <= 16384 && result.data->samples[3] == half,
			      std::string(c.name) + " is converted to 16 bits (0.5 -> " + std::to_string(half) + ")");
		}
	}

	write_wav(dir / "mono.wav", 1, 1, 48000, 16, values);
	check(lts::load_wav((dir / "mono.wav").u8string()).error == lts::WavError::NotStereo, "mono is rejected");
	write_wav(dir / "44k.wav", 1, 2, 44100, 16, values);
	check(lts::load_wav((dir / "44k.wav").u8string()).error == lts::WavError::Not48k, "44.1 kHz is rejected");
	write_wav(dir / "pcm8.wav", 1, 2, 48000, 8, values);
	check(lts::load_wav((dir / "pcm8.wav").u8string()).error == lts::WavError::UnsupportedFormat,
	      "8-bit PCM is rejected");
	check(lts::load_wav((dir / "missing.wav").u8string()).error == lts::WavError::OpenFailed,
	      "a missing file is reported");

	// A file name outside ASCII, as Japanese users often have
	const auto japanese = dir / std::filesystem::u8path(u8"トラック.wav");
	write_wav(japanese, 1, 2, 48000, 16, values);
	check(lts::load_wav(japanese.u8string()).data != nullptr, "a UTF-8 path outside ASCII loads");

	std::filesystem::remove_all(dir);
}

} // namespace

int main()
{
	test_loop_wraps();
	test_disabled_track_keeps_position();
	test_fade_in_length_and_monotonic();
	test_clip();
	test_short_track_replacement();
	test_invalid_loops();
	test_wav_loader();
	std::printf("%s engine\n", failures == 0 ? "PASS" : "FAIL");
	return failures == 0 ? 0 : 1;
}
