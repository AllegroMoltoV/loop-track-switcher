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

#include "track-loader.hpp"

#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#include "CDSPResampler.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace lts {

namespace {

constexpr size_t kChunkFrames = 4096;
constexpr uint32_t kBakeVersion = 1;
constexpr char kBakeMagic[8] = {'L', 'T', 'S', 'B', 'A', 'K', 'E', '\0'};
constexpr const char *kBakeExtension = ".ltsbake";
constexpr const char *kTempExtension = ".ltsbake.tmp";

fs::path to_path(const std::string &utf8)
{
	return fs::u8path(utf8);
}

FILE *open_file(const fs::path &path, const char *mode)
{
#ifdef _WIN32
	std::wstring wide_mode(mode, mode + std::strlen(mode));
	return _wfopen(path.c_str(), wide_mode.c_str());
#else
	return std::fopen(path.c_str(), mode);
#endif
}

struct SourceStat {
	uint64_t size = 0;
	int64_t mtime = 0;
};

bool stat_source(const fs::path &path, SourceStat &stat)
{
	std::error_code ec;
	stat.size = fs::file_size(path, ec);
	if (ec)
		return false;
	const auto time = fs::last_write_time(path, ec);
	if (ec)
		return false;
	stat.mtime = static_cast<int64_t>(time.time_since_epoch().count());
	return true;
}

uint64_t fnv1a(uint64_t hash, const void *data, size_t size)
{
	const auto *bytes = static_cast<const uint8_t *>(data);
	for (size_t i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

std::string bake_name(const std::string &path, const SourceStat &stat)
{
	uint64_t hash = 14695981039346656037ull;
	hash = fnv1a(hash, path.data(), path.size());
	hash = fnv1a(hash, &stat.size, sizeof(stat.size));
	hash = fnv1a(hash, &stat.mtime, sizeof(stat.mtime));
	hash = fnv1a(hash, &kBakeVersion, sizeof(kBakeVersion));
	char name[32];
	std::snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hash));
	return name;
}

// Tracks loaded by any source, so that sources using the same file share one copy in memory
struct SharedEntry {
	std::weak_ptr<const TrackData> data;
	LoadInfo info;
	std::string bake_path;
};
std::mutex registry_mutex;
std::unordered_map<std::string, SharedEntry> registry;

std::string registry_key(const std::string &path, const SourceStat &stat)
{
	return path + "\n" + std::to_string(stat.size) + "\n" + std::to_string(stat.mtime);
}

// Picks the front left and front right channels. Vorbis orders three or more channels as left, center, right
// (except four channels, which are front left, front right, rear left, rear right). WAV, FLAC, and MP3 put front
// left and front right first.
void pick_channels(uint32_t channels, bool vorbis_order, uint32_t &left, uint32_t &right)
{
	left = 0;
	if (channels <= 1)
		right = 0;
	else if (vorbis_order && channels >= 3 && channels != 4)
		right = 2;
	else
		right = 1;
}

inline int16_t to_s16(double value)
{
	const double scaled = std::round(value * 32767.0);
	return static_cast<int16_t>(std::clamp(scaled, -32768.0, 32767.0));
}

// A decoder that delivers interleaved float frames
class Decoder {
public:
	virtual ~Decoder() = default;
	virtual uint64_t read(float *out, uint64_t frames) = 0;
	uint32_t rate = 0;
	uint32_t channels = 0;
	// 0 when the length is not known in advance
	uint64_t total_frames = 0;
	bool vorbis_order = false;
};

class WavDecoder : public Decoder {
public:
	bool open(const fs::path &path)
	{
#ifdef _WIN32
		ok_ = drwav_init_file_w(&wav_, path.c_str(), nullptr) == DRWAV_TRUE;
#else
		ok_ = drwav_init_file(&wav_, path.c_str(), nullptr) == DRWAV_TRUE;
#endif
		if (ok_) {
			rate = wav_.sampleRate;
			channels = wav_.channels;
			total_frames = wav_.totalPCMFrameCount;
		}
		return ok_;
	}
	~WavDecoder() override
	{
		if (ok_)
			drwav_uninit(&wav_);
	}
	uint64_t read(float *out, uint64_t frames) override { return drwav_read_pcm_frames_f32(&wav_, frames, out); }
	uint64_t read_s16(int16_t *out, uint64_t frames) { return drwav_read_pcm_frames_s16(&wav_, frames, out); }

private:
	drwav wav_ = {};
	bool ok_ = false;
};

class FlacDecoder : public Decoder {
public:
	bool open(const fs::path &path)
	{
#ifdef _WIN32
		flac_ = drflac_open_file_w(path.c_str(), nullptr);
#else
		flac_ = drflac_open_file(path.c_str(), nullptr);
#endif
		if (flac_) {
			rate = flac_->sampleRate;
			channels = flac_->channels;
			total_frames = flac_->totalPCMFrameCount;
		}
		return flac_ != nullptr;
	}
	~FlacDecoder() override
	{
		if (flac_)
			drflac_close(flac_);
	}
	uint64_t read(float *out, uint64_t frames) override { return drflac_read_pcm_frames_f32(flac_, frames, out); }

private:
	drflac *flac_ = nullptr;
};

class Mp3Decoder : public Decoder {
public:
	bool open(const fs::path &path)
	{
#ifdef _WIN32
		ok_ = drmp3_init_file_w(&mp3_, path.c_str(), nullptr) == DRMP3_TRUE;
#else
		ok_ = drmp3_init_file(&mp3_, path.c_str(), nullptr) == DRMP3_TRUE;
#endif
		if (ok_) {
			rate = mp3_.sampleRate;
			channels = mp3_.channels;
			// Scans the frame headers and returns to the start. The delay and padding of the encoder are
			// removed by dr_mp3 while reading.
			total_frames = drmp3_get_pcm_frame_count(&mp3_);
		}
		return ok_;
	}
	~Mp3Decoder() override
	{
		if (ok_)
			drmp3_uninit(&mp3_);
	}
	uint64_t read(float *out, uint64_t frames) override { return drmp3_read_pcm_frames_f32(&mp3_, frames, out); }

private:
	drmp3 mp3_ = {};
	bool ok_ = false;
};

class VorbisDecoder : public Decoder {
public:
	bool open(const fs::path &path)
	{
		FILE *file = open_file(path, "rb");
		if (!file)
			return false;
		int error = 0;
		vorbis_ = stb_vorbis_open_file(file, 1, &error, nullptr);
		if (!vorbis_) {
			std::fclose(file);
			return false;
		}
		const stb_vorbis_info info = stb_vorbis_get_info(vorbis_);
		rate = info.sample_rate;
		channels = static_cast<uint32_t>(info.channels);
		total_frames = stb_vorbis_stream_length_in_samples(vorbis_);
		vorbis_order = true;
		return true;
	}
	~VorbisDecoder() override
	{
		if (vorbis_)
			stb_vorbis_close(vorbis_);
	}
	uint64_t read(float *out, uint64_t frames) override
	{
		const int got = stb_vorbis_get_samples_float_interleaved(vorbis_, static_cast<int>(channels), out,
									 static_cast<int>(frames * channels));
		return got > 0 ? static_cast<uint64_t>(got) : 0;
	}

private:
	stb_vorbis *vorbis_ = nullptr;
};

enum class Format { Unknown, Wav, Flac, Vorbis };

Format sniff(const fs::path &path)
{
	FILE *file = open_file(path, "rb");
	if (!file)
		return Format::Unknown;
	char head[4] = {};
	const size_t got = std::fread(head, 1, sizeof(head), file);
	std::fclose(file);
	if (got < 4)
		return Format::Unknown;
	if (!std::memcmp(head, "RIFF", 4) || !std::memcmp(head, "RIFX", 4) || !std::memcmp(head, "RF64", 4) ||
	    !std::memcmp(head, "riff", 4))
		return Format::Wav;
	if (!std::memcmp(head, "fLaC", 4))
		return Format::Flac;
	if (!std::memcmp(head, "OggS", 4))
		return Format::Vorbis;
	return Format::Unknown;
}

std::unique_ptr<Decoder> open_decoder(const fs::path &path, Format format)
{
	switch (format) {
	case Format::Wav: {
		auto d = std::make_unique<WavDecoder>();
		return d->open(path) ? std::move(d) : nullptr;
	}
	case Format::Flac: {
		auto d = std::make_unique<FlacDecoder>();
		return d->open(path) ? std::move(d) : nullptr;
	}
	case Format::Vorbis: {
		auto d = std::make_unique<VorbisDecoder>();
		return d->open(path) ? std::move(d) : nullptr;
	}
	default: {
		// Anything else is tried as MP3, which may start with an ID3 tag or directly with a frame
		auto d = std::make_unique<Mp3Decoder>();
		return d->open(path) ? std::move(d) : nullptr;
	}
	}
}

// Bake file: header, the UTF-8 source path, then interleaved 16-bit stereo samples
struct BakeHeader {
	char magic[8];
	uint32_t version;
	uint32_t source_rate;
	uint32_t source_channels;
	uint32_t path_length;
	uint64_t source_size;
	int64_t source_mtime;
	uint64_t frames;
	uint64_t full_frames;
};

bool read_bake(const fs::path &bake, const std::string &path, const SourceStat &stat, LoadResult &result)
{
	FILE *file = open_file(bake, "rb");
	if (!file)
		return false;
	BakeHeader header = {};
	bool ok = std::fread(&header, sizeof(header), 1, file) == 1 && !std::memcmp(header.magic, kBakeMagic, 8) &&
		  header.version == kBakeVersion && header.source_size == stat.size &&
		  header.source_mtime == stat.mtime && header.path_length == path.size() &&
		  header.frames <= kMaxTrackFrames;
	if (ok) {
		std::string stored(header.path_length, '\0');
		ok = std::fread(stored.data(), 1, stored.size(), file) == stored.size() && stored == path;
	}
	if (ok) {
		// A bake cut short by a crash has fewer samples than its header says
		std::error_code ec;
		const uint64_t expected = sizeof(header) + header.path_length + header.frames * 4;
		ok = fs::file_size(bake, ec) == expected && !ec;
	}
	if (ok) {
		try {
			auto data = std::make_shared<TrackData>();
			data->frames = header.frames;
			data->samples.resize(static_cast<size_t>(header.frames) * 2);
			ok = std::fread(data->samples.data(), 4, static_cast<size_t>(header.frames), file) ==
			     header.frames;
			if (ok) {
				result.data = std::move(data);
				result.info.source_rate = header.source_rate;
				result.info.source_channels = header.source_channels;
				result.info.full_frames = header.full_frames;
				result.info.trimmed = header.full_frames > header.frames;
				result.info.from_bake = true;
			}
		} catch (const std::bad_alloc &) {
			ok = false;
			result.error = LoadError::OutOfMemory;
		}
	}
	std::fclose(file);
	if (ok) {
		// The modification time of a bake records its last use, for clean_old_bakes
		std::error_code ec;
		fs::last_write_time(bake, fs::file_time_type::clock::now(), ec);
	}
	return ok;
}

// Collects the converted samples into a bake file when possible, and into memory otherwise
class Sink {
public:
	Sink(const fs::path &temp, const BakeHeader &header, const std::string &path) : temp_(temp), header_(header)
	{
		if (temp_.empty())
			return;
		file_ = open_file(temp_, "wb");
		if (file_ && (std::fwrite(&header_, sizeof(header_), 1, file_) != 1 ||
			      std::fwrite(path.data(), 1, path.size(), file_) != path.size()))
			fail();
	}
	~Sink()
	{
		if (file_) {
			std::fclose(file_);
			std::error_code ec;
			fs::remove(temp_, ec);
		}
	}
	bool to_file() const { return file_ != nullptr; }
	void reserve(uint64_t frames)
	{
		if (!file_)
			memory_.reserve(static_cast<size_t>(frames) * 2);
	}
	void add(const int16_t *frames, size_t count)
	{
		if (file_) {
			if (std::fwrite(frames, 4, count, file_) != count)
				fail();
		} else {
			memory_.insert(memory_.end(), frames, frames + count * 2);
		}
		frames_ += count;
	}
	uint64_t frames() const { return frames_; }
	std::vector<int16_t> take_memory() { return std::move(memory_); }
	// Writes the final header and renames the temporary file. Returns false when the file could not be finished.
	// Writes the final header and renames the temporary file to the bake. Returns false when the bake could not be
	// finished. When only the rename failed, the complete temporary file is kept for the caller to read.
	bool finish(const fs::path &bake, uint64_t full_frames, std::string &detail)
	{
		if (!file_)
			return false;
		header_.frames = frames_;
		header_.full_frames = full_frames;
		bool ok = std::fseek(file_, 0, SEEK_SET) == 0 && std::fwrite(&header_, sizeof(header_), 1, file_) == 1;
		ok = std::fclose(file_) == 0 && ok;
		file_ = nullptr;
		std::error_code ec;
		if (!ok) {
			detail = "writing the bake header failed";
			fs::remove(temp_, ec);
			return false;
		}
		// Another program, such as a virus scanner, may hold the new file open for a moment. The rename is retried.
		for (int attempt = 0; attempt < 20; attempt++) {
			fs::rename(temp_, bake, ec);
			if (!ec)
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		detail = "renaming the bake failed: " + ec.message();
		temp_kept_ = true;
		return false;
	}
	bool temp_kept() const { return temp_kept_; }
	const fs::path &temp() const { return temp_; }

private:
	void fail()
	{
		// The disk is full or not writable. The samples written so far are moved to memory.
		std::fclose(file_);
		file_ = nullptr;
		std::error_code ec;
		fs::remove(temp_, ec);
		if (frames_ > 0)
			lost_ = true;
	}

	fs::path temp_;
	BakeHeader header_;
	FILE *file_ = nullptr;
	std::vector<int16_t> memory_;
	uint64_t frames_ = 0;
	bool temp_kept_ = false;

public:
	// True when samples were lost because writing failed after some were written
	bool lost_ = false;
};

// Reads a 48 kHz WAV directly into 16 bits, without float conversion and without a bake
LoadResult load_wav_direct(WavDecoder &wav)
{
	LoadResult result;
	const uint64_t frames = std::min<uint64_t>(wav.total_frames, kMaxTrackFrames);
	uint32_t left = 0, right = 0;
	pick_channels(wav.channels, false, left, right);
	try {
		auto data = std::make_shared<TrackData>();
		data->samples.resize(static_cast<size_t>(frames) * 2);
		std::vector<int16_t> chunk(kChunkFrames * wav.channels);
		uint64_t done = 0;
		while (done < frames) {
			const uint64_t want = std::min<uint64_t>(kChunkFrames, frames - done);
			const uint64_t got = wav.read_s16(chunk.data(), want);
			if (got == 0)
				break;
			for (uint64_t i = 0; i < got; i++) {
				data->samples[(done + i) * 2] = chunk[i * wav.channels + left];
				data->samples[(done + i) * 2 + 1] = chunk[i * wav.channels + right];
			}
			done += got;
		}
		if (done == 0) {
			result.error = LoadError::ReadFailed;
			return result;
		}
		data->frames = done;
		data->samples.resize(static_cast<size_t>(done) * 2);
		result.data = std::move(data);
		result.info.full_frames = std::max(wav.total_frames, done);
		result.info.trimmed = wav.total_frames > kMaxTrackFrames;
	} catch (const std::bad_alloc &) {
		result.error = LoadError::OutOfMemory;
	}
	return result;
}

// Decodes, resamples to 48 kHz, and converts to 16-bit stereo in chunks
LoadResult convert(Decoder &decoder, const std::string &path, const SourceStat &stat, const fs::path &bake,
		   const std::atomic<bool> &cancel)
{
	LoadResult result;
	const bool resample = decoder.rate != kSampleRate;
	uint32_t left = 0, right = 0;
	pick_channels(decoder.channels, decoder.vorbis_order, left, right);

	// Output length: floor(input frames x 48000 / source rate), cut at kMaxTrackFrames
	auto out_length = [&decoder](uint64_t in_frames) {
		return in_frames * kSampleRate / decoder.rate;
	};

	BakeHeader header = {};
	std::memcpy(header.magic, kBakeMagic, 8);
	header.version = kBakeVersion;
	header.source_rate = decoder.rate;
	header.source_channels = decoder.channels;
	header.path_length = static_cast<uint32_t>(path.size());
	header.source_size = stat.size;
	header.source_mtime = stat.mtime;

	fs::path temp;
	if (!bake.empty()) {
		temp = bake;
		temp.replace_extension(kTempExtension);
	}

	try {
		Sink sink(temp, header, path);
		const uint64_t expected = decoder.total_frames ? out_length(decoder.total_frames) : 0;
		sink.reserve(std::min<uint64_t>(expected, kMaxTrackFrames));

		std::unique_ptr<r8b::CDSPResampler24> resampler_left;
		std::unique_ptr<r8b::CDSPResampler24> resampler_right;
		if (resample) {
			resampler_left = std::make_unique<r8b::CDSPResampler24>(decoder.rate, kSampleRate,
										static_cast<int>(kChunkFrames));
			resampler_right = std::make_unique<r8b::CDSPResampler24>(decoder.rate, kSampleRate,
										 static_cast<int>(kChunkFrames));
		}

		std::vector<float> input(kChunkFrames * std::max<uint32_t>(decoder.channels, 1));
		std::vector<double> in_left(kChunkFrames), in_right(kChunkFrames);
		std::vector<int16_t> out(kChunkFrames * 8);
		uint64_t in_frames = 0;
		uint64_t limit = kMaxTrackFrames;
		bool more_input = true;

		auto emit = [&](const double *l, const double *r, size_t count) {
			const uint64_t room = limit - std::min(limit, sink.frames());
			count = static_cast<size_t>(std::min<uint64_t>(count, room));
			if (out.size() < count * 2)
				out.resize(count * 2);
			for (size_t i = 0; i < count; i++) {
				out[i * 2] = to_s16(l[i]);
				out[i * 2 + 1] = to_s16(r[i]);
			}
			sink.add(out.data(), count);
		};

		while (more_input && sink.frames() < limit) {
			if (cancel.load()) {
				result.error = LoadError::Cancelled;
				return result;
			}
			const uint64_t got = decoder.read(input.data(), kChunkFrames);
			if (got == 0) {
				more_input = false;
				break;
			}
			in_frames += got;
			for (uint64_t i = 0; i < got; i++) {
				in_left[i] = input[i * decoder.channels + left];
				in_right[i] = input[i * decoder.channels + right];
			}
			if (resample) {
				double *ol = nullptr;
				double *or_ = nullptr;
				const int nl = resampler_left->process(in_left.data(), static_cast<int>(got), ol);
				const int nr = resampler_right->process(in_right.data(), static_cast<int>(got), or_);
				emit(ol, or_, static_cast<size_t>(std::min(nl, nr)));
			} else {
				emit(in_left.data(), in_right.data(), static_cast<size_t>(got));
			}
		}

		if (in_frames == 0) {
			result.error = LoadError::ReadFailed;
			return result;
		}

		uint64_t full = out_length(in_frames);
		if (more_input) {
			// Cut at the limit. The full length comes from the decoder when it knows it.
			full = std::max(full, expected);
		} else {
			limit = std::min(limit, full);
			// Feeds zeros until the resampler has delivered the whole output length
			std::fill(in_left.begin(), in_left.end(), 0.0);
			std::fill(in_right.begin(), in_right.end(), 0.0);
			while (resample && sink.frames() < limit) {
				if (cancel.load()) {
					result.error = LoadError::Cancelled;
					return result;
				}
				double *ol = nullptr;
				double *or_ = nullptr;
				const int nl =
					resampler_left->process(in_left.data(), static_cast<int>(kChunkFrames), ol);
				const int nr =
					resampler_right->process(in_right.data(), static_cast<int>(kChunkFrames), or_);
				emit(ol, or_, static_cast<size_t>(std::min(nl, nr)));
			}
		}

		result.info.source_rate = decoder.rate;
		result.info.source_channels = decoder.channels;
		result.info.full_frames = full;
		result.info.trimmed = full > sink.frames();

		if (sink.to_file()) {
			LoadResult baked;
			if (sink.finish(bake, full, result.detail)) {
				if (read_bake(bake, path, stat, baked)) {
					baked.info.from_bake = false;
					baked.bake_path = bake.u8string();
					return baked;
				}
				result.detail = "reading back the bake failed";
			} else if (sink.temp_kept()) {
				// The bake could not be put in place. The samples are read from the temporary file, which is
				// deleted afterwards, so playback goes on without a bake.
				const bool read = read_bake(sink.temp(), path, stat, baked);
				std::error_code ec;
				fs::remove(sink.temp(), ec);
				if (read) {
					baked.info.from_bake = false;
					baked.detail = result.detail;
					return baked;
				}
			}
			result.error = LoadError::ReadFailed;
			result.retry_without_bake = true;
			return result;
		}
		if (sink.lost_) {
			result.detail = "writing the bake failed after some samples";
			result.error = LoadError::ReadFailed;
			result.retry_without_bake = true;
			return result;
		}
		auto data = std::make_shared<TrackData>();
		data->samples = sink.take_memory();
		data->frames = data->samples.size() / 2;
		result.data = std::move(data);
	} catch (const std::bad_alloc &) {
		result = LoadResult();
		result.error = LoadError::OutOfMemory;
	}
	return result;
}

} // namespace

LoadResult load_track(const std::string &path, const std::string &bake_dir, const std::atomic<bool> &cancel)
{
	LoadResult result;
	const fs::path source = to_path(path);
	SourceStat stat;
	if (path.empty() || !stat_source(source, stat)) {
		result.error = LoadError::OpenFailed;
		return result;
	}

	const std::string key = registry_key(path, stat);
	{
		std::lock_guard<std::mutex> lock(registry_mutex);
		auto it = registry.find(key);
		if (it != registry.end()) {
			if (auto shared = it->second.data.lock()) {
				result.data = std::move(shared);
				result.info = it->second.info;
				result.bake_path = it->second.bake_path;
				return result;
			}
		}
	}

	const Format format = sniff(source);
	std::unique_ptr<Decoder> decoder = open_decoder(source, format);
	if (!decoder || decoder->rate == 0 || decoder->channels == 0) {
		result.error = format == Format::Unknown ? LoadError::UnsupportedFormat : LoadError::OpenFailed;
		return result;
	}
	result.info.source_rate = decoder->rate;
	result.info.source_channels = decoder->channels;

	if (format == Format::Wav && decoder->rate == kSampleRate) {
		LoadResult direct = load_wav_direct(static_cast<WavDecoder &>(*decoder));
		direct.info.source_rate = decoder->rate;
		direct.info.source_channels = decoder->channels;
		result = std::move(direct);
	} else {
		fs::path bake;
		if (!bake_dir.empty()) {
			std::error_code ec;
			fs::create_directories(to_path(bake_dir), ec);
			bake = to_path(bake_dir) / (bake_name(path, stat) + kBakeExtension);
			LoadResult baked;
			if (read_bake(bake, path, stat, baked)) {
				baked.bake_path = bake.u8string();
				result = std::move(baked);
			} else if (baked.error == LoadError::OutOfMemory) {
				result.error = LoadError::OutOfMemory;
				return result;
			}
		}
		if (!result.data)
			result = convert(*decoder, path, stat, bake, cancel);
		if (result.retry_without_bake) {
			// The bake failed. The file is decoded again into memory only, so that it still plays.
			const std::string detail = result.detail;
			std::unique_ptr<Decoder> again = open_decoder(source, format);
			if (again) {
				result = convert(*again, path, stat, fs::path(), cancel);
				result.detail = detail + "; loaded without a bake";
			}
		}
	}

	if (result.data) {
		std::lock_guard<std::mutex> lock(registry_mutex);
		registry[key] = {result.data, result.info, result.bake_path};
	}
	return result;
}

bool remove_bake_if_unused(const std::string &bake_path)
{
	if (bake_path.empty())
		return false;
	{
		std::lock_guard<std::mutex> lock(registry_mutex);
		for (auto it = registry.begin(); it != registry.end();) {
			if (it->second.data.expired()) {
				it = registry.erase(it);
				continue;
			}
			if (it->second.bake_path == bake_path)
				return false;
			++it;
		}
	}
	std::error_code ec;
	return fs::remove(to_path(bake_path), ec);
}

void clean_old_bakes(const std::string &bake_dir, int max_age_days)
{
	std::error_code ec;
	const fs::path dir = to_path(bake_dir);
	if (bake_dir.empty() || !fs::is_directory(dir, ec))
		return;
	const auto now = fs::file_time_type::clock::now();
	const auto max_age = std::chrono::hours(24) * max_age_days;
	for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
		const fs::path file = it->path();
		const std::string name = file.filename().u8string();
		const bool bake =
			name.size() > std::strlen(kBakeExtension) &&
			name.compare(name.size() - std::strlen(kBakeExtension), std::string::npos, kBakeExtension) == 0;
		const bool temp =
			name.size() > std::strlen(kTempExtension) &&
			name.compare(name.size() - std::strlen(kTempExtension), std::string::npos, kTempExtension) == 0;
		if (!bake && !temp)
			continue;
		std::error_code time_ec;
		const auto modified = fs::last_write_time(file, time_ec);
		if (time_ec)
			continue;
		const auto age = now - modified;
		if ((bake && age > max_age) || (temp && age > std::chrono::hours(24))) {
			std::error_code remove_ec;
			fs::remove(file, remove_ec);
		}
	}
}

} // namespace lts
