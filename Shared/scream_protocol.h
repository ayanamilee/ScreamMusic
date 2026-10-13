/*
 * ScreamMusic: Scream wire protocol (header encode/decode, sample packing).
 *
 * Mirrors snd-screamalsa.c 2.0.7 (repo root), the reference sender:
 *   extended (default): 6-byte header, S16_LE / S24_3LE / S24_LE / S32_LE
 *   original:           5-byte igor63r header, S32_LE only
 *
 * Header-only, no allocation, no system calls: safe on the HAL IO thread.
 *
 * License: GPL-3.0 (see ../LICENSE).
 */
#ifndef SCREAM_PROTOCOL_H
#define SCREAM_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SCREAM_PAYLOAD_SIZE          1152u
#define SCREAM_HEADER_SIZE_EXTENDED  6u
#define SCREAM_HEADER_SIZE_ORIGINAL  5u
#define SCREAM_HEADER_SIZE_MAX       SCREAM_HEADER_SIZE_EXTENDED
#define SCREAM_PACKET_SIZE_MAX       (SCREAM_HEADER_SIZE_MAX + SCREAM_PAYLOAD_SIZE)
#define SCREAM_DEFAULT_PORT          4011u
#define SCREAM_CHANNELS              2u
#define SCREAM_CHANNEL_MASK          3u   /* ch_mask[2] in snd-screamalsa.c */

#define SCREAM_B4_EOT                0x80u
#define SCREAM_B4_BASE_44100         0x10u

typedef enum {
    SCREAM_DIALECT_EXTENDED = 0,
    SCREAM_DIALECT_ORIGINAL = 1,
} scream_dialect_t;

/* PCM layout on the wire. byte[5] is 1 only for S24_LE (24 bits in a 32-bit container). */
typedef enum {
    SCREAM_WIRE_S16LE = 0,
    SCREAM_WIRE_S24_3LE,
    SCREAM_WIRE_S24LE,
    SCREAM_WIRE_S32LE,
} scream_wire_t;

static inline unsigned scream_header_size(scream_dialect_t d)
{
    return d == SCREAM_DIALECT_ORIGINAL ? SCREAM_HEADER_SIZE_ORIGINAL : SCREAM_HEADER_SIZE_EXTENDED;
}

static inline unsigned scream_bytes_per_sample(scream_wire_t w)
{
    switch (w) {
    case SCREAM_WIRE_S16LE:   return 2;
    case SCREAM_WIRE_S24_3LE: return 3;
    case SCREAM_WIRE_S24LE:
    case SCREAM_WIRE_S32LE:   return 4;
    }
    return 4;
}

static inline unsigned scream_frame_bytes(scream_wire_t w)
{
    return scream_bytes_per_sample(w) * SCREAM_CHANNELS;
}

/* header byte[1]: effective PCM bit depth (scream_pcm_wire_bits) */
static inline uint8_t scream_sample_size_byte(scream_wire_t w)
{
    switch (w) {
    case SCREAM_WIRE_S16LE:   return 16;
    case SCREAM_WIRE_S24_3LE:
    case SCREAM_WIRE_S24LE:   return 24;
    case SCREAM_WIRE_S32LE:   return 32;
    }
    return 32;
}

/* The original 5-byte dialect has no wire_layout byte: receivers treat PCM as 32-bit. */
static inline bool scream_wire_allowed(scream_dialect_t d, scream_wire_t w)
{
    return d == SCREAM_DIALECT_EXTENDED || w == SCREAM_WIRE_S32LE;
}

/*
 * scream_fill_header() from snd-screamalsa.c. PCM only (macOS has no DSD source).
 * Extended: mult = rate / base (base 44100 if rate % 44100 == 0, else 48000),
 *           byte[0] = mult & 0xff, byte[4] = (mult >> 8 & 0x0f) | 0x10 (44.1k) | 0x80 (EOT).
 * Original: byte[0] = 128 + rate/44100 (44.1k family) or rate/48000; byte[4] = 0x80 at EOT, else 0.
 * Returns the header size written.
 */
static inline unsigned scream_encode_header(uint8_t *out, uint32_t rate, scream_wire_t w,
                                            scream_dialect_t d, bool eot)
{
    out[2] = (uint8_t)SCREAM_CHANNELS;
    out[3] = (uint8_t)SCREAM_CHANNEL_MASK;

    if (d == SCREAM_DIALECT_ORIGINAL) {
        out[1] = 32;
        if (rate % 44100u)
            out[0] = (uint8_t)(rate / 48000u);
        else
            out[0] = (uint8_t)(128u + rate / 44100u);
        out[4] = eot ? SCREAM_B4_EOT : 0;
        return SCREAM_HEADER_SIZE_ORIGINAL;
    }

    uint32_t base = (rate % 44100u == 0) ? 44100u : 48000u;
    uint32_t mult = rate / base;
    out[0] = (uint8_t)(mult & 0xffu);
    out[1] = scream_sample_size_byte(w);
    out[4] = (uint8_t)(((mult >> 8) & 0x0fu) |
                       (base == 44100u ? SCREAM_B4_BASE_44100 : 0) |
                       (eot ? SCREAM_B4_EOT : 0));
    out[5] = (w == SCREAM_WIRE_S24LE) ? 1 : 0;
    return SCREAM_HEADER_SIZE_EXTENDED;
}

