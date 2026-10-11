/*
 * Differential test: Shared/scream_protocol.h against scream_fill_header() taken verbatim
 * from snd-screamalsa.c (extracted at build time into build/kernel_header_snippet.c by
 * Tests/run_tests.sh), for every PCM format, both dialects, EOT on/off, and every rate
 * 44.1k/48k family up to the 12-bit multiplier limit.
 */
#include "../Shared/scream_protocol.h"

#include <stdio.h>

/* ---- shims for the kernel snippet ---- */
typedef uint8_t u8;
typedef enum {
    SNDRV_PCM_FORMAT_S16_LE, SNDRV_PCM_FORMAT_S24_LE, SNDRV_PCM_FORMAT_S32_LE, SNDRV_PCM_FORMAT_S24_3LE,
} snd_pcm_format_t;
#define SCREAM_WIRE_PACKED  0U
#define SCREAM_WIRE_S24_LE  1U
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
static const u8 ch_mask[] = {0, 1, 3, 7, 15, 31, 63, 127, 255};
static char header_str[16] = "extended";
static bool sysfs_streq(const char *a, const char *b) { return strcmp(a, b) == 0; }
struct snd_scream_device {
    unsigned int sample_rate;
    unsigned int channels;
    snd_pcm_format_t format;
    bool is_dsd;
};

#include "kernel_header_snippet.c"

int main(void)
{
    static const struct { snd_pcm_format_t k; scream_wire_t w; } fmts[] = {
        {SNDRV_PCM_FORMAT_S16_LE, SCREAM_WIRE_S16LE},
        {SNDRV_PCM_FORMAT_S24_3LE, SCREAM_WIRE_S24_3LE},
        {SNDRV_PCM_FORMAT_S24_LE, SCREAM_WIRE_S24LE},
        {SNDRV_PCM_FORMAT_S32_LE, SCREAM_WIRE_S32LE},
    };
    unsigned long cases = 0, failures = 0;

    for (int d = 0; d < 2; d++) {
        strcpy(header_str, d ? "original" : "extended");
        static const uint32_t bases[] = {44100, 48000};
        for (size_t b = 0; b < 2; b++) {
            /* original byte[0] is 7 bits of multiplier (bit 7 = 44.1k family) */
            uint32_t max_mult = d ? 127u : 4095u;
            for (uint32_t mult = 1; mult <= max_mult; mult++) {
                uint32_t rate = bases[b] * mult;
                for (size_t f = 0; f < sizeof fmts / sizeof fmts[0]; f++) {
                    if (d && fmts[f].w != SCREAM_WIRE_S32LE) continue;
                    for (int eot = 0; eot < 2; eot++) {
                        struct snd_scream_device dev = {rate, 2, fmts[f].k, false};
                        u8 want[6] = {0}, got[6] = {0};
                        scream_fill_header(&dev, want, eot);
                        unsigned n = scream_encode_header(got, rate, fmts[f].w, (scream_dialect_t)d, eot);
                        cases++;
                        if (memcmp(want, got, n) != 0) {
                            if (failures++ < 10)
                                printf("FAIL dialect %d rate %u fmt %zu eot %d: kernel %02x %02x %02x %02x %02x %02x, ours %02x %02x %02x %02x %02x %02x\n",
                                       d, rate, f, eot, want[0], want[1], want[2], want[3], want[4], want[5],
                                       got[0], got[1], got[2], got[3], got[4], got[5]);
                        }
                    }
                }
            }
        }
    }
    if (failures) {
        printf("%lu/%lu header cases differ from snd-screamalsa.c\n", failures, cases);
        return 1;
    }
    printf("all %lu header cases match snd-screamalsa.c\n", cases);
    return 0;
}
