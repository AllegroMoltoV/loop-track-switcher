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

#include "wav-loader.hpp"

#include "dr_wav.h"

#include <new>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace lts {

namespace {

bool open_wav(drwav *wav, const std::string &path)
{
#ifdef _WIN32
	// dr_wav's narrow version uses fopen, which does not take UTF-8 paths on Windows
	const int length = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
	if (length <= 0)
		return false;
	std::wstring wide(static_cast<size_t>(length), L'\0');
	if (MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), length) <= 0)
		return false;
	return drwav_init_file_w(wav, wide.c_str(), nullptr) == DRWAV_TRUE;
#else
	return drwav_init_file(wav, path.c_str(), nullptr) == DRWAV_TRUE;
#endif
}

bool supported_format(const drwav &wav)
{
	if (wav.translatedFormatTag == DR_WAVE_FORMAT_PCM)
		return wav.bitsPerSample == 16 || wav.bitsPerSample == 24 || wav.bitsPerSample == 32;
	if (wav.translatedFormatTag == DR_WAVE_FORMAT_IEEE_FLOAT)
		return wav.bitsPerSample == 32;
	return false;
}

} // namespace

WavLoadResult load_wav(const std::string &path)
{
	WavLoadResult result;
	drwav wav;
	if (path.empty() || !open_wav(&wav, path)) {
		result.error = WavError::OpenFailed;
		return result;
	}

	if (wav.channels != 2)
		result.error = WavError::NotStereo;
	else if (wav.sampleRate != kSampleRate)
		result.error = WavError::Not48k;
	else if (!supported_format(wav))
		result.error = WavError::UnsupportedFormat;
	else if (wav.totalPCMFrameCount == 0)
		result.error = WavError::ReadFailed;
	if (result.error != WavError::None) {
		drwav_uninit(&wav);
		return result;
	}

	auto data = std::make_unique<TrackData>();
	try {
		data->samples.resize(static_cast<size_t>(wav.totalPCMFrameCount) * 2);
	} catch (const std::bad_alloc &) {
		drwav_uninit(&wav);
		result.error = WavError::OutOfMemory;
		return result;
	}

	const drwav_uint64 read = drwav_read_pcm_frames_s16(&wav, wav.totalPCMFrameCount, data->samples.data());
	drwav_uninit(&wav);
	if (read == 0) {
		result.error = WavError::ReadFailed;
		return result;
	}
	// A truncated file keeps what could be read
	data->frames = read;
	data->samples.resize(static_cast<size_t>(read) * 2);
	data->samples.shrink_to_fit();
	result.data = std::move(data);
	return result;
}

} // namespace lts
