/*
 * ScreamMusic.driver: AudioServerPlugIn that sends what is played into it over the Scream
 * protocol (UDP). Structure follows Apple's NullAudio sample (MIT); see DESIGN.md §4.
 *
 * Objects: the plug-in, one device "ScreamMusic", a 2-channel output stream and a 2-channel
 * input stream (Apple Music mode: other apps' sound, read back by the app).
 *
 * Data path:
 *   DoIOOperation(ProcessOutput)  [HAL IO thread, real time, per client, before the mix]
 *     Apple Music mode: every client but Music -> the "others" loopback ring, zeroed
 *   DoIOOperation(WriteMix)
 *     physical-format frames -> wire bytes (Shared/scream_protocol.h) -> SPSC ring
 *   DoIOOperation(ReadInput)
 *     the "others" ring -> input stream
 *   sender thread
 *     ring -> 1152-byte payloads, paced to payload duration -> sendto();
 *     holds at a track boundary or drops a wrong-rate start on request (SMhd)
 *
 * The IO thread takes no locks, allocates nothing and makes no system calls.
 *
 * License: GPL-3.0 (see ../LICENSE). Portions derived from Apple's NullAudio sample (MIT).
 */

#include "../Shared/scream_protocol.h"

#include <CoreAudio/AudioServerPlugIn.h>
#include <arpa/inet.h>
#include <errno.h>
#include <mach/mach_init.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <netinet/in.h>
#include <os/log.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <unistd.h>

/* ---- identity ------------------------------------------------------------ */

#define kDriver_Name          "ScreamMusic"
#define kDriver_Manufacturer  "ScreamMusic"
#define kDevice_UID           "ScreamMusic_UID"
#define kDevice_ModelUID      "ScreamMusic_ModelUID"

enum {
    kObjectID_PlugIn        = kAudioObjectPlugInObject,
    kObjectID_Device        = 2,
    kObjectID_Stream_Output = 3,
    kObjectID_Stream_Input  = 4,
};

/* Custom properties (DESIGN.md §4.4). All are CFPropertyList-typed. */
enum {
    kSM_Destination = 'SMds',   /* CFDictionary {ip, port, header} */
    kSM_Status      = 'SMst',   /* CFDictionary, read only */
    kSM_EndOfTrack  = 'SMeo',   /* CFNumber, write 1 */
    kSM_SendDelay   = 'SMdl',   /* CFNumber, milliseconds */
    kSM_WireBits    = 'SMbd',   /* CFNumber, Float32 input: 0 auto, 16/24/32 */
    kSM_MusicPID    = 'SMmx',   /* CFNumber, Apple Music mode: only this pid is sent; 0 = no Music */
    kSM_Hold        = 'SMhd',   /* CFNumber, hold point: 0 run, 1 gate, 2 boundary, 3 drop */
    kSM_HoldReached = 'SMhr',   /* CFNumber, read only: 1 = holding */
    kSM_Owner       = 'SMac',   /* CFNumber, the controlling app's pid (cleanup when it dies) */
    kSM_Mirror      = 'SMmr',   /* CFDictionary {ip, port}: also send every packet there (diagnostics); {} = off */
    kSM_LeadIn      = 'SMli',   /* CFNumber, milliseconds of silence before a new wire format (relock) */
    kSM_LeadOnce    = 'SMlo',   /* CFNumber, ms of lead-in for the next format change only (0 = none); expires */
};

enum { kHold_Run = 0, kHold_Gate = 1, kHold_Boundary = 2, kHold_Drop = 3 };

static const Float64 kRates[] = {44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};
#define kNumRates (sizeof kRates / sizeof kRates[0])

/* Physical formats. Float32 is mixable (the default); integers are non-mixable, for players
 * that hog the device in integer mode, so their samples reach WriteMix untouched. */
typedef enum { kFmt_Float32 = 0, kFmt_SInt16, kFmt_SInt24, kFmt_SInt32, kNumFormats } fmt_t;

#define kChannels             2
#define kZeroTimeStampPeriod  16384u
#define kDefaultDelayMs       20u
#define kMaxDelayMs           1000u
#define kDefaultLeadInMs      500u
#define kMaxLeadInMs          2000u
#define kLeadFillSec          1.5          /* after the lead-in, zeros until the run's first audio */
#define kLeadOnceSec          3            /* SMlo: how long it waits for the format change it was set for */
#define kRingBytes            (1u << 22)   /* 4 MiB: > 1 s at 384 kHz S32 stereo */
#define kMaxClients           64
#define kClientSlots          256          /* client ID -> pid, read on the IO thread */
#define kOthersFrames         65536u       /* others loopback, indexed by sample time */
#define kHoldTimeoutSec       4            /* a gate/boundary hold left by the app is released */
#define kDropTimeoutSec       10

static os_log_t gLog;

/* ---- driver interface plumbing ------------------------------------------ */

static HRESULT  SM_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface);
static ULONG    SM_AddRef(void *inDriver);
static ULONG    SM_Release(void *inDriver);
static OSStatus SM_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus SM_CreateDevice(AudioServerPlugInDriverRef, CFDictionaryRef, const AudioServerPlugInClientInfo *, AudioObjectID *);
static OSStatus SM_DestroyDevice(AudioServerPlugInDriverRef, AudioObjectID);
static OSStatus SM_AddDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus SM_RemoveDeviceClient(AudioServerPlugInDriverRef, AudioObjectID, const AudioServerPlugInClientInfo *);
static OSStatus SM_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static OSStatus SM_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef, AudioObjectID, UInt64, void *);
static Boolean  SM_HasProperty(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *);
static OSStatus SM_IsPropertySettable(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, Boolean *);
static OSStatus SM_GetPropertyDataSize(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32 *);
static OSStatus SM_GetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, UInt32 *, void *);
static OSStatus SM_SetPropertyData(AudioServerPlugInDriverRef, AudioObjectID, pid_t, const AudioObjectPropertyAddress *, UInt32, const void *, UInt32, const void *);
static OSStatus SM_StartIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus SM_StopIO(AudioServerPlugInDriverRef, AudioObjectID, UInt32);
static OSStatus SM_GetZeroTimeStamp(AudioServerPlugInDriverRef, AudioObjectID, UInt32, Float64 *, UInt64 *, UInt64 *);
static OSStatus SM_WillDoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, Boolean *, Boolean *);
static OSStatus SM_BeginIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);
static OSStatus SM_DoIOOperation(AudioServerPlugInDriverRef, AudioObjectID, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *, void *, void *);
static OSStatus SM_EndIOOperation(AudioServerPlugInDriverRef, AudioObjectID, UInt32, UInt32, UInt32, const AudioServerPlugInIOCycleInfo *);

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    SM_QueryInterface, SM_AddRef, SM_Release,
    SM_Initialize, SM_CreateDevice, SM_DestroyDevice,
    SM_AddDeviceClient, SM_RemoveDeviceClient,
    SM_PerformDeviceConfigurationChange, SM_AbortDeviceConfigurationChange,
    SM_HasProperty, SM_IsPropertySettable, SM_GetPropertyDataSize, SM_GetPropertyData, SM_SetPropertyData,
    SM_StartIO, SM_StopIO, SM_GetZeroTimeStamp,
    SM_WillDoIOOperation, SM_BeginIOOperation, SM_DoIOOperation, SM_EndIOOperation,
};
static AudioServerPlugInDriverInterface *gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef gDriverRef = &gInterfacePtr;
static UInt32 gRefCount;
static AudioServerPlugInHostRef gHost;

/* ---- state ---------------------------------------------------------------- */

/* Guarded by gStateMutex (property and configuration threads). Lock order: gIOMutex, then
 * gStateMutex, then gDestMutex. */
static pthread_mutex_t gStateMutex = PTHREAD_MUTEX_INITIALIZER;
static Float64 gRate = 44100;
static fmt_t gFormat = kFmt_Float32;
static Float64 gPendingRate;
static int gPendingFormat = -1;
static struct {
    pid_t pid;
    UInt32 clientID;
    char bundle[128];
} gClients[kMaxClients];
static int gClientCount;

/* client ID -> pid for the IO thread: (clientID << 32 | pid), 0 = empty */
static _Atomic uint64_t gClientPIDs[kClientSlots];
/* host time of each client's last ProcessOutput (who is actually playing), by client slot */
static _Atomic uint64_t gClientLastOutput[kClientSlots];
/* Apple Music mode: the sample time up to which Music has handed the HAL output (its last
 * ProcessOutput's time + frames). The HAL calls StartIO once for the device, not per client, so
 * whether Music plays is told by its output; Music's buffers can be longer than the device's IO
 * cycle, so a WriteMix cycle counts as Music's if it lies inside what Music has covered. */
static _Atomic int64_t gMusicCoveredUntil = INT64_MIN;
static _Atomic int64_t gDiagMusicSample, gDiagMusicFrames, gDiagMixSample, gDiagMixFrames;
static _Atomic uint64_t gStatGateSkipped;

/* Destination, guarded by gDestMutex (property thread writes, sender reads). */
static pthread_mutex_t gDestMutex = PTHREAD_MUTEX_INITIALIZER;
static char gDestIP[64] = "127.0.0.1";
static unsigned gDestPort = SCREAM_DEFAULT_PORT;
static scream_dialect_t gDialect = SCREAM_DIALECT_EXTENDED;
static _Atomic unsigned gDestGeneration;
static char gMirrorIP[64];                 /* "" = no mirror; guarded by gDestMutex */
static unsigned gMirrorPort;

static _Atomic unsigned gDelayMs = kDefaultDelayMs;
static _Atomic unsigned gLeadInMs = kDefaultLeadInMs;
static _Atomic int gLeadOnceMs = -1;             /* SMlo; -1 = not set */
static _Atomic uint64_t gLeadOnceUntil;          /* host time after which it no longer applies */
static _Atomic unsigned gWantBits;         /* SMbd: 0 = auto */
static _Atomic int gMusicPID;              /* SMmx */
static _Atomic int gOwnerPID;              /* SMac */
static _Atomic int gHoldMode;              /* SMhd */
static _Atomic unsigned gHoldSerial;       /* bumped on every SMhd write */
static _Atomic bool gHoldReached;          /* SMhr */

/* Clock (GetZeroTimeStamp), guarded by gIOMutex. */
static pthread_mutex_t gIOMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt32 gIOCount;                    /* StartIO count: the clock and the run */
static UInt64 gAnchorHost;
static UInt64 gNumTimeStamps;
static UInt64 gClockSeed = 1;
static Float64 gHostTicksPerFrame;
static Float64 gHostTicksPerSecond;

/* Stream snapshot, published at the start of each run (StartIO, or a configuration change while
 * running); read by the IO and sender threads. IO is stopped while it changes. */
typedef struct {
    uint32_t rate;
    fmt_t format;
    scream_wire_t wire;
    scream_dialect_t dialect;
} stream_t;
static stream_t gStream;

/* SPSC ring: IO thread writes, sender reads. Byte counters only grow. */
static uint8_t gRing[kRingBytes];
static _Atomic uint64_t gRingWrite, gRingRead;

/* Others loopback (Apple Music mode), IO thread only. */
static Float32 gOthers[kOthersFrames * kChannels];
static Float64 gOthersThrough = -1.0;

/* Sender control. A new run publishes gStream and gStartPos (the ring position where its bytes
 * begin), then bumps gStartGen; the sender, the ring's only consumer, does the hand-over. */
