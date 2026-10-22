/*
 * alla5.h - clean-room reimplementation of the Fuji Xerox "alla5" lossless
 * raster compressor used by the Xerox Phaser 6000/6010 host-based driver
 * (XRM_AL5CB).  Reconstructed from the shipped macOS filter by static and
 * dynamic analysis; output is byte-identical to the original encoder.
 */
#ifndef ALLA5_H
#define ALLA5_H

#include <stddef.h>
#include <stdint.h>

/* One growing output buffer per code stream. */
typedef struct {
    uint8_t *data;
    size_t len, cap;
    uint64_t acc;  /* pending bits, MSB first */
    int nacc;      /* number of pending bits in acc */
} a5_stream;

typedef struct a5_enc a5_enc;

/*
 * color: 1 = CMYK, 32 bit per pixel (4 error streams)
 *        0 = monochrome, 8 bit per pixel (1 error stream)
 * width: pixels per input line, lph: prepend the 8 pixel LPH marker
 */
a5_enc *a5_create(int width, int color, int lph);
/* line: width pixels, 4 bytes (C,M,Y,K) per pixel for color, 1 byte for mono */
void a5_encode_line(a5_enc *e, const uint8_t *line);
/* Terminates the image and flushes all streams. */
void a5_finish(a5_enc *e);
/* Streams 0..4 (0 = run/order codes, 1..4 = per-component errors). */
const a5_stream *a5_stream_get(const a5_enc *e, int i);
int a5_nstreams(const a5_enc *e);
void a5_destroy(a5_enc *e);

#endif