/* Decoded header, as the Unix receiver / scream2diretta read it. */
typedef struct {
    uint32_t rate;
    uint8_t  sample_size;   /* 16 / 24 / 32, 1 = DSD */
    uint8_t  channels;
    uint8_t  wire_layout;   /* extended byte[5]; 0 in original */
    bool     eot;
} scream_header_t;

static inline void scream_decode_header(const uint8_t *in, scream_dialect_t d, scream_header_t *h)
{
    h->sample_size = in[1];
    h->channels = in[2];
    h->eot = (in[4] & SCREAM_B4_EOT) != 0;
    if (d == SCREAM_DIALECT_ORIGINAL) {
        h->rate = in[0] >= 128 ? 44100u * (uint32_t)(in[0] - 128) : 48000u * in[0];
        h->wire_layout = 0;
        return;
    }
    uint32_t mult = (uint32_t)in[0] | ((uint32_t)(in[4] & 0x0fu) << 8);
    h->rate = ((in[4] & SCREAM_B4_BASE_44100) ? 44100u : 48000u) * mult;
    h->wire_layout = in[5];
}

static inline unsigned scream_header_bytes_per_sample(const scream_header_t *h)
{
    switch (h->sample_size) {
    case 1:  return 4;
    case 16: return 2;
    case 24: return h->wire_layout == 1 ? 4 : 3;
    case 32: return 4;
    default: return 0;
    }
}

/* ---- sample packing ------------------------------------------------------ */

static inline void scream_put_le(uint8_t *dst, int32_t v, unsigned bytes)
{
    uint32_t u = (uint32_t)v;
    for (unsigned i = 0; i < bytes; i++)
        dst[i] = (uint8_t)(u >> (8 * i));
}

/* Little-endian signed integer of 2..4 bytes, sign-extended. */
static inline int32_t scream_get_le(const uint8_t *src, unsigned bytes)
{
    uint32_t u = 0;
    for (unsigned i = 0; i < bytes; i++)
        u |= (uint32_t)src[i] << (8 * i);
    if (bytes < 4 && (u & (1u << (8 * bytes - 1))))
        u |= ~0u << (8 * bytes);
    return (int32_t)u;
}

/*
 * The inverse of scream_pack_float: wire integer -> Float32, v / 2^(N-1). Exact for every 16- and
 * 24-bit sample, and for the original dialect's 24-bit audio left-aligned in S32 (24 significant
 * bits fit Float32's mantissa); a full 32-bit integer source would not be exact (never mixed in
 * Float32 on this path).
 */
static inline void scream_unpack_float(float *dst, const uint8_t *src, size_t samples, scream_wire_t w)
{
    unsigned bits = scream_sample_size_byte(w);
    unsigned bps = scream_bytes_per_sample(w);
    double inv = 1.0 / (double)(1ull << (bits - 1));
    for (size_t i = 0; i < samples; i++) {
        dst[i] = (float)((double)scream_get_le(src, bps) * inv);
        src += bps;
    }
}

/*
 * Exact float -> N-bit integer: round(x * 2^(N-1)), clamped, no dither.
 * A 16- or 24-bit sample carried in Float32 comes back unchanged. Scaling by
 * 2^(N-1) - 1 (32767, 8388607, ...) would not be bit-perfect.
 * Returns how many samples landed off the N-bit grid (x * 2^(N-1) not an integer).
 */
static inline unsigned scream_pack_float(uint8_t *dst, const float *src, size_t samples, scream_wire_t w)
{
    unsigned bits = scream_sample_size_byte(w);
    unsigned bps = scream_bytes_per_sample(w);
    double scale = (double)(1ull << (bits - 1));
    double hi = scale - 1.0, lo = -scale;
    unsigned off_grid = 0;

    for (size_t i = 0; i < samples; i++) {
        double y = (double)src[i] * scale;
        if (y != y)                     /* NaN */
            y = 0.0;
        double r = __builtin_rint(y);
        if (r != y)
            off_grid++;
        if (r > hi) r = hi;
        if (r < lo) r = lo;
        scream_put_le(dst, (int32_t)r, bps);
        dst += bps;
    }
    return off_grid;
}

/*
 * Native-endian integer samples (CoreAudio physical formats) -> wire.
 * in_bits / in_bytes describe the source (SInt16 = 16/2, packed SInt24 = 24/3,
 * SInt32 = 32/4, 24 in 4 = 24/4 low-aligned). The value is left-shifted when the
 * wire is wider (original dialect sends S32_LE), never truncated.
 */
static inline void scream_pack_int(uint8_t *dst, const uint8_t *src, size_t samples,
                                   unsigned in_bits, unsigned in_bytes, scream_wire_t w)
{
    unsigned out_bits = scream_sample_size_byte(w);
    unsigned bps = scream_bytes_per_sample(w);
    unsigned shift = out_bits > in_bits ? out_bits - in_bits : 0;

    for (size_t i = 0; i < samples; i++) {
        int32_t v = 0;
        switch (in_bytes) {
        case 2: { int16_t s; memcpy(&s, src, 2); v = s; break; }
        case 3: v = (int32_t)(((uint32_t)src[0] << 8) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 24)) >> 8;
                break;
        case 4: memcpy(&v, src, 4);
                if (in_bits < 32) v = (int32_t)((uint32_t)v << (32 - in_bits)) >> (32 - in_bits);
                break;
        default: break;
        }
        scream_put_le(dst, (int32_t)((uint32_t)v << shift), bps);
        src += in_bytes;
        dst += bps;
    }
}

#endif /* SCREAM_PROTOCOL_H */