static pthread_mutex_t gSendMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gSendCond = PTHREAD_COND_INITIALIZER;
static _Atomic bool gRunning;
static _Atomic unsigned gStartGen;
static _Atomic uint64_t gStartPos;
static _Atomic bool gEOTRequest;
static _Atomic bool gSenderStarted;

/* Statistics for SMst. "Run" counters restart with each run. */
static _Atomic uint64_t gStatPackets, gStatBytes, gStatSendErrors, gStatUnderruns, gStatOverflowBytes;
static _Atomic uint64_t gStatFramesIn, gStatOffGrid, gStatEOTs, gStatRunFrames, gStatRunOffGrid;
static _Atomic uint64_t gStatOthersFrames;
/* Float32 input, this run: least-squares gain of the samples against their nearest grid values,
 * and the mean distance from the grid (LSB at the wire depth). A plain gain (Music's own scaling,
 * a volume below 100) shows as gain != 1; resampling or EQ as ~0.25 LSB of scattered deviation.
 * IO thread accumulates; published as double bit patterns. */
static double gFitYR, gFitRR, gFitDev, gFitN;
static _Atomic uint64_t gStatRunGain, gStatRunDev;
static _Atomic int gStatLastErrno;

/* ---- helpers --------------------------------------------------------------- */

static void fill_asbd(AudioStreamBasicDescription *d, Float64 rate, fmt_t f)
{
    memset(d, 0, sizeof *d);
    d->mSampleRate = rate;
    d->mFormatID = kAudioFormatLinearPCM;
    d->mFramesPerPacket = 1;
    d->mChannelsPerFrame = kChannels;
    switch (f) {
    case kFmt_Float32:
        d->mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
        d->mBitsPerChannel = 32;
        break;
    case kFmt_SInt16:
        d->mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonMixable;
        d->mBitsPerChannel = 16;
        break;
    case kFmt_SInt24:
        d->mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonMixable;
        d->mBitsPerChannel = 24;
        break;
    case kFmt_SInt32:
    default:
        d->mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonMixable;
        d->mBitsPerChannel = 32;
        break;
    }
    d->mBytesPerFrame = d->mBitsPerChannel / 8 * kChannels;
    d->mBytesPerPacket = d->mBytesPerFrame;
}

static bool rate_supported(Float64 r)
{
    for (size_t i = 0; i < kNumRates; i++)
        if (kRates[i] == r) return true;
    return false;
}

static int format_from_asbd(const AudioStreamBasicDescription *d)
{
    if (d->mFormatID != kAudioFormatLinearPCM || d->mChannelsPerFrame != kChannels) return -1;
    for (int f = 0; f < kNumFormats; f++) {
        AudioStreamBasicDescription c;
        fill_asbd(&c, d->mSampleRate, (fmt_t)f);
        /* tolerate a client leaving IsNonMixable unset */
        UInt32 mask = ~(UInt32)(kAudioFormatFlagIsNonMixable);
        if ((d->mFormatFlags & mask) == (c.mFormatFlags & mask) &&
            d->mBitsPerChannel == c.mBitsPerChannel && d->mBytesPerFrame == c.mBytesPerFrame)
            return f;
    }
    return -1;
}

static unsigned format_bits(fmt_t f)
{
    switch (f) {
    case kFmt_SInt16: return 16;
    case kFmt_SInt24: return 24;
    default:          return 32;
    }
}

static unsigned format_bytes(fmt_t f)
{
    switch (f) {
    case kFmt_SInt16: return 2;
    case kFmt_SInt24: return 3;
    default:          return 4;
    }
}

/* DESIGN.md §4.2. Integer input: the physical depth. Float32: SMbd, else 24-bit (P2 adds the
 * grid detection). The original dialect only carries S32_LE. */
static scream_wire_t choose_wire(fmt_t f, scream_dialect_t d)
{
    if (d == SCREAM_DIALECT_ORIGINAL) return SCREAM_WIRE_S32LE;
    unsigned bits = f == kFmt_Float32 ? atomic_load(&gWantBits) : format_bits(f);
    switch (bits) {
    case 16: return SCREAM_WIRE_S16LE;
    case 32: return SCREAM_WIRE_S32LE;
    default: return SCREAM_WIRE_S24_3LE;
    }
}

static UInt32 delay_frames(void)
{
    return (UInt32)((UInt64)atomic_load(&gDelayMs) * (UInt64)gRate / 1000u);
}

static void notify(AudioObjectID obj, AudioObjectPropertySelector sel)
{
    if (!gHost) return;
    AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    gHost->PropertiesChanged(gHost, obj, 1, &a);
}

static pid_t client_pid(UInt32 clientID)
{
    uint64_t v = atomic_load_explicit(&gClientPIDs[clientID % kClientSlots], memory_order_acquire);
    return (v >> 32) == clientID ? (pid_t)(uint32_t)v : 0;
}

static void set_hold(int mode)
{
    atomic_store(&gHoldMode, mode);
    atomic_fetch_add(&gHoldSerial, 1);
}

/* ---- destination (SMds) --------------------------------------------------- */

static CFDictionaryRef copy_destination(void)
{
    pthread_mutex_lock(&gDestMutex);
    CFStringRef ip = CFStringCreateWithCString(NULL, gDestIP, kCFStringEncodingUTF8);
    int port = (int)gDestPort;
    CFNumberRef portNum = CFNumberCreate(NULL, kCFNumberIntType, &port);
    CFStringRef header = gDialect == SCREAM_DIALECT_ORIGINAL ? CFSTR("original") : CFSTR("extended");
    pthread_mutex_unlock(&gDestMutex);

    const void *keys[] = {CFSTR("ip"), CFSTR("port"), CFSTR("header")};
    const void *vals[] = {ip, portNum, header};
    CFDictionaryRef d = CFDictionaryCreate(NULL, keys, vals, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(ip);
    CFRelease(portNum);
    return d;
}

/* Returns false if the dictionary is malformed. Missing keys keep their current value. */
static bool apply_destination(CFDictionaryRef d)
{
    if (!d || CFGetTypeID(d) != CFDictionaryGetTypeID()) return false;
    char ip[64] = {0};
    int port = -1;
    int dialect = -1;

    CFTypeRef v = CFDictionaryGetValue(d, CFSTR("ip"));
    if (v) {
        struct in_addr tmp;
        if (CFGetTypeID(v) != CFStringGetTypeID() || !CFStringGetCString(v, ip, sizeof ip, kCFStringEncodingUTF8) ||
            inet_pton(AF_INET, ip, &tmp) != 1)
            return false;
    }
    v = CFDictionaryGetValue(d, CFSTR("port"));
    if (v) {
        if (CFGetTypeID(v) != CFNumberGetTypeID() || !CFNumberGetValue(v, kCFNumberIntType, &port) || port < 1 || port > 65535)
            return false;
    }
    v = CFDictionaryGetValue(d, CFSTR("header"));
    if (v) {
        if (CFGetTypeID(v) != CFStringGetTypeID()) return false;
        if (CFEqual(v, CFSTR("extended"))) dialect = SCREAM_DIALECT_EXTENDED;
        else if (CFEqual(v, CFSTR("original")) || CFEqual(v, CFSTR("legacy"))) dialect = SCREAM_DIALECT_ORIGINAL;
        else return false;
    }

    pthread_mutex_lock(&gDestMutex);
    if (ip[0]) strlcpy(gDestIP, ip, sizeof gDestIP);
    if (port > 0) gDestPort = (unsigned)port;
    if (dialect >= 0) gDialect = (scream_dialect_t)dialect;
    os_log(gLog, "ScreamMusic: destination %{public}s:%u, %{public}s header", gDestIP, gDestPort,
           gDialect == SCREAM_DIALECT_ORIGINAL ? "original" : "extended");
    pthread_mutex_unlock(&gDestMutex);
    atomic_fetch_add(&gDestGeneration, 1);
    return true;
}

static void save_settings(void)
{
    if (!gHost) return;
    CFDictionaryRef d = copy_destination();
    gHost->WriteToStorage(gHost, CFSTR("destination"), d);
    CFRelease(d);
    int ms = (int)atomic_load(&gDelayMs);
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberIntType, &ms);
    gHost->WriteToStorage(gHost, CFSTR("delay_ms"), n);
    CFRelease(n);
    ms = (int)atomic_load(&gLeadInMs);
    n = CFNumberCreate(NULL, kCFNumberIntType, &ms);
    gHost->WriteToStorage(gHost, CFSTR("lead_in_ms"), n);
    CFRelease(n);
}

static void load_settings(void)
{
    if (!gHost) return;
    CFPropertyListRef p = NULL;
    if (gHost->CopyFromStorage(gHost, CFSTR("destination"), &p) == 0 && p) {
        apply_destination(p);
        CFRelease(p);
    }
    p = NULL;
    if (gHost->CopyFromStorage(gHost, CFSTR("delay_ms"), &p) == 0 && p) {
        int ms;
        if (CFGetTypeID(p) == CFNumberGetTypeID() && CFNumberGetValue(p, kCFNumberIntType, &ms) && ms >= 0 && ms <= (int)kMaxDelayMs)
            atomic_store(&gDelayMs, (unsigned)ms);
        CFRelease(p);
    }
    p = NULL;
    if (gHost->CopyFromStorage(gHost, CFSTR("lead_in_ms"), &p) == 0 && p) {
        int ms;
        if (CFGetTypeID(p) == CFNumberGetTypeID() && CFNumberGetValue(p, kCFNumberIntType, &ms) && ms >= 0 && ms <= (int)kMaxLeadInMs)
            atomic_store(&gLeadInMs, (unsigned)ms);
        CFRelease(p);
    }
}

/* ---- status (SMst) -------------------------------------------------------- */

static void dict_set_u64(CFMutableDictionaryRef d, CFStringRef key, uint64_t v)
{
    long long ll = (long long)v;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongLongType, &ll);
    CFDictionarySetValue(d, key, n);
    CFRelease(n);
}

