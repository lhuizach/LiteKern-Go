#ifndef LKX_MP3DEC_H
#define LKX_MP3DEC_H
#include <stdint.h>

#define MP3_FRAME_MAX 1152                      /* frames (per channel) one MP3 frame makes */

struct mp3_info {
    int used;                                   /* bytes of the input it ate (0: needs more) */
    int channels, hz, kbps;
};

void mp3_reset(void);
/* Decodes the first frame in in[0..bytes): interleaved 16 bit into pcm (MP3_FRAME_MAX * 2).
 * Returns the frames made; 0 with used > 0 means it skipped something that wasn't sound. */
int mp3_frame(const uint8_t *in, int bytes, int16_t *pcm, struct mp3_info *out);
#endif
