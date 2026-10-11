/*
 * Unit tests for Shared/scream_protocol.h.
 * Header vectors follow scream_fill_header() in snd-screamalsa.c 2.0.7.
 */
#include "../Shared/scream_protocol.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void expect_header(uint32_t rate, scream_wire_t w, scream_dialect_t d, bool eot,
                          const uint8_t *want, unsigned want_len)
{
    uint8_t h[SCREAM_HEADER_SIZE_MAX] = {0};
    unsigned n = scream_encode_header(h, rate, w, d, eot);
    CHECK(n == want_len, "rate %u: header size %u, want %u", rate, n, want_len);
    for (unsigned i = 0; i < want_len; i++)
        CHECK(h[i] == want[i], "rate %u wire %d dialect %d eot %d: byte[%u] = 0x%02x, want 0x%02x",
              rate, (int)w, (int)d, (int)eot, i, h[i], want[i]);
}

static void test_headers(void)
{
    expect_header(44100,  SCREAM_WIRE_S16LE,   SCREAM_DIALECT_EXTENDED, false, (const uint8_t[]){1, 16, 2, 3, 0x10, 0}, 6);
    expect_header(48000,  SCREAM_WIRE_S16LE,   SCREAM_DIALECT_EXTENDED, true,  (const uint8_t[]){1, 16, 2, 3, 0x80, 0}, 6);
    expect_header(96000,  SCREAM_WIRE_S24_3LE, SCREAM_DIALECT_EXTENDED, false, (const uint8_t[]){2, 24, 2, 3, 0x00, 0}, 6);
    expect_header(192000, SCREAM_WIRE_S24LE,   SCREAM_DIALECT_EXTENDED, false, (const uint8_t[]){4, 24, 2, 3, 0x00, 1}, 6);
    expect_header(352800, SCREAM_WIRE_S32LE,   SCREAM_DIALECT_EXTENDED, false, (const uint8_t[]){8, 32, 2, 3, 0x10, 0}, 6);
    expect_header(88200,  SCREAM_WIRE_S24_3LE, SCREAM_DIALECT_EXTENDED, true,  (const uint8_t[]){2, 24, 2, 3, 0x90, 0}, 6);
    /* 12-bit multiplier: 44100 * 300 */
    expect_header(13230000, SCREAM_WIRE_S32LE, SCREAM_DIALECT_EXTENDED, false, (const uint8_t[]){44, 32, 2, 3, 0x11, 0}, 6);

    expect_header(44100,  SCREAM_WIRE_S32LE, SCREAM_DIALECT_ORIGINAL, false, (const uint8_t[]){129, 32, 2, 3, 0x00}, 5);
    expect_header(176400, SCREAM_WIRE_S32LE, SCREAM_DIALECT_ORIGINAL, true,  (const uint8_t[]){132, 32, 2, 3, 0x80}, 5);
    expect_header(96000,  SCREAM_WIRE_S32LE, SCREAM_DIALECT_ORIGINAL, false, (const uint8_t[]){2, 32, 2, 3, 0x00}, 5);

    static const uint32_t rates[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000};
    for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
        for (int d = 0; d < 2; d++) {
            for (int e = 0; e < 2; e++) {
                uint8_t h[SCREAM_HEADER_SIZE_MAX];
                scream_header_t got;
                scream_wire_t w = d ? SCREAM_WIRE_S32LE : SCREAM_WIRE_S24_3LE;
                scream_encode_header(h, rates[i], w, (scream_dialect_t)d, e);
                scream_decode_header(h, (scream_dialect_t)d, &got);
                CHECK(got.rate == rates[i], "roundtrip rate %u dialect %d: got %u", rates[i], d, got.rate);
                CHECK(got.eot == (bool)e, "roundtrip eot rate %u dialect %d", rates[i], d);
                CHECK(got.sample_size == scream_sample_size_byte(w), "roundtrip size rate %u", rates[i]);
            }
        }
    }

    CHECK(SCREAM_PAYLOAD_SIZE % scream_frame_bytes(SCREAM_WIRE_S16LE) == 0, "1152 %% S16 frame");
    CHECK(SCREAM_PAYLOAD_SIZE % scream_frame_bytes(SCREAM_WIRE_S24_3LE) == 0, "1152 %% S24_3LE frame");
    CHECK(SCREAM_PAYLOAD_SIZE % scream_frame_bytes(SCREAM_WIRE_S32LE) == 0, "1152 %% S32 frame");
}