static CFDictionaryRef copy_status(void)
{
    CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    bool running = atomic_load(&gRunning);
    CFDictionarySetValue(d, CFSTR("running"), running ? kCFBooleanTrue : kCFBooleanFalse);

    pthread_mutex_lock(&gStateMutex);
    dict_set_u64(d, CFSTR("rate"), (uint64_t)gRate);
    static const char *names[] = {"float32", "int16", "int24", "int32"};
    CFStringRef fmt = CFStringCreateWithCString(NULL, names[gFormat], kCFStringEncodingUTF8);
    CFDictionarySetValue(d, CFSTR("physicalFormat"), fmt);
    CFRelease(fmt);
    /* clients that played in the last second (every HAL client is attached to every device) */
    CFMutableArrayRef clients = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    uint64_t now = mach_absolute_time();
    for (int i = 0; i < gClientCount; i++) {
        uint64_t last = atomic_load(&gClientLastOutput[gClients[i].clientID % kClientSlots]);
        if (!last || now - last > (uint64_t)gHostTicksPerSecond) continue;
        CFMutableDictionaryRef c = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        dict_set_u64(c, CFSTR("pid"), (uint64_t)gClients[i].pid);
        CFStringRef b = CFStringCreateWithCString(NULL, gClients[i].bundle, kCFStringEncodingUTF8);
        CFDictionarySetValue(c, CFSTR("bundleID"), b);
        CFRelease(b);
        CFArrayAppendValue(clients, c);
        CFRelease(c);
    }
    dict_set_u64(d, CFSTR("attachedClients"), (uint64_t)gClientCount);
    pthread_mutex_unlock(&gStateMutex);
    CFDictionarySetValue(d, CFSTR("clients"), clients);
    CFRelease(clients);

    if (running) {
        dict_set_u64(d, CFSTR("wireBits"), scream_sample_size_byte(gStream.wire));
        dict_set_u64(d, CFSTR("wireRate"), gStream.rate);
    }
    dict_set_u64(d, CFSTR("packets"), atomic_load(&gStatPackets));
    dict_set_u64(d, CFSTR("bytes"), atomic_load(&gStatBytes));
    dict_set_u64(d, CFSTR("sendErrors"), atomic_load(&gStatSendErrors));
    dict_set_u64(d, CFSTR("lastErrno"), (uint64_t)atomic_load(&gStatLastErrno));
    dict_set_u64(d, CFSTR("underruns"), atomic_load(&gStatUnderruns));
    dict_set_u64(d, CFSTR("overflowBytes"), atomic_load(&gStatOverflowBytes));
    dict_set_u64(d, CFSTR("framesIn"), atomic_load(&gStatFramesIn));
    dict_set_u64(d, CFSTR("offGridSamples"), atomic_load(&gStatOffGrid));
    dict_set_u64(d, CFSTR("runFrames"), atomic_load(&gStatRunFrames));
    dict_set_u64(d, CFSTR("runOffGridSamples"), atomic_load(&gStatRunOffGrid));
    dict_set_u64(d, CFSTR("othersFrames"), atomic_load(&gStatOthersFrames));
    dict_set_u64(d, CFSTR("endOfTrackMarkers"), atomic_load(&gStatEOTs));
    dict_set_u64(d, CFSTR("delayMs"), atomic_load(&gDelayMs));
    dict_set_u64(d, CFSTR("leadInMs"), atomic_load(&gLeadInMs));
    dict_set_u64(d, CFSTR("musicPID"), (uint64_t)atomic_load(&gMusicPID));
    dict_set_u64(d, CFSTR("ownerPID"), (uint64_t)atomic_load(&gOwnerPID));
    dict_set_u64(d, CFSTR("holdMode"), (uint64_t)atomic_load(&gHoldMode));
    dict_set_u64(d, CFSTR("musicSkippedCycles"), atomic_load(&gStatGateSkipped));
    {
        uint64_t gb = atomic_load(&gStatRunGain), db = atomic_load(&gStatRunDev);
        double g, dv;
        memcpy(&g, &gb, 8);
        memcpy(&dv, &db, 8);
        CFNumberRef gn = CFNumberCreate(NULL, kCFNumberDoubleType, &g), dn = CFNumberCreate(NULL, kCFNumberDoubleType, &dv);
        CFDictionarySetValue(d, CFSTR("runGain"), gn);
        CFDictionarySetValue(d, CFSTR("runMeanDeviationLSB"), dn);
        CFRelease(gn);
        CFRelease(dn);
    }
    dict_set_u64(d, CFSTR("diagMusicSample"), (uint64_t)atomic_load(&gDiagMusicSample));
    dict_set_u64(d, CFSTR("diagMusicFrames"), (uint64_t)atomic_load(&gDiagMusicFrames));
    dict_set_u64(d, CFSTR("diagMixSample"), (uint64_t)atomic_load(&gDiagMixSample));
    dict_set_u64(d, CFSTR("diagMixFrames"), (uint64_t)atomic_load(&gDiagMixFrames));
    CFDictionarySetValue(d, CFSTR("holdReached"), atomic_load(&gHoldReached) ? kCFBooleanTrue : kCFBooleanFalse);
    return d;
}

/* ---- sender thread ---------------------------------------------------------- */

static void set_realtime(Float64 periodSeconds)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double ticksPerNs = (double)tb.denom / tb.numer;
    thread_time_constraint_policy_data_t p;
    p.period = (uint32_t)(periodSeconds * 1e9 * ticksPerNs);
    p.computation = (uint32_t)(200000 * ticksPerNs);    /* 0.2 ms */
    p.constraint = (uint32_t)(1000000 * ticksPerNs);    /* 1 ms */
    p.preemptible = 1;
    thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
}

typedef struct {
    int fd;
    struct sockaddr_in addr;
    struct sockaddr_in mirror;
    bool mirrorOn;
    unsigned generation;
} sock_t;

