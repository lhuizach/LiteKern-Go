/* LiteKern X — DEFLATE compression (RFC 1951), the other half of
 * kernel/inflate.h: for what the system saves to the disk itself (a custom
 * wallpaper, kernel/settings.h).
 *
 * Raw deflate (no zlib or gzip wrapper): LZ77 over a 32 KiB window with hash
 * chains, then Huffman codes made for each block. Not as tight as zlib's
 * best, but most of the way there, and quick. */
#ifndef LKX_DEFLATE_H
#define LKX_DEFLATE_H

#include <stddef.h>
#include <stdint.h>

/* The working memory deflate_raw() needs, given by the caller. */
#define DEFLATE_WORK (5u * 32768 * 4)

/* Compress in[0..in_len) into out[0..out_len). Returns the bytes written,
 * or -ENOSPC if out is too small (in_len + in_len / 64 + 1024 is always
 * enough). */
int deflate_raw(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len, void *work);

/* The same, for one part of a bigger stream: the parts, each compressed on its own (so they can be
 * done at the same time), joined one after another make a valid stream if every one but the last
 * has final 0 (it then ends with an empty stored block, so it ends on a byte) and the last has 1.
 * effort is how many earlier places are tried for each match: 0 the usual 32; fewer is faster and
 * makes a little more. */
int deflate_part(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len, void *work, int final,
                 int effort);

#endif
