/*
Builds the implementations of dr_wav, dr_flac, and dr_mp3 (src/third-party, public domain or MIT-0) in their own
translation unit. stb_vorbis.c is compiled as a separate source file.
*/

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