static void refresh_destination(sock_t *s)
{
    unsigned gen = atomic_load(&gDestGeneration);
    if (s->fd >= 0 && gen == s->generation) return;
    if (s->fd < 0) {
        s->fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (s->fd < 0) {
            atomic_store(&gStatLastErrno, errno);
            os_log_error(gLog, "ScreamMusic: socket() failed: %d", errno);
            return;
        }
        int sndbuf = 1 << 20;
        setsockopt(s->fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    }
    pthread_mutex_lock(&gDestMutex);
    memset(&s->addr, 0, sizeof s->addr);
    s->addr.sin_family = AF_INET;
    s->addr.sin_len = sizeof s->addr;
    s->addr.sin_port = htons((uint16_t)gDestPort);
    inet_pton(AF_INET, gDestIP, &s->addr.sin_addr);
    s->mirrorOn = gMirrorIP[0] != 0;
    if (s->mirrorOn) {
        memset(&s->mirror, 0, sizeof s->mirror);
        s->mirror.sin_family = AF_INET;
        s->mirror.sin_len = sizeof s->mirror;
        s->mirror.sin_port = htons((uint16_t)gMirrorPort);
        inet_pton(AF_INET, gMirrorIP, &s->mirror.sin_addr);
    }
    pthread_mutex_unlock(&gDestMutex);
    s->generation = gen;
}

static void send_packet(sock_t *s, const uint8_t *pkt, size_t len)
{
    if (s->fd < 0) return;
    if (s->mirrorOn) sendto(s->fd, pkt, len, 0, (const struct sockaddr *)&s->mirror, sizeof s->mirror);
    ssize_t r = sendto(s->fd, pkt, len, 0, (const struct sockaddr *)&s->addr, sizeof s->addr);
    if (r < 0) {
        atomic_fetch_add(&gStatSendErrors, 1);
        atomic_store(&gStatLastErrno, errno);
    }
}

static uint64_t ring_fill(void)
{
    return atomic_load_explicit(&gRingWrite, memory_order_acquire) - atomic_load_explicit(&gRingRead, memory_order_relaxed);
}

static void ring_peek(uint8_t *dst, size_t n)
{
    uint64_t r = atomic_load_explicit(&gRingRead, memory_order_relaxed);
    size_t off = (size_t)(r % kRingBytes);
    size_t first = n < kRingBytes - off ? n : kRingBytes - off;
    memcpy(dst, gRing + off, first);
    memcpy(dst + first, gRing, n - first);
}

static void ring_take(uint8_t *dst, size_t n)
{
    if (dst) ring_peek(dst, n);
    atomic_store_explicit(&gRingRead, atomic_load_explicit(&gRingRead, memory_order_relaxed) + n, memory_order_release);
}

/* Consumer only: drop everything before ring position `pos`. */
static void ring_skip_to(uint64_t pos)
{
    if (atomic_load_explicit(&gRingRead, memory_order_relaxed) < pos)
        atomic_store_explicit(&gRingRead, pos, memory_order_release);
}

/* Header and payload use the run's snapshot; the dialect is the destination's at the run's start. */
static void send_payload(sock_t *s, const stream_t *st, const uint8_t *payload)
{
    uint8_t pkt[SCREAM_PACKET_SIZE_MAX];
    unsigned h = scream_encode_header(pkt, st->rate, st->wire, st->dialect, false);
    memcpy(pkt + h, payload, SCREAM_PAYLOAD_SIZE);
    send_packet(s, pkt, h + SCREAM_PAYLOAD_SIZE);
    atomic_fetch_add(&gStatPackets, 1);
    atomic_fetch_add(&gStatBytes, SCREAM_PAYLOAD_SIZE);
}

/* UDP end-of-track marker: header only, as snd-screamalsa's scream_send_last_packet(). */
static void send_eot(sock_t *s, const stream_t *st)
{
    uint8_t pkt[SCREAM_HEADER_SIZE_MAX];
    unsigned h = scream_encode_header(pkt, st->rate, st->wire, st->dialect, true);
    send_packet(s, pkt, h);
    atomic_fetch_add(&gStatEOTs, 1);
}

typedef struct {
    uint64_t periodTicks;
    uint64_t bytesPerSec;
} pace_t;

static pace_t pace_for(const stream_t *st)
{
    pace_t p = {0, (uint64_t)st->rate * scream_frame_bytes(st->wire)};
    if (p.bytesPerSec) {
        Float64 period = (Float64)SCREAM_PAYLOAD_SIZE / (Float64)p.bytesPerSec;
        p.periodTicks = (uint64_t)(period * gHostTicksPerSecond);
        set_realtime(period);
    }
    return p;
}

/* Absolute-time pacing (snd-screamalsa's scream_playback_thread): behind = catch up, never burst. */
static void pace_wait(uint64_t *next, uint64_t periodTicks)
{
    *next += periodTicks;
    uint64_t now = mach_absolute_time();
    if (*next < now) *next = now;
    else mach_wait_until(*next);
}

/* Sends the ring up to `limit` (a previous run's tail) with that run's header; a final
 * partial payload is zero-padded. */
static void drain_to(sock_t *s, const stream_t *st, uint64_t limit)
{
    uint8_t payload[SCREAM_PAYLOAD_SIZE];
    pace_t p = pace_for(st);
    uint64_t next = mach_absolute_time();
    for (;;) {
        uint64_t r = atomic_load_explicit(&gRingRead, memory_order_relaxed);
        if (r >= limit) return;
        size_t n = limit - r < SCREAM_PAYLOAD_SIZE ? (size_t)(limit - r) : SCREAM_PAYLOAD_SIZE;
        memset(payload, 0, sizeof payload);
        ring_take(payload, n);
        send_payload(s, st, payload);
        pace_wait(&next, p.periodTicks);
    }
}

static bool same_wire(const stream_t *a, const stream_t *b)
{
    return a->rate == b->rate && a->wire == b->wire && a->dialect == b->dialect;
}

static bool frame_is_zero(const uint8_t *f, unsigned fb)
{
    for (unsigned i = 0; i < fb; i++)
        if (f[i]) return false;
    return true;
}

enum { kScan_Send, kScan_Hold, kScan_Wait };

/*
 * Hold-point search over the next payload (DESIGN.md §6.2.3), on wire bytes. `silent` is the run of
 * silent frames just before the payload (what was last sent; the start of a run counts as silence).
 *   gate:     hold at the first sound that follows >= `thr` silent frames (the first sound after a
 *             pause: what was still playing when the gate was armed goes out, fade and all);
 *   boundary: hold where a run of >= `thr` silent frames begins (Music's inter-track zeros).
 * Returns kScan_Hold with the byte offset in *holdAt, kScan_Send, or kScan_Wait when the run
 * at the end of the payload needs frames not yet written.
 */
static int scan_hold(int mode, uint64_t fill, unsigned fb, uint32_t thr, uint32_t silent, size_t *holdAt)
{
    static uint8_t buf[SCREAM_PAYLOAD_SIZE + 4000 * 8];
    uint32_t payloadFrames = SCREAM_PAYLOAD_SIZE / fb;
    uint64_t availFrames = fill / fb;
    uint32_t want = payloadFrames + (mode == kHold_Boundary ? thr : 0);
    uint32_t n = availFrames < want ? (uint32_t)availFrames : want;
    if (n * fb > sizeof buf) n = (uint32_t)(sizeof buf / fb);
    ring_peek(buf, (size_t)n * fb);

    uint32_t run = silent;
    if (mode == kHold_Gate) {
        for (uint32_t i = 0; i < payloadFrames; i++) {
            if (frame_is_zero(buf + (size_t)i * fb, fb)) {
                run++;
            } else {
                if (run >= thr) {
                    *holdAt = (size_t)i * fb;
                    return kScan_Hold;
                }
                run = 0;
            }
        }
        return kScan_Send;
    }

    int64_t runStart = run ? 0 : -1;
    if (run >= thr) { *holdAt = 0; return kScan_Hold; }
    for (uint32_t i = 0; i < n; i++) {
        if (frame_is_zero(buf + (size_t)i * fb, fb)) {
            if (!run) runStart = i;
            if (++run >= thr) {
                *holdAt = (size_t)runStart * fb;
                return kScan_Hold;
            }
        } else {
            run = 0;
            runStart = -1;
            if (i >= payloadFrames) break;      /* no run begins inside this payload */
        }
        if (i + 1 == payloadFrames && !run) break;
    }
    if (run && runStart >= 0 && runStart < payloadFrames && n < want) return kScan_Wait;
    return kScan_Send;
}

/* The run of silent frames at the end of a payload that went out, added to `silent` when the whole
 * payload was silent. Capped: only "at least thr" matters. */
static uint32_t silent_after(const uint8_t *payload, size_t bytes, unsigned fb, uint32_t silent)
{
    uint32_t frames = (uint32_t)(bytes / fb), tail = 0;
    while (tail < frames && frame_is_zero(payload + (size_t)(frames - 1 - tail) * fb, fb)) tail++;
    uint32_t r = tail == frames ? silent + tail : tail;
    return r > 1u << 20 ? 1u << 20 : r;
}

/* The controlling app died: undo Apple Music mode so audio is not held or filtered forever. */
static void check_owner(uint64_t *lastCheck)
{
    uint64_t now = mach_absolute_time();
    if (now - *lastCheck < (uint64_t)gHostTicksPerSecond) return;
    *lastCheck = now;
    int owner = atomic_load(&gOwnerPID);
    if (owner > 0 && kill(owner, 0) == -1 && errno == ESRCH) {
        os_log(gLog, "ScreamMusic: controlling app (pid %d) is gone; Apple Music mode off", owner);
        atomic_store(&gOwnerPID, 0);
        atomic_store(&gMusicPID, 0);
        atomic_store(&gDelayMs, kDefaultDelayMs);
        set_hold(kHold_Run);
    }
}

static void *sender_main(void *arg)
{
    (void)arg;
    sock_t s = {.fd = -1};
    uint8_t payload[SCREAM_PAYLOAD_SIZE];
    unsigned myGen = 0;
    bool streamed = false;      /* sent audio since the last end-of-track marker */
    stream_t st = gStream;
    stream_t lastSent = {0};    /* the wire format the receiver last got a whole lead-in or audio in */
    uint64_t ownerCheck = 0;

    for (;;) {
        /* idle until a new run starts or an end-of-track marker is asked for */
        pthread_mutex_lock(&gSendMutex);
        while (atomic_load(&gStartGen) == myGen && !atomic_load(&gEOTRequest)) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            pthread_cond_timedwait(&gSendCond, &gSendMutex, &ts);
            check_owner(&ownerCheck);
        }
        pthread_mutex_unlock(&gSendMutex);
        refresh_destination(&s);

        if (atomic_exchange(&gEOTRequest, false) && streamed) {
            send_eot(&s, &st);
            streamed = false;
        }
        unsigned gen = atomic_load(&gStartGen);
        if (gen == myGen) continue;

        /* hand-over to the new run */
        uint64_t startPos = atomic_load(&gStartPos);
        stream_t next = gStream;
        if (streamed && !same_wire(&st, &next)) {
            if (atomic_load(&gHoldMode) == kHold_Drop) ring_skip_to(startPos);
            else drain_to(&s, &st, startPos);    /* the old run's tail, old header */
            send_eot(&s, &st);
            streamed = false;
        } else if (!streamed) {
            ring_skip_to(startPos);         /* nothing of the old run is pending on the wire */
        }
        /* else: same format, still streaming: the old tail flows straight into the new run */
        myGen = gen;
        st = next;

        pace_t p = pace_for(&st);
        if (!p.bytesPerSec) continue;
        unsigned fb = scream_frame_bytes(st.wire);

        /* relock lead-in (SMli): a new wire format starts with digital silence, so the receiver's
         * DAC locks to it before the first sound; then zeros while the run's audio is not ready
         * (a switch's rewind and prefill), at most kLeadFillSec more, so the stream has no gap */
        unsigned leadMs = atomic_load(&gLeadInMs);
        bool changed = !same_wire(&lastSent, &st);
        int once = atomic_load(&gLeadOnceMs);
        if (changed && once >= 0) {      /* SMlo: for this format change only (a stream's live upgrade) */
            if (mach_absolute_time() < atomic_load(&gLeadOnceUntil)) leadMs = (unsigned)once;
            atomic_store(&gLeadOnceMs, -1);
        }
        bool leading = leadMs && changed;
        if (!leading) lastSent = st;     /* no lead-in owed: the receiver counts as synced to this format */
        uint64_t leadLeft = leading ? ((uint64_t)leadMs * p.bytesPerSec / 1000u + SCREAM_PAYLOAD_SIZE - 1)
                                          / SCREAM_PAYLOAD_SIZE * SCREAM_PAYLOAD_SIZE : 0;
        uint64_t leadNext = mach_absolute_time();
        uint64_t leadUntil = leadNext + (uint64_t)(((double)leadMs / 1000.0 + kLeadFillSec) * gHostTicksPerSecond);
        uint32_t thr = st.rate / 100;     /* 10 ms of silence marks a track boundary */

        /* prefill: the send delay (SMdl), at least one payload */
        uint64_t prefill = (uint64_t)atomic_load(&gDelayMs) * p.bytesPerSec / 1000u;
        if (prefill < SCREAM_PAYLOAD_SIZE) prefill = SCREAM_PAYLOAD_SIZE;
        bool needPrefill = !streamed;

        uint64_t idleTicks = (uint64_t)(0.3 * gHostTicksPerSecond);  /* no audio this long: the end of it */
        uint64_t lastAudio = mach_absolute_time();
        uint64_t lastFill = 0;
        unsigned holdSerial = atomic_load(&gHoldSerial) - 1;   /* force a first look */
        bool held = false;
        uint64_t heldSince = 0;
        uint32_t silent = thr;      /* the start of a run counts as silence (gate) */
        bool sentInRun = false;
        uint64_t nextTick = mach_absolute_time();

        for (;;) {
            if (atomic_load(&gStartGen) != myGen) break;     /* restarted: hand over above */
            refresh_destination(&s);
            check_owner(&ownerCheck);
            if (atomic_exchange(&gEOTRequest, false)) {
                if (streamed) send_eot(&s, &st);
                streamed = false;
            }
            bool running = atomic_load(&gRunning);
            uint64_t now = mach_absolute_time();

            if (leading) {
                if (sentInRun || !running || (!leadLeft && now > leadUntil)) {
                    leading = false;
                    leadLeft = 0;
                } else {
                    while (now >= leadNext &&
                           (leadLeft || (needPrefill && ring_fill() < prefill && now <= leadUntil))) {
                        memset(payload, 0, sizeof payload);
                        send_payload(&s, &st, payload);
                        /* not `streamed` and not lastSent: a lead-in cut short (the run restarts while
                         * the device switches) must neither end with a marker nor count as done */
                        leadLeft = leadLeft > SCREAM_PAYLOAD_SIZE ? leadLeft - SCREAM_PAYLOAD_SIZE : 0;
                        if (!leadLeft) lastSent = st;
                        leadNext += p.periodTicks;
                    }
                }
            }

            /* hold point requests */
            int mode = atomic_load(&gHoldMode);
            unsigned serial = atomic_load(&gHoldSerial);
            if (serial != holdSerial) {
                holdSerial = serial;
                if (mode == kHold_Drop && streamed) {
                    send_eot(&s, &st);
                    streamed = false;
                }
                if (held && mode == kHold_Run) nextTick = now;     /* released */
                held = false;
                heldSince = now;
                atomic_store(&gHoldReached, false);
            }
            if (mode == kHold_Drop) {
                /* discard everything (the wrong-rate start, the pause fade) until released */
                ring_skip_to(atomic_load_explicit(&gRingWrite, memory_order_acquire));
                atomic_store(&gHoldReached, true);
                needPrefill = true;
                if (now - heldSince > (uint64_t)(kDropTimeoutSec * gHostTicksPerSecond)) {
                    os_log(gLog, "ScreamMusic: drop not released after %d s; releasing", kDropTimeoutSec);
                    set_hold(kHold_Run);
                }
                usleep(2000);
                continue;
            }
            if (held && !running) {
                /* the device stopped while we held: what we hold is stale (a new track's start, or
                 * nothing of use); drop it, mark the end, and end the run */
                ring_skip_to(atomic_load_explicit(&gRingWrite, memory_order_acquire));
                if (streamed) send_eot(&s, &st);
                streamed = false;
                break;
            }
            if (held) {
                if (now - heldSince > (uint64_t)(kHoldTimeoutSec * gHostTicksPerSecond)) {
                    os_log(gLog, "ScreamMusic: hold not released after %d s; releasing", kHoldTimeoutSec);
                    set_hold(kHold_Run);
                }
                usleep(2000);
                continue;
            }

            if (leadLeft) {          /* the lead-in first: the audio waits in the ring */
                usleep(500);
                continue;
            }
            uint64_t fill = ring_fill();
            if (needPrefill) {
                if (running && fill < prefill) {
                    if (fill != lastFill) {
                        lastFill = fill;
                        lastAudio = now;
                    } else if (fill && now - lastAudio > idleTicks) {
                        ring_take(NULL, (size_t)fill);   /* a sound shorter than the delay that ended */
                        lastFill = 0;
                    }
                    usleep(1000);
                    continue;
                }
                lastAudio = now;
                needPrefill = false;
                nextTick = now;
            }
            if (fill >= SCREAM_PAYLOAD_SIZE) {
                if (mode == kHold_Gate || mode == kHold_Boundary) {
                    size_t holdAt = 0;
                    /* the start of a run counts as silence for the gate only: a boundary is the end of
                     * something that played */
                    int r = scan_hold(mode, fill, fb, thr, mode == kHold_Gate || sentInRun ? silent : 0, &holdAt);
                    if (r == kScan_Wait && running) { usleep(1000); continue; }
                    if (r == kScan_Hold) {
                        if (holdAt) {       /* what comes before the hold point, zero-padded */
                            memset(payload, 0, sizeof payload);
                            ring_take(payload, holdAt);
                            send_payload(&s, &st, payload);
                            streamed = true;
                            lastSent = st;
                        }
                        held = true;
                        heldSince = now;
                        silent = thr;       /* the padding: whatever follows the hold starts after silence */
                        atomic_store(&gHoldReached, true);
                        os_log(gLog, "ScreamMusic: holding (%{public}s)", mode == kHold_Gate ? "gate" : "boundary");
                        continue;
                    }
                }
                ring_take(payload, SCREAM_PAYLOAD_SIZE);
                send_payload(&s, &st, payload);
                streamed = true;
                lastSent = st;
                lastAudio = now;
                silent = silent_after(payload, SCREAM_PAYLOAD_SIZE, fb, silent);
                sentInRun = true;
            } else if (running && now - lastAudio < idleTicks) {
                /* producer behind: wait for it without advancing the schedule */
                if (streamed) atomic_fetch_add(&gStatUnderruns, 1);
                usleep(1000);
                nextTick = mach_absolute_time();
                continue;
            } else if (running) {
                /* the device runs, but nothing has played for a while (Music stopped, only the
                 * app's loopback reader is left): end the stream; prefill again when audio returns */
                if (fill) {
                    memset(payload, 0, sizeof payload);
                    ring_take(payload, (size_t)fill);
                    if (streamed) send_payload(&s, &st, payload);
                }
                if (streamed) send_eot(&s, &st);
                streamed = false;
                needPrefill = true;
                usleep(1000);
                continue;
            } else {
                /* stopped and drained: pad the tail to a full payload, then mark the end */
                if (fill) {
                    memset(payload, 0, sizeof payload);
                    ring_take(payload, (size_t)fill);
                    send_payload(&s, &st, payload);
                }
                if (streamed) send_eot(&s, &st);
                streamed = false;
                break;
            }
            /* after a hold the ring holds more than the send delay: catch up at double speed */
            pace_wait(&nextTick, fill > 2 * prefill + SCREAM_PAYLOAD_SIZE ? p.periodTicks / 2 : p.periodTicks);
        }
    }
    return NULL;
}

