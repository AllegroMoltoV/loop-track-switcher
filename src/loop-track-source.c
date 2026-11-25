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

#include "loop-track-source.h"

#include <plugin-support.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/util_uint64.h>

#include <errno.h>
#include <math.h>
#include <stdint.h>

// The source always generates 48 kHz stereo. OBS resamples it when its own output uses another sample rate or
// speaker layout. Each chunk is 10 ms long, and a chunk for [t, t + 10 ms) is sent at time t.
#define SAMPLE_RATE 48000
#define CHUNK_FRAMES 480
#define CHANNELS 2

struct loop_track {
	obs_source_t *source;
	pthread_t thread;
	os_event_t *stop_event;
	bool thread_created;
};

// Placeholder signal for the scaffold: a 440 Hz sine at -20 dB. The track engine replaces it.
static void *generator_thread(void *data)
{
	struct loop_track *lt = data;
	float samples[CHANNELS][CHUNK_FRAMES];
	uint64_t start_ns = os_gettime_ns();
	uint64_t frames_sent = 0;
	const double step = 2.0 * 3.14159265358979323846 * 440.0 / SAMPLE_RATE;
	const float amplitude = 0.1f;

	os_set_thread_name("loop-track-switcher");

	while (os_event_try(lt->stop_event) == EAGAIN) {
		for (size_t i = 0; i < CHUNK_FRAMES; i++) {
			const float s = amplitude * (float)sin(step * (double)((frames_sent + i) % SAMPLE_RATE));
			samples[0][i] = s;
			samples[1][i] = s;
		}

		struct obs_source_audio audio = {0};
		for (size_t ch = 0; ch < CHANNELS; ch++)
			audio.data[ch] = (const uint8_t *)samples[ch];
		audio.frames = CHUNK_FRAMES;
		audio.speakers = SPEAKERS_STEREO;
		audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
		audio.samples_per_sec = SAMPLE_RATE;
		audio.timestamp = start_ns + util_mul_div64(frames_sent, 1000000000ULL, SAMPLE_RATE);
		obs_source_output_audio(lt->source, &audio);

		frames_sent += CHUNK_FRAMES;
		os_sleepto_ns(start_ns + util_mul_div64(frames_sent, 1000000000ULL, SAMPLE_RATE));
	}

	return NULL;
}

static const char *loop_track_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("LoopTrackSwitcher");
}

static void loop_track_destroy(void *data)
{
	struct loop_track *lt = data;
	if (!lt)
		return;

	if (lt->thread_created) {
		os_event_signal(lt->stop_event);
		pthread_join(lt->thread, NULL);
	}
	if (lt->stop_event)
		os_event_destroy(lt->stop_event);
	bfree(lt);
}

static void *loop_track_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct loop_track *lt = bzalloc(sizeof(struct loop_track));
	lt->source = source;

	if (os_event_init(&lt->stop_event, OS_EVENT_TYPE_MANUAL) != 0) {
		obs_log(LOG_ERROR, "failed to create the stop event");
		loop_track_destroy(lt);
		return NULL;
	}
	if (pthread_create(&lt->thread, NULL, generator_thread, lt) != 0) {
		obs_log(LOG_ERROR, "failed to start the generator thread");
		loop_track_destroy(lt);
		return NULL;
	}
	lt->thread_created = true;
	return lt;
}

struct obs_source_info loop_track_source_info = {
	.id = LOOP_TRACK_SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO,
	.icon_type = OBS_ICON_TYPE_AUDIO_INPUT,
	.get_name = loop_track_get_name,
	.create = loop_track_create,
	.destroy = loop_track_destroy,
};
