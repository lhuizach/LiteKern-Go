/* The MP3 decoder for the Music app: minimp3 (third_party/minimp3, public domain) behind a small
 * interface. minimp3 does its own maths inside (it uses floats, NEON on the phone); the rest of
 * the app doesn't. */
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#define MINIMP3_IMPLEMENTATION
#include "third_party/minimp3/minimp3.h"
#include "apps/music/mp3dec.h"

static mp3dec_t dec;

void mp3_reset(void)
{
    mp3dec_init(&dec);
}

int mp3_frame(const uint8_t *in, int bytes, int16_t *pcm, struct mp3_info *out)
{
    mp3dec_frame_info_t fi;
    int n = mp3dec_decode_frame(&dec, in, bytes, pcm, &fi);
    out->used = fi.frame_bytes;
    out->channels = fi.channels;
    out->hz = fi.hz;
    out->kbps = fi.bitrate_kbps;
    return n;
}