static void start_sender(void)
{
    if (atomic_exchange(&gSenderStarted, true)) return;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, sender_main, NULL);
    pthread_attr_destroy(&attr);
}

static void wake_sender(void)
{
    pthread_mutex_lock(&gSendMutex);
    pthread_cond_broadcast(&gSendCond);
    pthread_mutex_unlock(&gSendMutex);
}

/* Publishes a new run (caller holds gIOMutex; IO is not running). */
static void begin_run_locked(void)
{
    pthread_mutex_lock(&gStateMutex);
    pthread_mutex_lock(&gDestMutex);
    scream_dialect_t dialect = gDialect;
    pthread_mutex_unlock(&gDestMutex);
    gStream.rate = (uint32_t)gRate;
    gStream.format = gFormat;
    gStream.dialect = dialect;
    gStream.wire = choose_wire(gFormat, dialect);
    pthread_mutex_unlock(&gStateMutex);

    atomic_store(&gStatRunFrames, 0);
    atomic_store(&gStatRunOffGrid, 0);
    gFitYR = gFitRR = gFitDev = gFitN = 0;      /* IO is not running */
    atomic_store(&gStatRunGain, 0);
    atomic_store(&gStatRunDev, 0);
    atomic_store(&gStartPos, atomic_load(&gRingWrite));
    atomic_store(&gRunning, true);
    atomic_fetch_add(&gStartGen, 1);
    os_log(gLog, "ScreamMusic: start %u Hz, physical %d, wire %u-bit", gStream.rate, (int)gStream.format,
           scream_sample_size_byte(gStream.wire));
    wake_sender();
}

/* ---- IUnknown / factory ------------------------------------------------------ */

void *ScreamMusic_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);

void *ScreamMusic_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID)
{
    (void)inAllocator;
    if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) return gDriverRef;
    return NULL;
}

static HRESULT SM_QueryInterface(void *inDriver, REFIID inUUID, LPVOID *outInterface)
{
    if (inDriver != gDriverRef || !outInterface) return kAudioHardwareBadObjectError;
    CFUUIDRef req = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(req, IUnknownUUID) || CFEqual(req, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&gStateMutex);
        gRefCount++;
        pthread_mutex_unlock(&gStateMutex);
        *outInterface = gDriverRef;
        r = S_OK;
    }
    CFRelease(req);
    return r;
}

static ULONG SM_AddRef(void *inDriver)
{
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    ULONG r = ++gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return r;
}

static ULONG SM_Release(void *inDriver)
{
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    if (gRefCount) gRefCount--;
    ULONG r = gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return r;
}

/* ---- basic operations ------------------------------------------------------- */

static void update_clock_rate(void)
{
    gHostTicksPerFrame = gHostTicksPerSecond / gRate;
}

static OSStatus SM_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    gHost = inHost;
    gLog = os_log_create("com.ayanamilee.ScreamMusic", "driver");
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    gHostTicksPerSecond = 1e9 * (Float64)tb.denom / (Float64)tb.numer;
    update_clock_rate();
    load_settings();
    start_sender();
    os_log(gLog, "ScreamMusic: driver initialized");
    return 0;
}

static OSStatus SM_CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *c, AudioObjectID *out)
{
    (void)d; (void)desc; (void)c; (void)out;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus SM_DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID id)
{
    (void)d; (void)id;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus SM_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, const AudioServerPlugInClientInfo *info)
{
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    if (gClientCount < kMaxClients) {
        memset(&gClients[gClientCount], 0, sizeof gClients[0]);
        gClients[gClientCount].pid = info->mProcessID;
        gClients[gClientCount].clientID = info->mClientID;
        if (info->mBundleID)
            CFStringGetCString(info->mBundleID, gClients[gClientCount].bundle, sizeof gClients[0].bundle, kCFStringEncodingUTF8);
        gClientCount++;
    }
    atomic_store_explicit(&gClientPIDs[info->mClientID % kClientSlots],
                          ((uint64_t)info->mClientID << 32) | (uint32_t)info->mProcessID, memory_order_release);
    pthread_mutex_unlock(&gStateMutex);
    return 0;
}

static OSStatus SM_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, const AudioServerPlugInClientInfo *info)
{
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    for (int i = 0; i < gClientCount; i++) {
        if (gClients[i].clientID == info->mClientID) {
            gClients[i] = gClients[--gClientCount];
            break;
        }
    }
    uint64_t want = ((uint64_t)info->mClientID << 32) | (uint32_t)info->mProcessID;
    atomic_compare_exchange_strong(&gClientPIDs[info->mClientID % kClientSlots], &want, 0);
    pthread_mutex_unlock(&gStateMutex);
    return 0;
}

static OSStatus SM_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt64 action, void *info)
{
    (void)action; (void)info;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gIOMutex);
    pthread_mutex_lock(&gStateMutex);
    if (gPendingRate > 0) {
        gRate = gPendingRate;
        gPendingRate = 0;
    }
    if (gPendingFormat >= 0) {
        gFormat = (fmt_t)gPendingFormat;
        gPendingFormat = -1;
    }
    update_clock_rate();
    os_log(gLog, "ScreamMusic: now %.0f Hz, format %d", gRate, (int)gFormat);
    pthread_mutex_unlock(&gStateMutex);
    /* IO may resume without a new StartIO: restart the clock and the run with the new format */
    if (gIOCount) {
        gAnchorHost = mach_absolute_time();
        gNumTimeStamps = 0;
        gClockSeed++;
        begin_run_locked();
    }
    pthread_mutex_unlock(&gIOMutex);
    return 0;
}

static OSStatus SM_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt64 action, void *info)
{
    (void)action; (void)info;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gStateMutex);
    gPendingRate = 0;
    gPendingFormat = -1;
    pthread_mutex_unlock(&gStateMutex);
    return 0;
}

/* ---- properties: plug-in ---------------------------------------------------- */

#define WRITE_VALUE(type, value) do { \
        if (inDataSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; \
        *(type *)outData = (value); *outDataSize = sizeof(type); \
    } while (0)

static Boolean plugin_has(const AudioObjectPropertyAddress *a)
{
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyManufacturer:
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyBoxList:
    case kAudioPlugInPropertyTranslateUIDToBox:
    case kAudioPlugInPropertyDeviceList:
    case kAudioPlugInPropertyTranslateUIDToDevice:
    case kAudioPlugInPropertyResourceBundle:
        return true;
    }
    return false;
}

static OSStatus plugin_size(const AudioObjectPropertyAddress *a, UInt32 *out)
{
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:          *out = sizeof(AudioClassID); return 0;
    case kAudioObjectPropertyOwner:          *out = sizeof(AudioObjectID); return 0;
    case kAudioObjectPropertyManufacturer:
    case kAudioPlugInPropertyResourceBundle: *out = sizeof(CFStringRef); return 0;
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyDeviceList:     *out = sizeof(AudioObjectID); return 0;
    case kAudioPlugInPropertyBoxList:        *out = 0; return 0;
    case kAudioPlugInPropertyTranslateUIDToBox:
    case kAudioPlugInPropertyTranslateUIDToDevice: *out = sizeof(AudioObjectID); return 0;
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus plugin_get(const AudioObjectPropertyAddress *a, UInt32 qSize, const void *qData,
                           UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:   WRITE_VALUE(AudioClassID, kAudioObjectClassID); return 0;
    case kAudioObjectPropertyClass:       WRITE_VALUE(AudioClassID, kAudioPlugInClassID); return 0;
    case kAudioObjectPropertyOwner:       WRITE_VALUE(AudioObjectID, kAudioObjectUnknown); return 0;
    case kAudioObjectPropertyManufacturer: WRITE_VALUE(CFStringRef, CFSTR(kDriver_Manufacturer)); return 0;
    case kAudioPlugInPropertyResourceBundle: WRITE_VALUE(CFStringRef, CFSTR("")); return 0;
    case kAudioObjectPropertyOwnedObjects:
    case kAudioPlugInPropertyDeviceList:
        if (inDataSize < sizeof(AudioObjectID)) { *outDataSize = 0; return 0; }
        WRITE_VALUE(AudioObjectID, kObjectID_Device);
        return 0;
    case kAudioPlugInPropertyBoxList:
        *outDataSize = 0;
        return 0;
    case kAudioPlugInPropertyTranslateUIDToBox:
        WRITE_VALUE(AudioObjectID, kAudioObjectUnknown);
        return 0;
    case kAudioPlugInPropertyTranslateUIDToDevice: {
        if (qSize != sizeof(CFStringRef) || !qData) return kAudioHardwareBadPropertySizeError;
        CFStringRef uid = *(const CFStringRef *)qData;
        WRITE_VALUE(AudioObjectID, CFStringCompare(uid, CFSTR(kDevice_UID), 0) == kCFCompareEqualTo ? kObjectID_Device : kAudioObjectUnknown);
        return 0;
    }
    }
    return kAudioHardwareUnknownPropertyError;
}

/* ---- properties: device ------------------------------------------------------ */

#define CUSTOM(sel) {sel, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone}
static const AudioServerPlugInCustomPropertyInfo kCustomProps[] = {
    CUSTOM(kSM_Destination), CUSTOM(kSM_Status), CUSTOM(kSM_EndOfTrack), CUSTOM(kSM_SendDelay),
    CUSTOM(kSM_WireBits), CUSTOM(kSM_MusicPID), CUSTOM(kSM_Hold), CUSTOM(kSM_HoldReached), CUSTOM(kSM_Owner),
    CUSTOM(kSM_Mirror), CUSTOM(kSM_LeadIn), CUSTOM(kSM_LeadOnce),
};
#define kNumCustomProps (sizeof kCustomProps / sizeof kCustomProps[0])

static bool is_custom(AudioObjectPropertySelector s)
{
    for (size_t i = 0; i < kNumCustomProps; i++)
        if (kCustomProps[i].mSelector == s) return true;
    return false;
}

/* Streams of a scope: output, input, or both (global). */
static UInt32 device_streams(AudioObjectPropertyScope s, AudioObjectID out[2])
{
    UInt32 n = 0;
    if (s == kAudioObjectPropertyScopeGlobal || s == kAudioObjectPropertyScopeOutput) out[n++] = kObjectID_Stream_Output;
    if (s == kAudioObjectPropertyScopeGlobal || s == kAudioObjectPropertyScopeInput) out[n++] = kObjectID_Stream_Input;
    return n;
}

