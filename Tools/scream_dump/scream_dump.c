/*
 * scream_dump: minimal Scream receiver for testing ScreamMusic on the Mac itself.
 *
 * Receives Scream UDP packets, logs format changes / end-of-track markers / pacing, and
 * writes each format segment's payload to its own raw file, for byte-for-byte comparison
 * with the decoded source (Tests/compare.sh).
 *
 *   scream_dump [-p port] [-L] [-o prefix] [-q]
 *     -p port    UDP port (default 4011)
 *     -L         original 5-byte header (default: extended 6-byte)
 *     -o prefix  write segments to <prefix>-NNN-<rate>-<bits>.raw
 *     -q         no per-second pacing lines
 *
 * License: GPL-3.0 (see ../../LICENSE).
 */
#include "../../Shared/scream_protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

typedef struct {
    bool active;
    scream_header_t fmt;
    FILE *file;
    unsigned index;
    unsigned long long packets, bytes;
    double started;
    /* pacing over the current one-second window */
    double win_start, last_arrival, max_gap;
    unsigned long long win_packets;
} segment_t;

static const char *g_prefix;
static scream_dialect_t g_dialect = SCREAM_DIALECT_EXTENDED;
static bool g_quiet;

static const char *layout_name(const scream_header_t *h)
{
    if (h->sample_size == 24) return h->wire_layout == 1 ? "S24_LE" : "S24_3LE";
    if (h->sample_size == 16) return "S16_LE";
    if (h->sample_size == 32) return "S32_LE";
    if (h->sample_size == 1) return "DSD";
    return "?";
}

static void close_segment(segment_t *s, const char *why)
{
    if (!s->active) return;
    double secs = now_s() - s->started;
    unsigned bps = scream_header_bytes_per_sample(&s->fmt);
    double frames = bps ? (double)s->bytes / (bps * SCREAM_CHANNELS) : 0;
    printf("segment %03u end (%s): %llu packets, %llu bytes, %.3f s of audio in %.3f s\n",
           s->index, why, s->packets, s->bytes, s->fmt.rate ? frames / s->fmt.rate : 0, secs);
    if (s->file) fclose(s->file);
    s->file = NULL;
    s->active = false;
}

static void open_segment(segment_t *s, const scream_header_t *h)
{
    s->index++;
    s->active = true;
    s->fmt = *h;
    s->fmt.eot = false;
    s->packets = s->bytes = 0;
    s->started = s->win_start = s->last_arrival = now_s();
    s->win_packets = 0;
    s->max_gap = 0;
    printf("segment %03u: %u Hz, %s (%u-bit), %u ch\n", s->index, h->rate, layout_name(h), h->sample_size, h->channels);
    if (g_prefix) {
        char path[1024];
        snprintf(path, sizeof path, "%s-%03u-%u-%s.raw", g_prefix, s->index, h->rate, layout_name(h));
        s->file = fopen(path, "wb");
        if (!s->file) printf("  cannot write %s: %s\n", path, strerror(errno));
        else printf("  -> %s\n", path);
    }
}

static void pacing(segment_t *s, double t, size_t payload)
{
    double gap = t - s->last_arrival;
    if (s->packets > 1 && gap > s->max_gap) s->max_gap = gap;
    s->last_arrival = t;
    s->win_packets++;
    if (t - s->win_start < 1.0) return;
    unsigned bps = scream_header_bytes_per_sample(&s->fmt);
    double expect = bps ? (double)s->fmt.rate * bps * SCREAM_CHANNELS / payload : 0;
    double got = s->win_packets / (t - s->win_start);
    if (!g_quiet)
        printf("  %.1f pkt/s (expected %.1f), max gap %.2f ms\n", got, expect, s->max_gap * 1e3);
    s->win_start = t;
    s->win_packets = 0;
    s->max_gap = 0;
}

int main(int argc, char **argv)
{
    unsigned port = SCREAM_DEFAULT_PORT;
    int opt;
    while ((opt = getopt(argc, argv, "p:Lo:q")) != -1) {
        switch (opt) {
        case 'p': port = (unsigned)atoi(optarg); break;
        case 'L': g_dialect = SCREAM_DIALECT_ORIGINAL; break;
        case 'o': g_prefix = optarg; break;
        case 'q': g_quiet = true; break;
        default:
            fprintf(stderr, "usage: %s [-p port] [-L] [-o prefix] [-q]\n", argv[0]);
            return 2;
        }
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    int rcvbuf = 4 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;   /* no SA_RESTART: recv returns EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    unsigned hdr = scream_header_size(g_dialect);
    printf("listening on UDP %u, %s header (%u bytes)\n", port,
           g_dialect == SCREAM_DIALECT_ORIGINAL ? "original" : "extended", hdr);
    fflush(stdout);

    segment_t seg = {0};
    uint8_t buf[2048];
    unsigned long long odd_sizes = 0, eots = 0;

    while (!g_stop) {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof from;
        ssize_t n = recvfrom(fd, buf, sizeof buf, 0, (struct sockaddr *)&from, &fromlen);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("recvfrom");
            break;
        }
        double t = now_s();
        if ((size_t)n < hdr) { odd_sizes++; continue; }

        scream_header_t h;
        scream_decode_header(buf, g_dialect, &h);
        size_t payload = (size_t)n - hdr;

        if (h.eot) {
            eots++;
            printf("end-of-track marker (%u Hz, %s, payload %zu)\n", h.rate, layout_name(&h), payload);
            close_segment(&seg, "end-of-track");
            fflush(stdout);
            continue;   /* UDP EOT carries no new audio (snd-screamalsa sends header only) */
        }
        if (payload != SCREAM_PAYLOAD_SIZE) {
            if (odd_sizes++ < 5) printf("packet with %zu-byte payload (expected %u)\n", payload, SCREAM_PAYLOAD_SIZE);
        }

        bool changed = !seg.active || h.rate != seg.fmt.rate || h.sample_size != seg.fmt.sample_size ||
                       h.channels != seg.fmt.channels || (h.sample_size == 24 && h.wire_layout != seg.fmt.wire_layout);
        if (changed) {
            close_segment(&seg, "format change");
            open_segment(&seg, &h);
        }
        seg.packets++;
        seg.bytes += payload;
        if (seg.file) fwrite(buf + hdr, 1, payload, seg.file);
        pacing(&seg, t, payload);
        fflush(stdout);
    }

    close_segment(&seg, "stopped");
    printf("end-of-track markers: %llu, malformed/odd packets: %llu\n", eots, odd_sizes);
    close(fd);
    return 0;
}