static int32_t read_le(const uint8_t *p, unsigned bytes)
{
    uint32_t u = 0;
    for (unsigned i = 0; i < bytes; i++)
        u |= (uint32_t)p[i] << (8 * i);
    unsigned sh = 32 - 8 * bytes;
    return (int32_t)(u << sh) >> sh;
}

/* Every 16-bit and every 24-bit value survives int -> Float32 (as CoreAudio does: v / 2^(N-1)) -> wire. */
static void test_float_exact(void)
{
    uint8_t out[4];
    for (int32_t v = -32768; v <= 32767; v++) {
        float x = (float)v / 32768.0f;
        unsigned off = scream_pack_float(out, &x, 1, SCREAM_WIRE_S16LE);
        int32_t got = read_le(out, 2);
        CHECK(got == v && off == 0, "16-bit %d -> %d (off %u)", v, got, off);
        if (got != v) return;
    }
    for (int32_t v = -8388608; v <= 8388607; v++) {
        float x = (float)v / 8388608.0f;
        scream_pack_float(out, &x, 1, SCREAM_WIRE_S24_3LE);
        int32_t got = read_le(out, 3);
        if (got != v) { CHECK(0, "24-bit %d -> %d", v, got); return; }
    }
    /* a 16-bit track sent as 24-bit: value << 8 */
    float x = -12345.0f / 32768.0f;
    scream_pack_float(out, &x, 1, SCREAM_WIRE_S24_3LE);
    CHECK(read_le(out, 3) == -12345 * 256, "16 in 24");

    /* clamp, NaN, off-grid count */
    float edge[] = {1.0f, -1.0f, 2.0f, NAN, 0.5f + 1.0f / 65536.0f};
    uint8_t buf[sizeof edge / sizeof edge[0] * 2];
    unsigned off = scream_pack_float(buf, edge, 5, SCREAM_WIRE_S16LE);
    CHECK(read_le(buf + 0, 2) == 32767, "clamp +1.0");
    CHECK(read_le(buf + 2, 2) == -32768, "-1.0");
    CHECK(read_le(buf + 4, 2) == 32767, "clamp +2.0");
    CHECK(read_le(buf + 6, 2) == 0, "NaN -> 0");
    CHECK(off == 1, "off-grid count %u, want 1", off);
}

static void test_pack_int(void)
{
    int16_t s16[] = {-32768, -1, 0, 1, 32767};
    uint8_t out[5 * 4];
    scream_pack_int(out, (const uint8_t *)s16, 5, 16, 2, SCREAM_WIRE_S16LE);
    for (int i = 0; i < 5; i++)
        CHECK(read_le(out + 2 * i, 2) == s16[i], "s16 passthrough %d", i);

    /* original dialect: 16-bit widened to S32_LE */
    scream_pack_int(out, (const uint8_t *)s16, 5, 16, 2, SCREAM_WIRE_S32LE);
    for (int i = 0; i < 5; i++)
        CHECK(read_le(out + 4 * i, 4) == (int32_t)((uint32_t)(int32_t)s16[i] << 16), "s16 -> s32 %d", i);

    uint8_t s24[] = {0x00, 0x00, 0x80, 0xff, 0xff, 0xff, 0x56, 0x34, 0x12};
    scream_pack_int(out, s24, 3, 24, 3, SCREAM_WIRE_S24_3LE);
    CHECK(memcmp(out, s24, 9) == 0, "s24 packed passthrough");

    int32_t s24in32[] = {-8388608, -1, 0x123456};
    scream_pack_int(out, (const uint8_t *)s24in32, 3, 24, 4, SCREAM_WIRE_S24_3LE);
    CHECK(memcmp(out, s24, 9) == 0, "24-in-32 -> s24 packed");

    int32_t s32[] = {INT32_MIN, -1, 0, INT32_MAX};
    scream_pack_int(out, (const uint8_t *)s32, 4, 32, 4, SCREAM_WIRE_S32LE);
    for (int i = 0; i < 4; i++)
        CHECK(read_le(out + 4 * i, 4) == s32[i], "s32 passthrough %d", i);
}

int main(void)
{
    test_headers();
    test_float_exact();
    test_pack_int();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all protocol tests passed\n");
    return 0;
}