static Boolean device_has(const AudioObjectPropertyAddress *a)
{
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioObjectPropertyOwnedObjects:
    case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID:
    case kAudioDevicePropertyTransportType:
    case kAudioDevicePropertyRelatedDevices:
    case kAudioDevicePropertyClockDomain:
    case kAudioDevicePropertyDeviceIsAlive:
    case kAudioDevicePropertyDeviceIsRunning:
    case kAudioObjectPropertyControlList:
    case kAudioDevicePropertyNominalSampleRate:
    case kAudioDevicePropertyAvailableNominalSampleRates:
    case kAudioDevicePropertyIsHidden:
    case kAudioDevicePropertyZeroTimeStampPeriod:
    case kAudioDevicePropertyStreams:
    case kAudioObjectPropertyCustomPropertyInfoList:
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
    case kAudioDevicePropertyLatency:
    case kAudioDevicePropertySafetyOffset:
    case kAudioDevicePropertyPreferredChannelsForStereo:
        return true;
    }
    return is_custom(a->mSelector);
}

static OSStatus device_settable(const AudioObjectPropertyAddress *a, Boolean *out)
{
    switch (a->mSelector) {
    case kAudioDevicePropertyNominalSampleRate:
    case kSM_Destination:
    case kSM_EndOfTrack:
    case kSM_SendDelay:
    case kSM_LeadIn:
    case kSM_LeadOnce:
    case kSM_WireBits:
    case kSM_MusicPID:
    case kSM_Hold:
    case kSM_Owner:
    case kSM_Mirror:
        *out = true;
        return 0;
    }
    *out = false;
    return device_has(a) ? 0 : kAudioHardwareUnknownPropertyError;
}

static OSStatus device_size(const AudioObjectPropertyAddress *a, UInt32 *out)
{
    AudioObjectID streams[2];
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:              *out = sizeof(AudioClassID); return 0;
    case kAudioObjectPropertyOwner:              *out = sizeof(AudioObjectID); return 0;
    case kAudioObjectPropertyName:
    case kAudioObjectPropertyManufacturer:
    case kAudioDevicePropertyDeviceUID:
    case kAudioDevicePropertyModelUID:           *out = sizeof(CFStringRef); return 0;
    case kAudioObjectPropertyOwnedObjects:
    case kAudioDevicePropertyStreams:            *out = device_streams(a->mScope, streams) * sizeof(AudioObjectID); return 0;
    case kAudioDevicePropertyRelatedDevices:     *out = sizeof(AudioObjectID); return 0;
    case kAudioObjectPropertyControlList:        *out = 0; return 0;
    case kAudioDevicePropertyTransportType:
    case kAudioDevicePropertyClockDomain:
    case kAudioDevicePropertyDeviceIsAlive:
    case kAudioDevicePropertyDeviceIsRunning:
    case kAudioDevicePropertyDeviceCanBeDefaultDevice:
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
    case kAudioDevicePropertyLatency:
    case kAudioDevicePropertySafetyOffset:
    case kAudioDevicePropertyIsHidden:
    case kAudioDevicePropertyZeroTimeStampPeriod: *out = sizeof(UInt32); return 0;
    case kAudioDevicePropertyNominalSampleRate:  *out = sizeof(Float64); return 0;
    case kAudioDevicePropertyAvailableNominalSampleRates: *out = kNumRates * sizeof(AudioValueRange); return 0;
    case kAudioDevicePropertyPreferredChannelsForStereo: *out = 2 * sizeof(UInt32); return 0;
    case kAudioObjectPropertyCustomPropertyInfoList: *out = sizeof kCustomProps; return 0;
    }
    if (is_custom(a->mSelector)) { *out = sizeof(CFPropertyListRef); return 0; }
    return kAudioHardwareUnknownPropertyError;
}

static CFNumberRef number_i(int v)
{
    return CFNumberCreate(NULL, kCFNumberIntType, &v);
}

static OSStatus device_get(const AudioObjectPropertyAddress *a, UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
    bool input = a->mScope == kAudioObjectPropertyScopeInput;
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:   WRITE_VALUE(AudioClassID, kAudioObjectClassID); return 0;
    case kAudioObjectPropertyClass:       WRITE_VALUE(AudioClassID, kAudioDeviceClassID); return 0;
    case kAudioObjectPropertyOwner:       WRITE_VALUE(AudioObjectID, kObjectID_PlugIn); return 0;
    case kAudioObjectPropertyName:        WRITE_VALUE(CFStringRef, CFSTR(kDriver_Name)); return 0;
    case kAudioObjectPropertyManufacturer: WRITE_VALUE(CFStringRef, CFSTR(kDriver_Manufacturer)); return 0;
    case kAudioDevicePropertyDeviceUID:   WRITE_VALUE(CFStringRef, CFSTR(kDevice_UID)); return 0;
    case kAudioDevicePropertyModelUID:    WRITE_VALUE(CFStringRef, CFSTR(kDevice_ModelUID)); return 0;
    case kAudioObjectPropertyOwnedObjects:
    case kAudioDevicePropertyStreams: {
        AudioObjectID streams[2];
        UInt32 n = device_streams(a->mScope, streams);
        if (n > inDataSize / sizeof(AudioObjectID)) n = inDataSize / sizeof(AudioObjectID);
        memcpy(outData, streams, n * sizeof(AudioObjectID));
        *outDataSize = n * sizeof(AudioObjectID);
        return 0;
    }
    case kAudioDevicePropertyRelatedDevices: WRITE_VALUE(AudioObjectID, kObjectID_Device); return 0;
    case kAudioObjectPropertyControlList: *outDataSize = 0; return 0;
    case kAudioDevicePropertyTransportType: WRITE_VALUE(UInt32, kAudioDeviceTransportTypeVirtual); return 0;
    case kAudioDevicePropertyClockDomain: WRITE_VALUE(UInt32, 0); return 0;
    case kAudioDevicePropertyDeviceIsAlive: WRITE_VALUE(UInt32, 1); return 0;
    case kAudioDevicePropertyDeviceIsRunning: {
        pthread_mutex_lock(&gIOMutex);
        UInt32 running = gIOCount > 0;
        pthread_mutex_unlock(&gIOMutex);
        WRITE_VALUE(UInt32, running);
        return 0;
    }
    /* never the default input (that is the others loopback) nor the alert device */
    case kAudioDevicePropertyDeviceCanBeDefaultDevice: WRITE_VALUE(UInt32, input ? 0 : 1); return 0;
    case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: WRITE_VALUE(UInt32, 0); return 0;
    case kAudioDevicePropertyLatency: {
        pthread_mutex_lock(&gStateMutex);
        UInt32 frames = input ? 0 : delay_frames();
        pthread_mutex_unlock(&gStateMutex);
        WRITE_VALUE(UInt32, frames);
        return 0;
    }
    case kAudioDevicePropertySafetyOffset: WRITE_VALUE(UInt32, 0); return 0;
    case kAudioDevicePropertyIsHidden: WRITE_VALUE(UInt32, 0); return 0;
    case kAudioDevicePropertyZeroTimeStampPeriod: WRITE_VALUE(UInt32, kZeroTimeStampPeriod); return 0;
    case kAudioDevicePropertyNominalSampleRate: {
        pthread_mutex_lock(&gStateMutex);
        Float64 r = gRate;
        pthread_mutex_unlock(&gStateMutex);
        WRITE_VALUE(Float64, r);
        return 0;
    }
    case kAudioDevicePropertyAvailableNominalSampleRates: {
        UInt32 n = inDataSize / sizeof(AudioValueRange);
        if (n > kNumRates) n = kNumRates;
        AudioValueRange *r = outData;
        for (UInt32 i = 0; i < n; i++) r[i].mMinimum = r[i].mMaximum = kRates[i];
        *outDataSize = n * sizeof(AudioValueRange);
        return 0;
    }
    case kAudioDevicePropertyPreferredChannelsForStereo: {
        if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        ((UInt32 *)outData)[0] = 1;
        ((UInt32 *)outData)[1] = 2;
        *outDataSize = 2 * sizeof(UInt32);
        return 0;
    }
    case kAudioObjectPropertyCustomPropertyInfoList: {
        UInt32 n = inDataSize / sizeof(AudioServerPlugInCustomPropertyInfo);
        if (n > kNumCustomProps) n = kNumCustomProps;
        memcpy(outData, kCustomProps, n * sizeof(AudioServerPlugInCustomPropertyInfo));
        *outDataSize = n * sizeof(AudioServerPlugInCustomPropertyInfo);
        return 0;
    }
    case kSM_Destination: WRITE_VALUE(CFPropertyListRef, copy_destination()); return 0;
    case kSM_Status:      WRITE_VALUE(CFPropertyListRef, copy_status()); return 0;
    case kSM_EndOfTrack:  WRITE_VALUE(CFPropertyListRef, number_i(0)); return 0;
    case kSM_SendDelay:   WRITE_VALUE(CFPropertyListRef, number_i((int)atomic_load(&gDelayMs))); return 0;
    case kSM_LeadIn:      WRITE_VALUE(CFPropertyListRef, number_i((int)atomic_load(&gLeadInMs))); return 0;
    case kSM_LeadOnce:    WRITE_VALUE(CFPropertyListRef, number_i(atomic_load(&gLeadOnceMs))); return 0;
    case kSM_WireBits:    WRITE_VALUE(CFPropertyListRef, number_i((int)atomic_load(&gWantBits))); return 0;
    case kSM_MusicPID:    WRITE_VALUE(CFPropertyListRef, number_i(atomic_load(&gMusicPID))); return 0;
    case kSM_Hold:        WRITE_VALUE(CFPropertyListRef, number_i(atomic_load(&gHoldMode))); return 0;
    case kSM_HoldReached: WRITE_VALUE(CFPropertyListRef, number_i(atomic_load(&gHoldReached) ? 1 : 0)); return 0;
    case kSM_Owner:       WRITE_VALUE(CFPropertyListRef, number_i(atomic_load(&gOwnerPID))); return 0;
    case kSM_Mirror: {
        pthread_mutex_lock(&gDestMutex);
        CFStringRef ip = CFStringCreateWithCString(NULL, gMirrorIP, kCFStringEncodingUTF8);
        CFNumberRef port = number_i((int)gMirrorPort);
        pthread_mutex_unlock(&gDestMutex);
        const void *k[] = {CFSTR("ip"), CFSTR("port")};
        const void *v[] = {ip, port};
        CFDictionaryRef d = CFDictionaryCreate(NULL, k, v, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFRelease(ip);
        CFRelease(port);
        WRITE_VALUE(CFPropertyListRef, d);
        return 0;
    }
    }
    return kAudioHardwareUnknownPropertyError;
}

static bool cf_int(CFPropertyListRef p, int *out)
{
    return p && CFGetTypeID(p) == CFNumberGetTypeID() && CFNumberGetValue(p, kCFNumberIntType, out);
}

static OSStatus request_change(void)
{
    if (!gHost) return 0;
    return gHost->RequestDeviceConfigurationChange(gHost, kObjectID_Device, 0, NULL);
}

