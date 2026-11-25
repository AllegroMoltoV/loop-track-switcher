"""Writes the small audio files used by tests/engine-test.cpp.

The sounds are synthesized here, so they carry no third-party rights. ffmpeg (with libmp3lame, libvorbis, and flac)
encodes them. Run from the repository root: python tests/data/make-fixtures.py

tone-44k.*  1.0 s, 44.1 kHz, stereo. Silence with a 20 ms burst at 0.5 s: 1 kHz on the left, 2 kHz on the right.
tone-6ch.*  1.0 s, 48 kHz, 5.1. The same bursts on front left and front right, silence on the other channels.
"""

import math
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def write_wav(path, rate, channels, frames):
    data = bytearray()
    for frame in frames:
        for value in frame:
            data += struct.pack("<h", max(-32768, min(32767, round(value * 32767))))
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, channels, rate, rate * channels * 2, channels * 2, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def burst(rate, i, frequency):
    start = rate // 2
    length = rate // 50
    if start <= i < start + length:
        return 0.5 * math.sin(2 * math.pi * frequency * (i - start) / rate)
    return 0.0


def main():
    with tempfile.TemporaryDirectory() as tmp:
        stereo = os.path.join(tmp, "stereo.wav")
        write_wav(stereo, 44100, 2, [(burst(44100, i, 1000), burst(44100, i, 2000)) for i in range(44100)])
        six = os.path.join(tmp, "six.wav")
        # WAV order: FL, FR, FC, LFE, BL, BR
        write_wav(six, 48000, 6, [(burst(48000, i, 1000), burst(48000, i, 2000), 0, 0, 0, 0) for i in range(48000)])

        def ffmpeg(*args):
            subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *args], check=True)

        ffmpeg("-i", stereo, "-c:a", "libmp3lame", "-b:a", "192k", os.path.join(HERE, "tone-44k.mp3"))
        ffmpeg("-i", stereo, "-c:a", "libvorbis", "-q:a", "6", os.path.join(HERE, "tone-44k.ogg"))
        ffmpeg("-i", stereo, "-c:a", "flac", os.path.join(HERE, "tone-44k.flac"))
        # The Vorbis encoder reorders 5.1 to Vorbis order (FL, FC, FR, BL, BR, LFE). FLAC keeps the WAV order.
        ffmpeg("-channel_layout", "5.1", "-i", six, "-c:a", "libvorbis", "-q:a", "6", os.path.join(HERE, "tone-6ch.ogg"))
        ffmpeg("-channel_layout", "5.1", "-i", six, "-c:a", "flac", os.path.join(HERE, "tone-6ch.flac"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
