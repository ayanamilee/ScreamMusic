/*
 * hogplay: plays a 16/24/32-bit PCM WAV to a CoreAudio output device the way a bit-perfect
 * player does, to test ScreamMusic end to end:
 *   - sets the device's nominal rate to the file's,
 *   - optionally takes hog mode and sets a non-mixable integer physical format,
 *   - writes the samples from an IOProc.
 *
 *   hogplay [-d device-uid] [-m int|float] [-H] file.wav
 *     -d uid    output device UID (default ScreamMusic_UID)
 *     -m int    integer physical format of the file's depth (implies -H)  [default]
 *     -m float  Float32 (mixable); samples written as v / 2^(N-1)
 *     -H        take hog mode (exclusive access)
 *
 * License: GPL-3.0 (see ../../LICENSE).
 */
#include <CoreAudio/CoreAudio.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    uint32_t rate;
    unsigned bits, bytes, channels;
    uint8_t *data;
    size_t frames;
} wav_t;

static int read_wav(const char *path, wav_t *w)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { fclose(f); return -1; }
    fclose(f);
    if (len < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) { fprintf(stderr, "%s: not a WAV file\n", path); return -1; }
    size_t p = 12;
    bool fmt = false;
    while (p + 8 <= (size_t)len) {
        uint32_t sz = buf[p + 4] | buf[p + 5] << 8 | buf[p + 6] << 16 | (uint32_t)buf[p + 7] << 24;
        if (!memcmp(buf + p, "fmt ", 4)) {
            uint16_t tag = buf[p + 8] | buf[p + 9] << 8;
            w->channels = buf[p + 10] | buf[p + 11] << 8;
            w->rate = buf[p + 12] | buf[p + 13] << 8 | buf[p + 14] << 16 | (uint32_t)buf[p + 15] << 24;
            w->bits = buf[p + 22] | buf[p + 23] << 8;
            if (tag == 0xFFFE && sz >= 40) tag = buf[p + 32] | buf[p + 33] << 8;   /* WAVE_FORMAT_EXTENSIBLE */
            if (tag != 1) { fprintf(stderr, "%s: not integer PCM\n", path); return -1; }
            fmt = true;
        } else if (!memcmp(buf + p, "data", 4) && fmt) {
            w->bytes = w->bits / 8;
            size_t avail = (size_t)len - (p + 8) < sz ? (size_t)len - (p + 8) : sz;
            w->frames = avail / (w->bytes * w->channels);
            w->data = buf + p + 8;
            if (w->channels != 2 || (w->bits != 16 && w->bits != 24 && w->bits != 32)) {
                fprintf(stderr, "%s: need stereo 16/24/32-bit, got %u ch %u-bit\n", path, w->channels, w->bits);
                return -1;
            }
            return 0;
        }
        p += 8 + sz + (sz & 1);
    }
    fprintf(stderr, "%s: no data chunk\n", path);
    return -1;
}

static AudioObjectPropertyAddress addr(AudioObjectPropertySelector s, AudioObjectPropertyScope sc)
{
    AudioObjectPropertyAddress a = {s, sc, kAudioObjectPropertyElementMain};
    return a;
}

static AudioObjectID find_device(const char *uid)
{
    CFStringRef u = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal);
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 size = sizeof dev;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof u, &u, &size, &dev);
    CFRelease(u);
    return dev;
}

static AudioObjectID output_stream(AudioObjectID dev)
{
    AudioObjectPropertyAddress a = addr(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
    AudioObjectID s[8];
    UInt32 size = sizeof s;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, s) || size < sizeof s[0]) return kAudioObjectUnknown;
    return s[0];
}

static wav_t gWav;
static bool gFloat;
static _Atomic size_t gPos;
static _Atomic bool gDone;
static AudioStreamBasicDescription gFmt;

static OSStatus ioproc(AudioObjectID dev, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *inTime,
                       AudioBufferList *out, const AudioTimeStamp *outTime, void *ctx)
{
    (void)dev; (void)now; (void)in; (void)inTime; (void)outTime; (void)ctx;
    AudioBuffer *b = &out->mBuffers[0];
    size_t frameBytes = gFmt.mBytesPerFrame;
    size_t frames = b->mDataByteSize / frameBytes;
    size_t pos = atomic_load(&gPos);
    size_t n = gWav.frames - pos < frames ? gWav.frames - pos : frames;
    memset(b->mData, 0, b->mDataByteSize);
    if (gFloat) {
        float *o = b->mData;
        double scale = (double)(1u << (gWav.bits - 1));
        for (size_t i = 0; i < n * 2; i++) {
            const uint8_t *s = gWav.data + (pos * 2 + i) * gWav.bytes;
            int32_t v = 0;
            for (unsigned k = 0; k < gWav.bytes; k++) v |= (int32_t)((uint32_t)s[k] << (8 * k + (32 - 8 * gWav.bytes)));
            v >>= 32 - 8 * gWav.bytes;
            o[i] = (float)((double)v / scale);
        }
    } else {
        memcpy(b->mData, gWav.data + pos * frameBytes, n * frameBytes);   /* little-endian WAV = native */
    }
    atomic_store(&gPos, pos + n);
    if (pos + n >= gWav.frames) atomic_store(&gDone, true);
    return 0;
}

