/*
 * smctl: command-line access to the ScreamMusic driver's properties (testing, scripting).
 *
 *   smctl status                     driver status (SMst) and device format
 *   smctl dest [ip port [header]]    show / set the receiver (header: extended | original)
 *   smctl bits N                     Float32 input wire depth: 0 auto, 16, 24, 32
 *   smctl delay MS                   send delay in milliseconds
 *   smctl rate HZ                    device nominal sample rate
 *   smctl eot                        send an end-of-track marker now
 *   smctl mirror ip port | off       also send every packet to ip:port (diagnostics)
 *
 * License: GPL-3.0 (see ../../LICENSE).
 */
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static AudioObjectPropertyAddress addr(AudioObjectPropertySelector s)
{
    AudioObjectPropertyAddress a = {s, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    return a;
}

static AudioObjectID find_device(void)
{
    CFStringRef u = CFSTR("ScreamMusic_UID");
    AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyTranslateUIDToDevice);
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 size = sizeof dev;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof u, &u, &size, &dev);
    return dev;
}

static CFPropertyListRef get(AudioObjectID dev, AudioObjectPropertySelector sel)
{
    AudioObjectPropertyAddress a = addr(sel);
    CFPropertyListRef v = NULL;
    UInt32 size = sizeof v;
    OSStatus st = AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &v);
    if (st) fprintf(stderr, "get failed: %d\n", (int)st);
    return v;
}

static int set(AudioObjectID dev, AudioObjectPropertySelector sel, CFPropertyListRef v)
{
    AudioObjectPropertyAddress a = addr(sel);
    OSStatus st = AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof v, &v);
    if (st) fprintf(stderr, "set failed: %d\n", (int)st);
    return st ? 1 : 0;
}

static int set_int(AudioObjectID dev, AudioObjectPropertySelector sel, int v)
{
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &v);
    int r = set(dev, sel, n);
    CFRelease(n);
    return r;
}

static void show(CFPropertyListRef v)
{
    if (!v) return;
    CFDataRef xml = CFPropertyListCreateData(NULL, v, kCFPropertyListXMLFormat_v1_0, 0, NULL);
    if (xml) {
        fwrite(CFDataGetBytePtr(xml), 1, (size_t)CFDataGetLength(xml), stdout);
        CFRelease(xml);
    }
    CFRelease(v);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: smctl status | dest [ip port [header]] | bits N | delay MS | rate HZ | eot | mirror ip port|off\n");
        return 2;
    }
    AudioObjectID dev = find_device();
    if (dev == kAudioObjectUnknown) { fprintf(stderr, "ScreamMusic device not found (driver not installed?)\n"); return 1; }
    const char *cmd = argv[1];

    if (!strcmp(cmd, "status")) {
        show(get(dev, 'SMst'));
        return 0;
    }
    if (!strcmp(cmd, "dest")) {
        if (argc >= 4) {
            int port = atoi(argv[3]);
            CFStringRef ip = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
            CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &port);
            CFStringRef h = CFStringCreateWithCString(NULL, argc >= 5 ? argv[4] : "extended", kCFStringEncodingUTF8);
            const void *k[] = {CFSTR("ip"), CFSTR("port"), CFSTR("header")};
            const void *vals[] = {ip, p, h};
            CFDictionaryRef d = CFDictionaryCreate(NULL, k, vals, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            int r = set(dev, 'SMds', d);
            CFRelease(d); CFRelease(ip); CFRelease(p); CFRelease(h);
            if (r) return r;
        }
        show(get(dev, 'SMds'));
        return 0;
    }
    if (!strcmp(cmd, "bits") && argc == 3) return set_int(dev, 'SMbd', atoi(argv[2]));
    if (!strcmp(cmd, "delay") && argc == 3) return set_int(dev, 'SMdl', atoi(argv[2]));
    if (!strcmp(cmd, "eot")) return set_int(dev, 'SMeo', 1);
    if (!strcmp(cmd, "mirror") && argc >= 3) {
        int port = argc >= 4 ? atoi(argv[3]) : 0;
        CFStringRef ip = CFStringCreateWithCString(NULL, strcmp(argv[2], "off") ? argv[2] : "", kCFStringEncodingUTF8);
        CFNumberRef p = CFNumberCreate(NULL, kCFNumberIntType, &port);
        const void *k[] = {CFSTR("ip"), CFSTR("port")};
        const void *vals[] = {ip, p};
        CFDictionaryRef d = CFDictionaryCreate(NULL, k, vals, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        int r = set(dev, 'SMmr', d);
        CFRelease(d); CFRelease(ip); CFRelease(p);
        return r;
    }
    if (!strcmp(cmd, "rate") && argc == 3) {
        Float64 r = atof(argv[2]);
        AudioObjectPropertyAddress a = addr(kAudioDevicePropertyNominalSampleRate);
        OSStatus st = AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof r, &r);
        if (st) fprintf(stderr, "set rate failed: %d\n", (int)st);
        return st ? 1 : 0;
    }
    fprintf(stderr, "unknown command\n");
    return 2;
}
