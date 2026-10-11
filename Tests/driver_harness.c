/*
 * In-process test of ScreamMusic.driver, no installation needed: loads the bundle, plays the
 * HAL's part (fake host, format changes, StartIO / WriteMix at real-time pace / StopIO) and
 * receives the Scream packets on 127.0.0.1, then checks them byte for byte.
 *
 *   driver_harness path/to/ScreamMusic.driver
 */
#include "../Shared/scream_protocol.h"

#include <CoreAudio/AudioServerPlugIn.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 14012
#define kDevice 2
#define kStream 3

static AudioServerPlugInDriverRef gDrv;
static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- fake host -------------------------------------------------------------- */

static CFMutableDictionaryRef gStorage;

static OSStatus host_PropertiesChanged(AudioServerPlugInHostRef h, AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress *a)
{
    (void)h; (void)obj; (void)n; (void)a;
    return 0;
}

static OSStatus host_CopyFromStorage(AudioServerPlugInHostRef h, CFStringRef key, CFPropertyListRef *out)
{
    (void)h;
    CFPropertyListRef v = CFDictionaryGetValue(gStorage, key);
    *out = v ? CFRetain(v) : NULL;
    return v ? 0 : kAudioHardwareUnknownPropertyError;
}

static OSStatus host_WriteToStorage(AudioServerPlugInHostRef h, CFStringRef key, CFPropertyListRef v)
{
    (void)h;
    CFDictionarySetValue(gStorage, key, v);
    return 0;
}

static OSStatus host_DeleteFromStorage(AudioServerPlugInHostRef h, CFStringRef key)
{
    (void)h;
    CFDictionaryRemoveValue(gStorage, key);
    return 0;
}

/* The HAL stops IO, then performs the change. The harness only changes formats while stopped. */
static OSStatus host_RequestConfigChange(AudioServerPlugInHostRef h, AudioObjectID dev, UInt64 action, void *info)
{
    (void)h;
    return (*gDrv)->PerformDeviceConfigurationChange(gDrv, dev, action, info);
}

static AudioServerPlugInHostInterface gHostIf = {
    host_PropertiesChanged, host_CopyFromStorage, host_WriteToStorage, host_DeleteFromStorage, host_RequestConfigChange,
};

/* ---- receiver --------------------------------------------------------------- */

typedef struct {
    scream_header_t fmt;
    uint8_t *data;
    size_t len, cap;
    unsigned packets, odd;
} rx_seg_t;

static rx_seg_t gSegs[16];
static int gSegCount;
static unsigned gEOTs;
static pthread_mutex_t gRxMutex = PTHREAD_MUTEX_INITIALIZER;
static scream_dialect_t gRxDialect = SCREAM_DIALECT_EXTENDED;

static void *rx_main(void *arg)
{
    int fd = *(int *)arg;
    uint8_t buf[2048];
    bool open = false;
    for (;;) {
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n < 0) continue;
        pthread_mutex_lock(&gRxMutex);
        unsigned hdr = scream_header_size(gRxDialect);
        scream_header_t h;
        scream_decode_header(buf, gRxDialect, &h);
        if (h.eot) {
            gEOTs++;
            open = false;
        } else {
            rx_seg_t *s = &gSegs[gSegCount ? gSegCount - 1 : 0];
            if (!open || h.rate != s->fmt.rate || h.sample_size != s->fmt.sample_size) {
                s = &gSegs[gSegCount++];
                s->fmt = h;
                open = true;
            }
            size_t payload = (size_t)n - hdr;
            if (payload != SCREAM_PAYLOAD_SIZE) s->odd++;
            if (s->len + payload > s->cap) {
                s->cap = (s->len + payload) * 2;
                s->data = realloc(s->data, s->cap);
            }
            memcpy(s->data + s->len, buf + hdr, payload);
            s->len += payload;
            s->packets++;
        }
        pthread_mutex_unlock(&gRxMutex);
    }
    return NULL;
}

/* ---- driver helpers ------------------------------------------------------------ */

static void set_cf(AudioObjectPropertySelector sel, CFPropertyListRef v)
{
    AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    OSStatus st = (*gDrv)->SetPropertyData(gDrv, kDevice, 0, &a, 0, NULL, sizeof v, &v);
    CHECK(st == 0, "set '%c%c%c%c' -> %d", (char)(sel >> 24), (char)(sel >> 16), (char)(sel >> 8), (char)sel, (int)st);
}

static void set_int(AudioObjectPropertySelector sel, int v)
{
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
    set_cf(sel, n);
    CFRelease(n);
}

static CFPropertyListRef get_cf(AudioObjectPropertySelector sel)
{
    AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFPropertyListRef v = NULL;
    UInt32 size = sizeof v;
    (*gDrv)->GetPropertyData(gDrv, kDevice, 0, &a, 0, NULL, size, &size, &v);
    return v;
}