static OSStatus device_set(const AudioObjectPropertyAddress *a, UInt32 inDataSize, const void *inData)
{
    if (a->mSelector == kAudioDevicePropertyNominalSampleRate) {
        if (inDataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        Float64 r = *(const Float64 *)inData;
        if (!rate_supported(r)) return kAudioHardwareIllegalOperationError;
        pthread_mutex_lock(&gStateMutex);
        bool change = r != gRate;
        if (change) gPendingRate = r;
        pthread_mutex_unlock(&gStateMutex);
        return change ? request_change() : 0;
    }
    if (inDataSize != sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
    CFPropertyListRef p = *(const CFPropertyListRef *)inData;
    int v = 0;
    if (a->mSelector == kSM_Mirror) {
        if (!p || CFGetTypeID(p) != CFDictionaryGetTypeID()) return kAudioHardwareIllegalOperationError;
        char ip[64] = {0};
        int port = 0;
        CFTypeRef ipv = CFDictionaryGetValue(p, CFSTR("ip")), pv = CFDictionaryGetValue(p, CFSTR("port"));
        struct in_addr tmp;
        if (ipv && (CFGetTypeID(ipv) != CFStringGetTypeID() || !CFStringGetCString(ipv, ip, sizeof ip, kCFStringEncodingUTF8) ||
                    (ip[0] && inet_pton(AF_INET, ip, &tmp) != 1)))
            return kAudioHardwareIllegalOperationError;
        if (pv && !cf_int(pv, &port)) return kAudioHardwareIllegalOperationError;
        if (ip[0] && (port < 1 || port > 65535)) return kAudioHardwareIllegalOperationError;
        pthread_mutex_lock(&gDestMutex);
        strlcpy(gMirrorIP, ip, sizeof gMirrorIP);
        gMirrorPort = (unsigned)port;
        pthread_mutex_unlock(&gDestMutex);
        atomic_fetch_add(&gDestGeneration, 1);
        os_log(gLog, "ScreamMusic: mirror %{public}s:%d", ip[0] ? ip : "off", port);
        return 0;
    }
    if (a->mSelector != kSM_Destination && !cf_int(p, &v)) return kAudioHardwareIllegalOperationError;

    switch (a->mSelector) {
    case kSM_Destination:
        if (!apply_destination(p)) return kAudioHardwareIllegalOperationError;
        save_settings();
        notify(kObjectID_Device, kSM_Destination);
        return 0;
    case kSM_EndOfTrack:
        if (v == 1) {
            atomic_store(&gEOTRequest, true);
            wake_sender();
        }
        return 0;
    case kSM_SendDelay: {
        if (v < 0 || v > (int)kMaxDelayMs) return kAudioHardwareIllegalOperationError;
        atomic_store(&gDelayMs, (unsigned)v);
        save_settings();
        AudioObjectPropertyAddress lat = {kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
        if (gHost) gHost->PropertiesChanged(gHost, kObjectID_Device, 1, &lat);
        return 0;
    }
    case kSM_LeadIn:
        if (v < 0 || v > (int)kMaxLeadInMs) return kAudioHardwareIllegalOperationError;
        atomic_store(&gLeadInMs, (unsigned)v);
        save_settings();
        return 0;
    case kSM_LeadOnce:      /* not persisted: it belongs to the format change that follows */
        if (v < -1 || v > (int)kMaxLeadInMs) return kAudioHardwareIllegalOperationError;
        atomic_store(&gLeadOnceUntil, mach_absolute_time() + (uint64_t)(kLeadOnceSec * gHostTicksPerSecond));
        atomic_store(&gLeadOnceMs, v);
        return 0;
    case kSM_WireBits: {
        if (v != 0 && v != 16 && v != 24 && v != 32) return kAudioHardwareIllegalOperationError;
        bool changed = atomic_exchange(&gWantBits, (unsigned)v) != (unsigned)v;
        /* while running, a configuration change restarts the run with the new wire depth */
        pthread_mutex_lock(&gIOMutex);
        bool running = gIOCount > 0;
        pthread_mutex_unlock(&gIOMutex);
        return changed && running && gStream.format == kFmt_Float32 ? request_change() : 0;
    }
    case kSM_MusicPID:
        atomic_store(&gMusicPID, v);
        os_log(gLog, "ScreamMusic: Music pid %d", v);
        return 0;
    case kSM_Hold:
        if (v < kHold_Run || v > kHold_Drop) return kAudioHardwareIllegalOperationError;
        set_hold(v);
        wake_sender();
        return 0;
    case kSM_Owner:
        atomic_store(&gOwnerPID, v);
        return 0;
    }
    return kAudioHardwareUnknownPropertyError;
}

/* ---- properties: streams ------------------------------------------------------ */

static Boolean stream_has(const AudioObjectPropertyAddress *a)
{
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:
    case kAudioObjectPropertyOwner:
    case kAudioObjectPropertyOwnedObjects:
    case kAudioStreamPropertyIsActive:
    case kAudioStreamPropertyDirection:
    case kAudioStreamPropertyTerminalType:
    case kAudioStreamPropertyStartingChannel:
    case kAudioStreamPropertyLatency:
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats:
        return true;
    }
    return false;
}

static OSStatus stream_settable(const AudioObjectPropertyAddress *a, Boolean *out)
{
    switch (a->mSelector) {
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:
        *out = true;
        return 0;
    }
    *out = false;
    return stream_has(a) ? 0 : kAudioHardwareUnknownPropertyError;
}

/* Output formats: Float32 (mixable) plus the non-mixable integers, which a client in integer
 * mode uses as-is (virtual = physical). The input (others loopback) is Float32 only. */
static UInt32 stream_format_count(bool input)
{
    return (input ? 1 : kNumFormats) * kNumRates;
}

static OSStatus stream_size(bool input, const AudioObjectPropertyAddress *a, UInt32 *out)
{
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:
    case kAudioObjectPropertyClass:            *out = sizeof(AudioClassID); return 0;
    case kAudioObjectPropertyOwner:            *out = sizeof(AudioObjectID); return 0;
    case kAudioObjectPropertyOwnedObjects:     *out = 0; return 0;
    case kAudioStreamPropertyIsActive:
    case kAudioStreamPropertyDirection:
    case kAudioStreamPropertyTerminalType:
    case kAudioStreamPropertyStartingChannel:
    case kAudioStreamPropertyLatency:          *out = sizeof(UInt32); return 0;
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat:   *out = sizeof(AudioStreamBasicDescription); return 0;
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats:
        *out = stream_format_count(input) * sizeof(AudioStreamRangedDescription);
        return 0;
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus stream_get(bool input, const AudioObjectPropertyAddress *a, UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
    switch (a->mSelector) {
    case kAudioObjectPropertyBaseClass:   WRITE_VALUE(AudioClassID, kAudioObjectClassID); return 0;
    case kAudioObjectPropertyClass:       WRITE_VALUE(AudioClassID, kAudioStreamClassID); return 0;
    case kAudioObjectPropertyOwner:       WRITE_VALUE(AudioObjectID, kObjectID_Device); return 0;
    case kAudioObjectPropertyOwnedObjects: *outDataSize = 0; return 0;
    case kAudioStreamPropertyIsActive:    WRITE_VALUE(UInt32, 1); return 0;
    case kAudioStreamPropertyDirection:   WRITE_VALUE(UInt32, input ? 1 : 0); return 0;
    case kAudioStreamPropertyTerminalType: WRITE_VALUE(UInt32, kAudioStreamTerminalTypeLine); return 0;
    case kAudioStreamPropertyStartingChannel: WRITE_VALUE(UInt32, 1); return 0;
    case kAudioStreamPropertyLatency:     WRITE_VALUE(UInt32, 0); return 0;
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: {
        if (inDataSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        pthread_mutex_lock(&gStateMutex);
        fill_asbd(outData, gRate, input ? kFmt_Float32 : gFormat);
        pthread_mutex_unlock(&gStateMutex);
        *outDataSize = sizeof(AudioStreamBasicDescription);
        return 0;
    }
    case kAudioStreamPropertyAvailableVirtualFormats:
    case kAudioStreamPropertyAvailablePhysicalFormats: {
        UInt32 max = inDataSize / sizeof(AudioStreamRangedDescription), n = 0;
        AudioStreamRangedDescription *d = outData;
        for (int f = 0; f < (input ? 1 : kNumFormats); f++) {
            for (size_t r = 0; r < kNumRates && n < max; r++, n++) {
                fill_asbd(&d[n].mFormat, kRates[r], (fmt_t)f);
                d[n].mSampleRateRange.mMinimum = d[n].mSampleRateRange.mMaximum = kRates[r];
            }
        }
        *outDataSize = n * sizeof(AudioStreamRangedDescription);
        return 0;
    }
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus stream_set(bool input, const AudioObjectPropertyAddress *a, UInt32 inDataSize, const void *inData)
{
    switch (a->mSelector) {
    case kAudioStreamPropertyVirtualFormat:
    case kAudioStreamPropertyPhysicalFormat: {
        if (inDataSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        const AudioStreamBasicDescription *d = inData;
        int f = format_from_asbd(d);
        if (f < 0 || (input && f != kFmt_Float32) || !rate_supported(d->mSampleRate)) return kAudioDeviceUnsupportedFormatError;
        pthread_mutex_lock(&gStateMutex);
        bool change = false;
        if (d->mSampleRate != gRate) { gPendingRate = d->mSampleRate; change = true; }
        if (!input && (fmt_t)f != gFormat) { gPendingFormat = f; change = true; }
        pthread_mutex_unlock(&gStateMutex);
        return change ? request_change() : 0;
    }
    }
    return kAudioHardwareUnknownPropertyError;
}

/* ---- property dispatch ------------------------------------------------------- */

static bool is_stream(AudioObjectID obj)
{
    return obj == kObjectID_Stream_Output || obj == kObjectID_Stream_Input;
}

static Boolean SM_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a)
{
    (void)pid;
    if (inDriver != gDriverRef || !a) return false;
    if (obj == kObjectID_PlugIn) return plugin_has(a);
    if (obj == kObjectID_Device) return device_has(a);
    if (is_stream(obj)) return stream_has(a);
    return false;
}

static OSStatus SM_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out)
{
    (void)pid;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (!a || !out) return kAudioHardwareIllegalOperationError;
    if (obj == kObjectID_PlugIn) {
        *out = false;
        return plugin_has(a) ? 0 : kAudioHardwareUnknownPropertyError;
    }
    if (obj == kObjectID_Device) return device_settable(a, out);
    if (is_stream(obj)) return stream_settable(a, out);
    return kAudioHardwareBadObjectError;
}

static OSStatus SM_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                       UInt32 qSize, const void *qData, UInt32 *out)
{
    (void)pid; (void)qSize; (void)qData;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (!a || !out) return kAudioHardwareIllegalOperationError;
    if (obj == kObjectID_PlugIn) return plugin_size(a, out);
    if (obj == kObjectID_Device) return device_size(a, out);
    if (is_stream(obj)) return stream_size(obj == kObjectID_Stream_Input, a, out);
    return kAudioHardwareBadObjectError;
}

static OSStatus SM_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                   UInt32 qSize, const void *qData, UInt32 inDataSize, UInt32 *outDataSize, void *outData)
{
    (void)pid;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (!a || !outDataSize || !outData) return kAudioHardwareIllegalOperationError;
    if (obj == kObjectID_PlugIn) return plugin_get(a, qSize, qData, inDataSize, outDataSize, outData);
    if (obj == kObjectID_Device) return device_get(a, inDataSize, outDataSize, outData);
    if (is_stream(obj)) return stream_get(obj == kObjectID_Stream_Input, a, inDataSize, outDataSize, outData);
    return kAudioHardwareBadObjectError;
}

static OSStatus SM_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                   UInt32 qSize, const void *qData, UInt32 inDataSize, const void *inData)
{
    (void)pid; (void)qSize; (void)qData;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (!a || !inData) return kAudioHardwareIllegalOperationError;
    if (obj == kObjectID_PlugIn) return kAudioHardwareUnknownPropertyError;
    if (obj == kObjectID_Device) return device_set(a, inDataSize, inData);
    if (is_stream(obj)) return stream_set(obj == kObjectID_Stream_Input, a, inDataSize, inData);
    return kAudioHardwareBadObjectError;
}

/* ---- IO ---------------------------------------------------------------------- */

static OSStatus SM_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32 client)
{
    (void)client;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gIOMutex);
    if (gIOCount == UINT32_MAX) {
        pthread_mutex_unlock(&gIOMutex);
        return kAudioHardwareIllegalOperationError;
    }
    if (gIOCount++ == 0) {
        gNumTimeStamps = 0;
        gAnchorHost = mach_absolute_time();
        gOthersThrough = -1.0;
        atomic_store(&gMusicCoveredUntil, INT64_MIN);
        begin_run_locked();
    }
    pthread_mutex_unlock(&gIOMutex);
    return 0;
}