int main(int argc, char **argv)
{
    const char *uid = "ScreamMusic_UID";
    bool hog = false;
    int opt;
    while ((opt = getopt(argc, argv, "d:m:H")) != -1) {
        switch (opt) {
        case 'd': uid = optarg; break;
        case 'm': gFloat = !strcmp(optarg, "float"); break;
        case 'H': hog = true; break;
        default: fprintf(stderr, "usage: %s [-d uid] [-m int|float] [-H] file.wav\n", argv[0]); return 2;
        }
    }
    if (optind >= argc) { fprintf(stderr, "usage: %s [-d uid] [-m int|float] [-H] file.wav\n", argv[0]); return 2; }
    if (!gFloat) hog = true;
    if (read_wav(argv[optind], &gWav)) return 1;

    AudioObjectID dev = find_device(uid);
    if (dev == kAudioObjectUnknown) { fprintf(stderr, "device %s not found\n", uid); return 1; }
    AudioObjectID stream = output_stream(dev);

    pid_t me = getpid();
    if (hog) {
        AudioObjectPropertyAddress a = addr(kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeOutput);
        pid_t p = me;
        OSStatus st = AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof p, &p);
        UInt32 size = sizeof p;
        AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &p);
        printf("hog mode: %s (owner pid %d)\n", st == 0 && p == me ? "taken" : "NOT taken", p);
    }

    memset(&gFmt, 0, sizeof gFmt);
    gFmt.mSampleRate = gWav.rate;
    gFmt.mFormatID = kAudioFormatLinearPCM;
    gFmt.mChannelsPerFrame = 2;
    gFmt.mFramesPerPacket = 1;
    if (gFloat) {
        gFmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
        gFmt.mBitsPerChannel = 32;
    } else {
        gFmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonMixable;
        gFmt.mBitsPerChannel = gWav.bits;
    }
    gFmt.mBytesPerFrame = gFmt.mBytesPerPacket = gFmt.mBitsPerChannel / 8 * 2;

    AudioObjectPropertyAddress pf = addr(kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal);
    OSStatus st = AudioObjectSetPropertyData(stream, &pf, 0, NULL, sizeof gFmt, &gFmt);
    if (st) { fprintf(stderr, "set physical format failed: %d\n", (int)st); return 1; }
    usleep(300000);   /* let the configuration change land */

    AudioStreamBasicDescription phys, virt;
    UInt32 size = sizeof phys;
    AudioObjectGetPropertyData(stream, &pf, 0, NULL, &size, &phys);
    AudioObjectPropertyAddress vf = addr(kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal);
    size = sizeof virt;
    AudioObjectGetPropertyData(stream, &vf, 0, NULL, &size, &virt);
    printf("file: %u Hz %u-bit, %zu frames\n", gWav.rate, gWav.bits, gWav.frames);
    printf("physical: %.0f Hz %u-bit flags 0x%x | virtual (IOProc): %.0f Hz %u-bit flags 0x%x\n",
           phys.mSampleRate, (unsigned)phys.mBitsPerChannel, (unsigned)phys.mFormatFlags,
           virt.mSampleRate, (unsigned)virt.mBitsPerChannel, (unsigned)virt.mFormatFlags);
    if (virt.mBitsPerChannel != gFmt.mBitsPerChannel || virt.mFormatFlags != gFmt.mFormatFlags || virt.mSampleRate != gWav.rate) {
        fprintf(stderr, "the IOProc format is not the requested one; refusing to play\n");
        return 1;
    }
    gFmt = virt;

    AudioDeviceIOProcID proc;
    AudioDeviceCreateIOProcID(dev, ioproc, NULL, &proc);
    AudioDeviceStart(dev, proc);
    while (!atomic_load(&gDone)) usleep(20000);
    usleep(200000);   /* last buffer */
    AudioDeviceStop(dev, proc);
    AudioDeviceDestroyIOProcID(dev, proc);
    if (hog) {
        AudioObjectPropertyAddress a = addr(kAudioDevicePropertyHogMode, kAudioObjectPropertyScopeOutput);
        pid_t p = -1;
        AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof p, &p);
    }
    printf("played %zu frames\n", atomic_load(&gPos));
    return 0;
}