static void set_destination(const char *header)
{
    int port = PORT;
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &port);
    CFStringRef hdr = CFStringCreateWithCString(NULL, header, kCFStringEncodingUTF8);
    const void *k[] = {CFSTR("ip"), CFSTR("port"), CFSTR("header")};
    const void *v[] = {CFSTR("127.0.0.1"), p, hdr};
    CFDictionaryRef d = CFDictionaryCreate(NULL, k, v, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    set_cf('SMds', d);
    CFRelease(d);
    CFRelease(p);
    CFRelease(hdr);
}

static void set_format(Float64 rate, UInt32 flags, UInt32 bits)
{
    AudioStreamBasicDescription d = {0};
    d.mSampleRate = rate;
    d.mFormatID = kAudioFormatLinearPCM;
    d.mFormatFlags = flags;
    d.mBitsPerChannel = bits;
    d.mChannelsPerFrame = 2;
    d.mFramesPerPacket = 1;
    d.mBytesPerFrame = d.mBytesPerPacket = bits / 8 * 2;
    AudioObjectPropertyAddress a = {kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    OSStatus st = (*gDrv)->SetPropertyData(gDrv, kStream, 0, &a, 0, NULL, sizeof d, &d);
    CHECK(st == 0, "set physical format %.0f/%u -> %d", rate, bits, (int)st);
}

/* Plays `frames` of `src` (physical format, `inFrameBytes` per frame) in 512-frame IO cycles at
 * real-time pace, as the HAL would. */
static void play(const uint8_t *src, size_t frames, size_t inFrameBytes, Float64 rate)
{
    (*gDrv)->StartIO(gDrv, kDevice, 1);
    const UInt32 cycle = 512;
    for (size_t done = 0; done < frames;) {
        UInt32 n = (UInt32)(frames - done < cycle ? frames - done : cycle);
        uint8_t buf[512 * 8];
        memcpy(buf, src + done * inFrameBytes, n * inFrameBytes);
        AudioServerPlugInIOCycleInfo info = {0};
        (*gDrv)->DoIOOperation(gDrv, kDevice, kStream, 1, kAudioServerPlugInIOOperationWriteMix, n, &info, buf, NULL);
        done += n;
        usleep((useconds_t)(n * 1e6 / rate));
    }
    (*gDrv)->StopIO(gDrv, kDevice, 1);
}

static void reset_rx(scream_dialect_t d)
{
    pthread_mutex_lock(&gRxMutex);
    for (int i = 0; i < gSegCount; i++) free(gSegs[i].data);
    memset(gSegs, 0, sizeof gSegs);
    gSegCount = 0;
    gEOTs = 0;
    gRxDialect = d;
    pthread_mutex_unlock(&gRxMutex);
}

/* Received audio must be `want` followed only by zero padding up to a whole payload. */
static void expect_rx(const char *name, uint32_t rate, unsigned bits, const uint8_t *want, size_t wantLen)
{
    usleep(300000);   /* drain + end-of-track */
    pthread_mutex_lock(&gRxMutex);
    CHECK(gSegCount == 1, "%s: %d segments", name, gSegCount);
    CHECK(gEOTs == 1, "%s: %u end-of-track markers", name, gEOTs);
    if (gSegCount >= 1) {
        rx_seg_t *s = &gSegs[0];
        CHECK(s->fmt.rate == rate, "%s: rate %u, want %u", name, s->fmt.rate, rate);
        CHECK(s->fmt.sample_size == bits, "%s: %u-bit, want %u", name, s->fmt.sample_size, bits);
        CHECK(s->odd == 0, "%s: %u packets with odd payload size", name, s->odd);
        size_t padded = (wantLen + SCREAM_PAYLOAD_SIZE - 1) / SCREAM_PAYLOAD_SIZE * SCREAM_PAYLOAD_SIZE;
        CHECK(s->len == padded, "%s: %zu bytes, want %zu", name, s->len, padded);
        size_t n = s->len < wantLen ? s->len : wantLen;
        size_t bad = n;
        for (size_t i = 0; i < n; i++) if (s->data[i] != want[i]) { bad = i; break; }
        CHECK(bad == n, "%s: first difference at byte %zu", name, bad);
        for (size_t i = wantLen; i < s->len; i++) if (s->data[i]) { CHECK(0, "%s: nonzero padding at %zu", name, i); break; }
        if (bad == n && s->len == padded) printf("ok   %-34s %u packets, %zu bytes bit-exact\n", name, s->packets, wantLen);
    }
    pthread_mutex_unlock(&gRxMutex);
}

static uint32_t rnd(void)
{
    static uint32_t x = 2463534242u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return x;
}

#define kIntFlags (kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonMixable)
#define kFloatFlags (kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked)

/* Float32 input carrying N-bit integers (as CoreAudio converts them: v / 2^(N-1)). */
static void case_float(Float64 rate, unsigned srcBits, int wantBits, scream_wire_t wire, const char *header, const char *name)
{
    size_t frames = (size_t)(rate * 0.4) + 37;   /* not a whole number of payloads */
    float *src = malloc(frames * 2 * sizeof(float));
    unsigned bps = scream_bytes_per_sample(wire);
    uint8_t *want = malloc(frames * 2 * bps);
    unsigned wireBits = scream_sample_size_byte(wire);
    for (size_t i = 0; i < frames * 2; i++) {
        int32_t v = (int32_t)(rnd() << (32 - srcBits)) >> (32 - srcBits);
        src[i] = (float)((double)v / (double)(1u << (srcBits - 1)));
        scream_put_le(want + i * bps, (int32_t)((uint32_t)v << (wireBits - srcBits)), bps);
    }
    scream_dialect_t d = strcmp(header, "original") == 0 ? SCREAM_DIALECT_ORIGINAL : SCREAM_DIALECT_EXTENDED;
    reset_rx(d);
    set_destination(header);
    set_int('SMbd', wantBits);
    set_format(rate, kFloatFlags, 32);
    play((const uint8_t *)src, frames, 8, rate);
    expect_rx(name, (uint32_t)rate, d == SCREAM_DIALECT_ORIGINAL ? 32 : wireBits, want, frames * 2 * bps);
    free(src);
    free(want);
}

/* Integer physical format (non-mixable): samples pass through unchanged. */
static void case_int(Float64 rate, unsigned bits, const char *name)
{
    size_t frames = (size_t)(rate * 0.4) + 11;
    unsigned bps = bits / 8;
    uint8_t *src = malloc(frames * 2 * bps);
    for (size_t i = 0; i < frames * 2 * bps; i++) src[i] = (uint8_t)rnd();
    reset_rx(SCREAM_DIALECT_EXTENDED);
    set_destination("extended");
    set_format(rate, kIntFlags, bits);
    play(src, frames, 2 * bps, rate);
    expect_rx(name, (uint32_t)rate, bits, src, frames * 2 * bps);
    free(src);
}

/* Stop, change format and start again immediately: the old run's tail goes out with the old
 * header and its end-of-track marker, then the new run. Then a quick same-format restart:
 * one seamless segment. */
static void case_restart(void)
{
    size_t fa = 44100 / 4 + 5, fb = 96000 / 4 + 3;
    uint8_t *a = malloc(fa * 4), *b = malloc(fb * 6);
    for (size_t i = 0; i < fa * 4; i++) a[i] = (uint8_t)rnd();
    for (size_t i = 0; i < fb * 6; i++) b[i] = (uint8_t)rnd();

    reset_rx(SCREAM_DIALECT_EXTENDED);
    set_int('SMdl', 200);     /* a long tail is still queued at the restart */
    set_format(44100, kIntFlags, 16);
    play(a, fa, 4, 44100);
    set_format(96000, kIntFlags, 24);
    play(b, fb, 6, 96000);
    usleep(500000);
    pthread_mutex_lock(&gRxMutex);
    CHECK(gSegCount == 2 && gEOTs == 2, "restart: %d segments, %u end-of-track markers", gSegCount, gEOTs);
    if (gSegCount == 2) {
        CHECK(gSegs[0].fmt.rate == 44100 && gSegs[0].len >= fa * 4 && memcmp(gSegs[0].data, a, fa * 4) == 0, "restart: first run");
        CHECK(gSegs[1].fmt.rate == 96000 && gSegs[1].len >= fb * 6 && memcmp(gSegs[1].data, b, fb * 6) == 0, "restart: second run");
        if (!failures) printf("ok   %-34s 2 segments, 2 markers, bit-exact\n", "restart with a new format");
    }
    pthread_mutex_unlock(&gRxMutex);

    reset_rx(SCREAM_DIALECT_EXTENDED);
    uint8_t *ab = malloc(fa * 4 * 2);
    memcpy(ab, a, fa * 4);
    for (size_t i = 0; i < fa * 4; i++) ab[fa * 4 + i] = (uint8_t)rnd();
    set_format(44100, kIntFlags, 16);
    play(ab, fa, 4, 44100);
    play(ab + fa * 4, fa, 4, 44100);
    usleep(500000);
    pthread_mutex_lock(&gRxMutex);
    /* the first run's partial last payload is not padded: the second run continues it */
    CHECK(gSegCount == 1 && gEOTs == 1, "same-format restart: %d segments, %u markers", gSegCount, gEOTs);
    if (gSegCount == 1)
        CHECK(gSegs[0].len >= fa * 8 && memcmp(gSegs[0].data, ab, fa * 8) == 0, "same-format restart: data");
    if (!failures) printf("ok   %-34s 1 segment, seamless\n", "restart with the same format");
    pthread_mutex_unlock(&gRxMutex);
    set_int('SMdl', 20);
    free(a); free(b); free(ab);
}


/* ---- Apple Music mode -------------------------------------------------------- */

static void add_client(UInt32 id, pid_t pid)
{
    AudioServerPlugInClientInfo c = {id, pid, true, NULL};
    (*gDrv)->AddDeviceClient(gDrv, kDevice, &c);
}

static void io(UInt32 client, UInt32 op, void *buf, UInt32 frames, Float64 t)
{
    AudioServerPlugInIOCycleInfo info = {0};
    info.mOutputTime.mSampleTime = t;
    info.mInputTime.mSampleTime = t;
    (*gDrv)->DoIOOperation(gDrv, kDevice, op == kAudioServerPlugInIOOperationReadInput ? 4 : kStream, client, op, frames, &info, buf, NULL);
}

static CFTypeRef status_value(CFStringRef key)
{
    static CFDictionaryRef last;
    if (last) CFRelease(last);
    last = get_cf('SMst');
    return last ? CFDictionaryGetValue(last, key) : NULL;
}

static long long status_int(CFStringRef key)
{
    long long v = -1;
    CFTypeRef n = status_value(key);
    if (n) CFNumberGetValue(n, kCFNumberLongLongType, &v);
    return v;
}

static void case_music_filter(void)
{
    add_client(11, 999001);   /* "Music" */
    add_client(12, 999002);   /* another app */
    add_client(13, 999003);   /* the controlling app */
    set_int('SMmx', 999001);
    usleep(100000);   /* let the property land */
    /* no SMac: the owner-death check would reset Music mode (999003 is not a real process) */
    set_format(44100, kFloatFlags, 32);

    /* The HAL calls StartIO once for the device: here the app's loopback reader starts IO first
     * and Music joins without a StartIO of its own (seen on macOS 27). */
    usleep(100000);   /* let the property land */
    reset_rx(SCREAM_DIALECT_EXTENDED);
    set_int('SMbd', 24);
    set_int('SMdl', 100);
    (*gDrv)->StartIO(gDrv, kDevice, 13);
    float zeros[512 * 2] = {0};
    Float64 t = 0;
    for (int i = 0; i < 40; i++) {     /* only the reader runs: nothing may go out */
        io(13, kAudioServerPlugInIOOperationReadInput, zeros, 512, t);
        io(13, kAudioServerPlugInIOOperationWriteMix, zeros, 512, t);
        t += 512;
        usleep(11000);
    }
    pthread_mutex_lock(&gRxMutex);
    CHECK(gSegCount == 0, "the loopback reader alone sent audio (%d segments)", gSegCount);
    pthread_mutex_unlock(&gRxMutex);

    /* Music and another app play: Music's samples go out exactly, the other app's go to the input */
    size_t frames = 44100;
    float *musicSrc = malloc(frames * 8);
    for (size_t i = 0; i < frames * 2; i++) musicSrc[i] = (float)((int32_t)(rnd() << 8) >> 8) / 8388608.0f;
    uint8_t *want = malloc(frames * 6);
    scream_pack_float(want, musicSrc, frames * 2, SCREAM_WIRE_S24_3LE);
    bool otherZeroed = true, loopbackOK = true;
    for (size_t done = 0; done < frames; done += 512) {
        UInt32 n = (UInt32)(frames - done < 512 ? frames - done : 512);
        float music[512 * 2], other[512 * 2], in[512 * 2];
        memcpy(music, musicSrc + done * 2, n * 8);
        for (UInt32 i = 0; i < n * 2; i++) other[i] = 0.25f;
        io(11, kAudioServerPlugInIOOperationProcessOutput, music, n, t);
        io(12, kAudioServerPlugInIOOperationProcessOutput, other, n, t);
        for (UInt32 i = 0; i < n * 2; i++) { if (other[i] != 0) otherZeroed = false; music[i] += other[i]; }   /* the HAL's mix */
        io(11, kAudioServerPlugInIOOperationWriteMix, music, n, t);
        io(13, kAudioServerPlugInIOOperationReadInput, in, n, t);
        for (UInt32 i = 0; i < n * 2; i++) if (in[i] != 0.25f) loopbackOK = false;
        t += n;
        usleep((useconds_t)(n * 1e6 / 44100));
    }
    CHECK(otherZeroed, "another app's buffer was not zeroed");
    CHECK(loopbackOK, "others loopback did not return the other app's audio");

    /* Music stops; the reader keeps the device running: the stream ends with a marker */
    for (int i = 0; i < 60; i++) {
        io(13, kAudioServerPlugInIOOperationReadInput, zeros, 512, t);
        io(13, kAudioServerPlugInIOOperationWriteMix, zeros, 512, t);
        t += 512;
        usleep(11000);
    }
    pthread_mutex_lock(&gRxMutex);
    CHECK(gSegCount == 1 && gEOTs == 1, "Music mode: %d segments, %u markers", gSegCount, gEOTs);
    if (gSegCount == 1) {
        CHECK(gSegs[0].len >= frames * 6 && memcmp(gSegs[0].data, want, frames * 6) == 0, "Music's samples not intact");
        bool silent = true;
        for (size_t i = frames * 6; i < gSegs[0].len; i++) if (gSegs[0].data[i]) silent = false;
        CHECK(silent, "something besides Music went out");
    }
    pthread_mutex_unlock(&gRxMutex);
    CHECK(status_value(CFSTR("running")) == kCFBooleanTrue, "the device should still run for the reader");
    (*gDrv)->StopIO(gDrv, kDevice, 13);
    free(musicSrc);
    free(want);
    set_int('SMmx', 0);
    set_int('SMdl', 20);
    if (!failures) printf("ok   %-34s reader alone silent, Music bit-exact, others looped back, marker\n", "Apple Music mode filter");
}

/* Float32 stream: track A, 50 ms of zeros, track B (Music's inter-track gap). */
static size_t make_tracks(float **out, size_t *aFrames, size_t *gapFrames, Float64 rate)
{
    *aFrames = (size_t)(rate * 0.5) + 17;
    *gapFrames = (size_t)(rate * 0.05);
    size_t bFrames = (size_t)(rate * 0.5);
    size_t total = *aFrames + *gapFrames + bFrames;
    float *f = calloc(total * 2, sizeof(float));
    for (size_t i = 0; i < *aFrames * 2; i++) f[i] = (float)((int32_t)(rnd() << 8) >> 8) / 8388608.0f;
    for (size_t i = (*aFrames + *gapFrames) * 2; i < total * 2; i++) f[i] = (float)((int32_t)(rnd() << 8) >> 8) / 8388608.0f;
    for (size_t i = 0; i < *aFrames * 2; i++) if (f[i] == 0) f[i] = 1.0f / 8388608.0f;   /* no zeros inside A */
    *out = f;
    return total;
}

static void feed(const float *src, size_t frames, Float64 rate, Float64 *t)
{
    for (size_t done = 0; done < frames;) {
        UInt32 n = (UInt32)(frames - done < 512 ? frames - done : 512);
        float buf[512 * 2];
        memcpy(buf, src + done * 2, n * 8);
        io(11, kAudioServerPlugInIOOperationWriteMix, buf, n, *t);
        *t += n;
        done += n;
        usleep((useconds_t)(n * 1e6 / rate));
    }
}

static void to_s24(uint8_t *dst, const float *src, size_t samples)
{
    scream_pack_float(dst, src, samples, SCREAM_WIRE_S24_3LE);
}

/* boundary hold, then drop: A arrives, B never does; one end-of-track marker */
static void case_boundary_drop(void)
{
    float *f;
    size_t aF, gF, total = make_tracks(&f, &aF, &gF, 48000);
    uint8_t *wantA = malloc(aF * 6);
    to_s24(wantA, f, aF * 2);
    reset_rx(SCREAM_DIALECT_EXTENDED);
    set_int('SMbd', 24);
    set_int('SMdl', 200);
    set_format(48000, kFloatFlags, 32);
    set_int('SMhd', 2);
    Float64 t = 0;
    (*gDrv)->StartIO(gDrv, kDevice, 11);
    feed(f, total, 48000, &t);
    usleep(300000);
    CHECK(status_int(CFSTR("holdMode")) == 2 && status_value(CFSTR("holdReached")) == kCFBooleanTrue, "boundary: not holding");
    set_int('SMhd', 3);
    usleep(100000);
    (*gDrv)->StopIO(gDrv, kDevice, 11);
    usleep(300000);
    set_int('SMhd', 0);
    pthread_mutex_lock(&gRxMutex);
    CHECK(gSegCount == 1 && gEOTs == 1, "boundary: %d segments, %u markers", gSegCount, gEOTs);
    if (gSegCount == 1) {
        rx_seg_t *s = &gSegs[0];
        CHECK(s->len >= aF * 6 && memcmp(s->data, wantA, aF * 6) == 0, "boundary: track A not intact");
        bool silent = true;
        for (size_t i = aF * 6; i < s->len; i++) if (s->data[i]) silent = false;
        CHECK(silent, "boundary: track B leaked after the hold point");
        CHECK(s->len < aF * 6 + SCREAM_PAYLOAD_SIZE, "boundary: %zu bytes after A", s->len - aF * 6);
        if (!failures) printf("ok   %-34s A intact, B dropped, 1 marker\n", "boundary hold + drop");
    }
    pthread_mutex_unlock(&gRxMutex);
    free(f); free(wantA);
}

/* boundary hold, then release (same rate after all): A, silence, then B intact */
static void case_boundary_release(void)
{
    float *f;
    size_t aF, gF, total = make_tracks(&f, &aF, &gF, 48000);
    size_t bF = total - aF - gF;
    uint8_t *want = malloc(total * 6);
    to_s24(want, f, total * 2);
    reset_rx(SCREAM_DIALECT_EXTENDED);
    set_int('SMhd', 2);
    Float64 t = 0;
    (*gDrv)->StartIO(gDrv, kDevice, 11);
    feed(f, aF + gF + 4800, 48000, &t);      /* into B */
    /* the next track's pre-roll while playing: a candidate for the grid check, no immediate switch */
    usleep(250000);
    CHECK(status_value(CFSTR("holdReached")) == kCFBooleanTrue, "release: not holding");
    set_int('SMhd', 0);
    feed(f + (aF + gF + 4800) * 2, total - (aF + gF + 4800), 48000, &t);
    (*gDrv)->StopIO(gDrv, kDevice, 11);
    usleep(600000);
    pthread_mutex_lock(&gRxMutex);
    CHECK(gSegCount == 1 && gEOTs == 1, "release: %d segments, %u markers", gSegCount, gEOTs);
    if (gSegCount == 1) {
        rx_seg_t *s = &gSegs[0];
        CHECK(memcmp(s->data, want, aF * 6) == 0, "release: track A not intact");
        /* B is somewhere after A, preceded only by silence (the gap, maybe some padding) */
        const uint8_t *wantB = want + (aF + gF) * 6;
        size_t bBytes = bF * 6, pos = aF * 6;
        while (pos < s->len && !s->data[pos]) pos++;
        pos -= pos % 6;
        while (pos > aF * 6 && memcmp(s->data + pos, wantB, 6) != 0) pos -= 6;
        CHECK(pos + bBytes <= s->len && memcmp(s->data + pos, wantB, bBytes) == 0, "release: track B not intact");
        if (!failures) printf("ok   %-34s A, gap, B intact\n", "boundary hold + release");
    }
    pthread_mutex_unlock(&gRxMutex);
    free(f); free(want);
}

/* gate: silence goes out, the first sound waits until released */
static void case_gate(void)
{
    size_t zF = 9600, aF = 24000;
    float *f = calloc((zF + aF) * 2, sizeof(float));
    for (size_t i = zF * 2; i < (zF + aF) * 2; i++) f[i] = (float)((int32_t)(rnd() << 8) >> 8) / 8388608.0f;
    f[zF * 2] = 0.5f;
    uint8_t *wantA = malloc(aF * 6);
    to_s24(wantA, f + zF * 2, aF * 2);
    reset_rx(SCREAM_DIALECT_EXTENDED);
    set_int('SMhd', 1);
    Float64 t = 0;
    (*gDrv)->StartIO(gDrv, kDevice, 11);
    feed(f, zF + aF / 2, 48000, &t);
    usleep(300000);
    pthread_mutex_lock(&gRxMutex);
    size_t before = gSegCount ? gSegs[0].len : 0;
    bool silent = true;
    for (size_t i = 0; i < before; i++) if (gSegs[0].data[i]) silent = false;
    pthread_mutex_unlock(&gRxMutex);
    CHECK(status_value(CFSTR("holdReached")) == kCFBooleanTrue, "gate: not holding");
    CHECK(silent, "gate: sound passed the gate");
    set_int('SMhd', 0);
    feed(f + (zF + aF / 2) * 2, aF - aF / 2, 48000, &t);
    (*gDrv)->StopIO(gDrv, kDevice, 11);
    usleep(600000);
    pthread_mutex_lock(&gRxMutex);
    if (gSegCount == 1) {
        rx_seg_t *s = &gSegs[0];
        size_t pos = 0;
        while (pos < s->len && !s->data[pos]) pos++;
        pos -= pos % 6;
        CHECK(pos + aF * 6 <= s->len && memcmp(s->data + pos, wantA, aF * 6) == 0, "gate: sound not intact after release");
        if (!failures) printf("ok   %-34s held at the first sound, intact after release\n", "gate");
    } else {
        CHECK(0, "gate: %d segments", gSegCount);
    }
    pthread_mutex_unlock(&gRxMutex);
    free(f); free(wantA);
}

/* Pause: the gate is armed while sound is still queued (the send delay holds 200 ms of it). That
 * sound and the silence after it go out; the first sound after the silence is held; when the device
 * stops while holding, the held sound is dropped and never sent later. */
static void case_pause_gate(void)
{
    size_t aF = 24000, zF = 9600, bF = 9600;    /* 0.5 s sound, 0.2 s silence, 0.2 s sound */
    float *f = calloc((aF + zF + bF) * 2, sizeof(float));
    for (size_t i = 0; i < aF * 2; i++) f[i] = (float)((int32_t)(rnd() << 8) >> 8) / 8388608.0f;
    for (size_t i = (aF + zF) * 2; i < (aF + zF + bF) * 2; i++) f[i] = 0.25f;
    for (size_t i = 0; i < aF * 2; i++) if (f[i] == 0) f[i] = 1.0f / 8388608.0f;
    uint8_t *wantA = malloc(aF * 6);
    to_s24(wantA, f, aF * 2);
    reset_rx(SCREAM_DIALECT_EXTENDED);
    set_int('SMbd', 24);
    set_int('SMdl', 200);
    set_format(48000, kFloatFlags, 32);
    Float64 t = 0;
    (*gDrv)->StartIO(gDrv, kDevice, 11);
    feed(f, aF, 48000, &t);
    set_int('SMhd', 1);                          /* "Paused" arrives with 200 ms of sound queued */
    feed(f + aF * 2, zF + bF, 48000, &t);
    usleep(400000);
    CHECK(status_value(CFSTR("holdReached")) == kCFBooleanTrue, "pause gate: not holding at the next sound");
    (*gDrv)->StopIO(gDrv, kDevice, 11);          /* Music stops its IO while we hold */
    usleep(300000);
    set_int('SMhd', 0);
    usleep(300000);
    pthread_mutex_lock(&gRxMutex);
    CHECK(gSegCount == 1 && gEOTs == 1, "pause gate: %d segments, %u markers", gSegCount, gEOTs);
    if (gSegCount == 1) {
        rx_seg_t *sg = &gSegs[0];
        CHECK(sg->len >= aF * 6 && memcmp(sg->data, wantA, aF * 6) == 0, "pause gate: the queued sound was cut");
        bool rest = true;
        for (size_t i = aF * 6; i < sg->len; i++) if (sg->data[i]) rest = false;
        CHECK(rest, "pause gate: the sound after the pause leaked out");
        if (!failures) printf("ok   %-34s queued sound intact, resumed sound held, dropped at stop\n", "pause gate");
    }
    pthread_mutex_unlock(&gRxMutex);
    set_int('SMdl', 20);
    free(f);
    free(wantA);
}

/* SMbd changed while running: the run restarts at the new depth (end-of-track in between) */
static void case_live_bits(void)
{
    size_t n = 24000;
    float *f = malloc(n * 2 * sizeof(float));
    for (size_t i = 0; i < n * 2; i++) f[i] = (float)((int16_t)rnd()) / 32768.0f;
    reset_rx(SCREAM_DIALECT_EXTENDED);
    set_int('SMdl', 20);
    set_int('SMbd', 24);
    Float64 t = 0;
    (*gDrv)->StartIO(gDrv, kDevice, 11);
    feed(f, n, 48000, &t);
    set_int('SMbd', 16);
    feed(f, n, 48000, &t);
    (*gDrv)->StopIO(gDrv, kDevice, 11);
    usleep(400000);
    pthread_mutex_lock(&gRxMutex);
    CHECK(gSegCount == 2 && gEOTs == 2, "live bits: %d segments, %u markers", gSegCount, gEOTs);
    if (gSegCount == 2) {
        CHECK(gSegs[0].fmt.sample_size == 24 && gSegs[1].fmt.sample_size == 16, "live bits: %u then %u", gSegs[0].fmt.sample_size, gSegs[1].fmt.sample_size);
        uint8_t *want16 = malloc(n * 4);
        scream_pack_float(want16, f, n * 2, SCREAM_WIRE_S16LE);
        CHECK(gSegs[1].len >= n * 4 && memcmp(gSegs[1].data, want16, n * 4) == 0, "live bits: 16-bit run not intact");
        free(want16);
        if (!failures) printf("ok   %-34s 24-bit run, marker, 16-bit run\n", "wire depth change while running");
    }
    pthread_mutex_unlock(&gRxMutex);
    set_int('SMbd', 0);
    free(f);
}

/* Music's buffer (4096 frames) is longer than the device's IO cycle (512): every cycle it covers
 * goes out, none is taken for silence */
static void case_music_long_buffers(void)
{
    add_client(21, 999021);
    set_int('SMmx', 999021);
    set_int('SMdl', 100);
    set_int('SMbd', 24);
    reset_rx(SCREAM_DIALECT_EXTENDED);
    set_format(48000, kFloatFlags, 32);
    size_t frames = 4096 * 12;
    float *src = malloc(frames * 8);
    for (size_t i = 0; i < frames * 2; i++) src[i] = (float)((int32_t)(rnd() << 8) >> 8) / 8388608.0f;
    uint8_t *want = malloc(frames * 6);
    scream_pack_float(want, src, frames * 2, SCREAM_WIRE_S24_3LE);
    long long skippedBefore = status_int(CFSTR("musicSkippedCycles"));
    (*gDrv)->StartIO(gDrv, kDevice, 21);
    Float64 t = 0;
    for (size_t done = 0; done < frames; done += 512) {
        if (done % 4096 == 0) {     /* Music renders a whole buffer ahead, once per 8 cycles */
            float big[4096 * 2];
            memcpy(big, src + done * 2, sizeof big);
            io(21, kAudioServerPlugInIOOperationProcessOutput, big, 4096, t);
        }
        float buf[512 * 2];
        memcpy(buf, src + done * 2, sizeof buf);
        io(21, kAudioServerPlugInIOOperationWriteMix, buf, 512, t);
        t += 512;
        usleep((useconds_t)(512 * 1e6 / 48000));
    }
    (*gDrv)->StopIO(gDrv, kDevice, 21);
    long long skipped = status_int(CFSTR("musicSkippedCycles")) - skippedBefore;
    CHECK(skipped == 0, "long buffers: %lld cycles taken for silence", skipped);
    expect_rx("Music with 4096-frame buffers", 48000, 24, want, frames * 6);
    set_int('SMmx', 0);
    set_int('SMdl', 20);
    free(src);
    free(want);
}

/* the grid fit reports a plain gain (e.g. a volume a hair below unity) */
static void case_gain_fit(void)
{
    size_t frames = 24000;
    float *f = malloc(frames * 8);
    for (size_t i = 0; i < frames * 2; i++) {
        int32_t v = (int16_t)rnd();
        f[i] = (float)((double)v / 32768.0 * 0.99999);
    }
    set_int('SMbd', 16);
    set_format(48000, kFloatFlags, 32);
    (*gDrv)->StartIO(gDrv, kDevice, 1);
    Float64 t = 0;
    for (size_t done = 0; done < frames; done += 512) {
        float buf[512 * 2];
        UInt32 n = (UInt32)(frames - done < 512 ? frames - done : 512);
        memcpy(buf, f + done * 2, n * 8);
        io(1, kAudioServerPlugInIOOperationWriteMix, buf, n, t);
        t += n;
    }
    double g = 0;
    CFTypeRef gn = status_value(CFSTR("runGain"));
    if (gn) CFNumberGetValue(gn, kCFNumberDoubleType, &g);
    (*gDrv)->StopIO(gDrv, kDevice, 1);
    usleep(300000);
    CHECK(g > 0.999985 && g < 0.999995, "gain fit %.9f, want ~0.99999", g);
    if (!failures) printf("ok   %-34s %.9f\n", "grid fit finds a plain gain", g);
    set_int('SMbd', 0);
    free(f);
}

/* the mirror gets every packet even when the destination is elsewhere */
static void case_mirror(void)
{
    int port = 14099, mport = PORT;
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &port), mp = CFNumberCreate(NULL, kCFNumberIntType, &mport);
    const void *k[] = {CFSTR("ip"), CFSTR("port")};
    const void *dv[] = {CFSTR("127.0.0.1"), p}, *mv[] = {CFSTR("127.0.0.1"), mp};
    CFDictionaryRef dest = CFDictionaryCreate(NULL, k, dv, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionaryRef mirror = CFDictionaryCreate(NULL, k, mv, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    set_cf('SMds', dest);
    set_cf('SMmr', mirror);
    size_t frames = 22050 + 7;
    uint8_t *src = malloc(frames * 4);
    for (size_t i = 0; i < frames * 4; i++) src[i] = (uint8_t)rnd();
    reset_rx(SCREAM_DIALECT_EXTENDED);
    set_format(44100, kIntFlags, 16);
    play(src, frames, 4, 44100);
    expect_rx("mirror copy of the stream", 44100, 16, src, frames * 4);
    const void *ov[] = {CFSTR(""), p};
    CFDictionaryRef off = CFDictionaryCreate(NULL, k, ov, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    set_cf('SMmr', off);
    set_destination("extended");
    CFRelease(dest); CFRelease(mirror); CFRelease(off); CFRelease(p); CFRelease(mp);
    free(src);
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s ScreamMusic.driver\n", argv[0]); return 2; }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int rcvbuf = 8 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
    pthread_t rx;
    pthread_create(&rx, NULL, rx_main, &fd);

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[1], (CFIndex)strlen(argv[1]), true);
    CFBundleRef bundle = CFBundleCreate(NULL, url);
    if (!bundle || !CFBundleLoadExecutable(bundle)) { fprintf(stderr, "cannot load %s\n", argv[1]); return 1; }
    void *(*create)(CFAllocatorRef, CFUUIDRef) = CFBundleGetFunctionPointerForName(bundle, CFSTR("ScreamMusic_Create"));
    if (!create) { fprintf(stderr, "no ScreamMusic_Create\n"); return 1; }
    gDrv = create(NULL, kAudioServerPlugInTypeUUID);
    if (!gDrv) { fprintf(stderr, "factory returned NULL\n"); return 1; }

    gStorage = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CHECK((*gDrv)->Initialize(gDrv, &gHostIf) == 0, "Initialize");
    set_int('SMdl', 20);

    case_int(44100, 16, "int16 44.1k passthrough");
    case_int(96000, 24, "int24 96k passthrough");
    case_int(192000, 32, "int32 192k passthrough");
    case_float(44100, 16, 16, SCREAM_WIRE_S16LE, "extended", "float 16-bit src -> S16 44.1k");
    case_float(48000, 24, 24, SCREAM_WIRE_S24_3LE, "extended", "float 24-bit src -> S24_3LE 48k");
    case_float(88200, 16, 0, SCREAM_WIRE_S24_3LE, "extended", "float 16-bit src -> S24 (auto) 88.2k");
    case_float(176400, 24, 0, SCREAM_WIRE_S32LE, "original", "float 24-bit src -> S32 original");
    set_destination("extended");
    case_restart();
    case_music_filter();
    case_boundary_drop();
    case_boundary_release();
    case_gate();
    case_pause_gate();
    case_live_bits();
    case_mirror();
    case_music_long_buffers();
    case_gain_fit();

    /* settings persisted through the host's storage */
    CHECK(CFDictionaryGetValue(gStorage, CFSTR("destination")) != NULL, "destination not stored");
    CFDictionaryRef status = get_cf('SMst');
    CHECK(status && CFGetTypeID(status) == CFDictionaryGetTypeID(), "status");
    if (status) {
        long long eots = 0, errs = -1;
        CFNumberGetValue(CFDictionaryGetValue(status, CFSTR("endOfTrackMarkers")), kCFNumberLongLongType, &eots);
        CFNumberGetValue(CFDictionaryGetValue(status, CFSTR("sendErrors")), kCFNumberLongLongType, &errs);
        CHECK(eots == 20, "status endOfTrackMarkers %lld", eots);
        CHECK(errs == 0, "status sendErrors %lld", errs);
        CFRelease(status);
    }

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("driver harness: all cases passed\n");
    return 0;
}