static OSStatus SM_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32 client)
{
    (void)client;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gIOMutex);
    if (gIOCount == 0) {
        pthread_mutex_unlock(&gIOMutex);
        return kAudioHardwareIllegalOperationError;
    }
    if (--gIOCount == 0) {
        atomic_store(&gRunning, false);
        os_log(gLog, "ScreamMusic: stop");
        wake_sender();
    }
    pthread_mutex_unlock(&gIOMutex);
    return 0;
}

static OSStatus SM_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32 client,
                                    Float64 *outSampleTime, UInt64 *outHostTime, UInt64 *outSeed)
{
    (void)client;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&gIOMutex);
    UInt64 now = mach_absolute_time();
    Float64 ticksPerPeriod = gHostTicksPerFrame * kZeroTimeStampPeriod;
    UInt64 next = gAnchorHost + (UInt64)((Float64)(gNumTimeStamps + 1) * ticksPerPeriod);
    if (next <= now) gNumTimeStamps++;
    *outSampleTime = (Float64)(gNumTimeStamps * kZeroTimeStampPeriod);
    *outHostTime = gAnchorHost + (UInt64)((Float64)gNumTimeStamps * ticksPerPeriod);
    *outSeed = gClockSeed;
    pthread_mutex_unlock(&gIOMutex);
    return 0;
}

static OSStatus SM_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32 client, UInt32 op,
                                     Boolean *outWillDo, Boolean *outInPlace)
{
    (void)client;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    bool will = op == kAudioServerPlugInIOOperationWriteMix || op == kAudioServerPlugInIOOperationReadInput ||
                op == kAudioServerPlugInIOOperationProcessOutput;
    if (outWillDo) *outWillDo = will;
    if (outInPlace) *outInPlace = true;
    return 0;
}

static OSStatus SM_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32 client, UInt32 op,
                                    UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle)
{
    (void)client; (void)op; (void)frames; (void)cycle;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    return 0;
}

/* Converts one IO buffer (physical format) to wire bytes and appends it to the ring. */
static void write_mix(const void *buffer, UInt32 frames)
{
    const stream_t *st = &gStream;
    unsigned outFrame = scream_frame_bytes(st->wire);
    uint64_t need = (uint64_t)frames * outFrame;
    uint64_t w = atomic_load_explicit(&gRingWrite, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&gRingRead, memory_order_acquire);
    atomic_fetch_add_explicit(&gStatFramesIn, frames, memory_order_relaxed);
    atomic_fetch_add_explicit(&gStatRunFrames, frames, memory_order_relaxed);
    if (kRingBytes - (w - r) < need) {
        atomic_fetch_add_explicit(&gStatOverflowBytes, need, memory_order_relaxed);
        return;
    }

    enum { kChunkFrames = 256 };
    uint8_t tmp[kChunkFrames * 8];
    const uint8_t *src = buffer;
    unsigned inBytes = st->format == kFmt_Float32 ? 4 : format_bytes(st->format);
    unsigned offGrid = 0;

    for (UInt32 done = 0; done < frames;) {
        UInt32 n = frames - done < kChunkFrames ? frames - done : kChunkFrames;
        if (st->format == kFmt_Float32)
            offGrid += scream_pack_float(tmp, (const float *)(const void *)src, (size_t)n * kChannels, st->wire);
        else
            scream_pack_int(tmp, src, (size_t)n * kChannels, format_bits(st->format), inBytes, st->wire);
        size_t bytes = (size_t)n * outFrame;
        size_t off = (size_t)(w % kRingBytes);
        size_t first = bytes < kRingBytes - off ? bytes : kRingBytes - off;
        memcpy(gRing + off, tmp, first);
        memcpy(gRing, tmp + first, bytes - first);
        w += bytes;
        src += (size_t)n * kChannels * inBytes;
        done += n;
    }
    if (offGrid) {
        atomic_fetch_add_explicit(&gStatOffGrid, offGrid, memory_order_relaxed);
        atomic_fetch_add_explicit(&gStatRunOffGrid, offGrid, memory_order_relaxed);
    }
    if (st->format == kFmt_Float32) {
        const float *f = buffer;
        double scale = (double)(1u << (scream_sample_size_byte(st->wire) - 1));
        for (UInt32 i = 0; i < frames * kChannels; i++) {
            double y = (double)f[i] * scale, r = __builtin_rint(y);
            if (r == 0) continue;
            gFitYR += y * r;
            gFitRR += r * r;
            gFitDev += y > r ? y - r : r - y;
            gFitN += 1;
        }
        if (gFitRR > 0) {
            double g = gFitYR / gFitRR, d = gFitDev / gFitN;
            uint64_t gb, db;
            memcpy(&gb, &g, 8);
            memcpy(&db, &d, 8);
            atomic_store_explicit(&gStatRunGain, gb, memory_order_relaxed);
            atomic_store_explicit(&gStatRunDev, db, memory_order_relaxed);
        }
    }
    atomic_store_explicit(&gRingWrite, w, memory_order_release);
}

/*
 * Apple Music mode, per client before the HAL's mix (canonical Float32): a client that isn't
 * Music is summed into the others loopback and zeroed, so the mix is Music + exact zeros.
 * Frames past everything written so far replace an old lap; the rest add (Nativerate's 1.1.5
 * rule: clients of one cycle need not share a sample time).
 */
/* On while the controlling app owns the device, or a Music pid is set (the harness sets only the
 * pid). Pid 0 means Music is not running, not that the mode is off. */
static bool music_mode_on(void)
{
    return atomic_load_explicit(&gMusicPID, memory_order_relaxed) != 0
        || atomic_load_explicit(&gOwnerPID, memory_order_relaxed) != 0;
}

static void process_output(UInt32 client, Float32 *buf, UInt32 frames, Float64 sampleTime)
{
    atomic_store_explicit(&gClientLastOutput[client % kClientSlots], mach_absolute_time(), memory_order_relaxed);
    if (sampleTime < 0.0 || gStream.format != kFmt_Float32 || !music_mode_on()) return;
    pid_t music = atomic_load_explicit(&gMusicPID, memory_order_relaxed);
    /* pid 0 is also what client_pid returns for an unknown client: never treat that as Music */
    if (music && client_pid(client) == music) {
        int64_t end = (int64_t)sampleTime + frames;
        int64_t cur = atomic_load_explicit(&gMusicCoveredUntil, memory_order_relaxed);
        if (end > cur || cur - end > (int64_t)kOthersFrames) /* later, or the time line went back */
            atomic_store_explicit(&gMusicCoveredUntil, end, memory_order_relaxed);
        atomic_store_explicit(&gDiagMusicSample, (int64_t)sampleTime, memory_order_relaxed);
        atomic_store_explicit(&gDiagMusicFrames, frames, memory_order_relaxed);
        return;
    }
    if (gOthersThrough - sampleTime > kOthersFrames / 2) gOthersThrough = -1.0;   /* time went back */
    UInt64 start = (UInt64)sampleTime % kOthersFrames;
    for (UInt32 i = 0; i < frames; i++) {
        UInt64 k = ((start + i) % kOthersFrames) * kChannels;
        if (sampleTime + i >= gOthersThrough) { gOthers[k] = buf[i * 2]; gOthers[k + 1] = buf[i * 2 + 1]; }
        else { gOthers[k] += buf[i * 2]; gOthers[k + 1] += buf[i * 2 + 1]; }
    }
    if (sampleTime + frames > gOthersThrough) gOthersThrough = sampleTime + frames;
    memset(buf, 0, (size_t)frames * kChannels * sizeof(Float32));
    atomic_fetch_add_explicit(&gStatOthersFrames, frames, memory_order_relaxed);
}

/* The others loopback, read back on the input stream and cleared as it is read. */
static void read_input(Float32 *buf, UInt32 frames, Float64 sampleTime)
{
    if (sampleTime < 0.0 || !music_mode_on()) {
        memset(buf, 0, (size_t)frames * kChannels * sizeof(Float32));
        return;
    }
    UInt64 start = (UInt64)sampleTime % kOthersFrames;
    for (UInt32 i = 0; i < frames; i++) {
        UInt64 k = ((start + i) % kOthersFrames) * kChannels;
        buf[i * 2] = gOthers[k];
        buf[i * 2 + 1] = gOthers[k + 1];
        gOthers[k] = gOthers[k + 1] = 0;
    }
}

static OSStatus SM_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, AudioObjectID stream, UInt32 client,
                                 UInt32 op, UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle, void *main, void *secondary)
{
    (void)stream; (void)secondary;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    if (!main || !cycle) return 0;
    switch (op) {
    case kAudioServerPlugInIOOperationProcessOutput:
        process_output(client, main, frames, cycle->mOutputTime.mSampleTime);
        break;
    case kAudioServerPlugInIOOperationWriteMix:
        if (!atomic_load_explicit(&gRunning, memory_order_relaxed)) break;
        /* Apple Music mode: only cycles Music played in (the device also runs for the app reading
         * the others loopback, and for other apps, whose output is moved there) */
        atomic_store_explicit(&gDiagMixSample, (int64_t)cycle->mOutputTime.mSampleTime, memory_order_relaxed);
        atomic_store_explicit(&gDiagMixFrames, frames, memory_order_relaxed);
        if (music_mode_on() && gStream.format == kFmt_Float32) {
            int64_t covered = atomic_load_explicit(&gMusicCoveredUntil, memory_order_relaxed);
            int64_t t = (int64_t)cycle->mOutputTime.mSampleTime;
            /* not covered by Music: it isn't playing (allow one cycle of slack for ordering) */
            if (covered == INT64_MIN || t >= covered + (int64_t)frames || covered - t > (int64_t)kOthersFrames) {
                atomic_fetch_add_explicit(&gStatGateSkipped, 1, memory_order_relaxed);
                break;
            }
        }
        write_mix(main, frames);
        break;
    case kAudioServerPlugInIOOperationReadInput:
        read_input(main, frames, cycle->mInputTime.mSampleTime);
        break;
    }
    return 0;
}

static OSStatus SM_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDevice, UInt32 client, UInt32 op,
                                  UInt32 frames, const AudioServerPlugInIOCycleInfo *cycle)
{
    (void)client; (void)op; (void)frames; (void)cycle;
    if (inDriver != gDriverRef || inDevice != kObjectID_Device) return kAudioHardwareBadObjectError;
    return 0;
}
