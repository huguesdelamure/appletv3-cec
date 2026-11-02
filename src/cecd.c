// cecd: HDMI-CEC for the Apple TV 3 (root daemon, LaunchDaemon fr.appletvrevival.cecd).
//
// Needs the CEC controller activated by /var/root/cec-runtime (diagnostics/cec):
// until then IOCECInterfaceCreate returns NULL and cecd retries every 5 s.
//
// What it does:
//   - claims a playback logical address (4, 8 or 11) and answers the TV's
//     queries (physical address, name, vendor, power status, CEC version),
//     presenting itself as an LG SIMPLINK device (see simplink());
//   - Darwin notification "fr.appletvrevival.cec.wake" (display woke up):
//     <Image View On> to the TV, then <Active Source>: the TV turns on and
//     switches to the Apple TV input. If the TV was in standby (HPD low, no
//     CEC interface), the two frames go through /var/root/cec-runtime/cec-send-nohpd
//     straight to the HDMI controller;
//   - "fr.appletvrevival.cec.sleep" (display about to sleep): <Standby> to the
//     TV, only if the Apple TV is the source on screen (no TV turned off while
//     another input is being watched). Answered by "...cec.sleep.done".
//   - <Standby> from the TV while the Apple TV is on screen: posts
//     "fr.appletvrevival.cec.tvstandby" (ModernLauncher puts the Apple TV to sleep).
// The notifications are posted by ModernLauncher (App/Core/MLCEC.m).
//
// IOCEC.framework is private: signatures recovered from its code and from its
// CoreRC caller (CECIOCECInterfaceListener). The frame is passed by value.
//
//   cecd               daemon
//   cecd post <name>   posts fr.appletvrevival.cec.<name> (tests: wake, sleep)
//   cecd tx 4f 36      has the daemon send one raw frame (tests; it owns the bus)
//   touch /var/root/cecd/disabled   CEC off (the daemon stays idle)

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <notify.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define NOTE_WAKE "fr.appletvrevival.cec.wake"
#define NOTE_SLEEP "fr.appletvrevival.cec.sleep"
#define NOTE_SLEEP_DONE "fr.appletvrevival.cec.sleep.done"
#define NOTE_TV_STANDBY "fr.appletvrevival.cec.tvstandby"
#define NOTE_TX "fr.appletvrevival.cec.tx"
#define TX_FILE "/var/root/cecd/tx"
#define DISABLED_FILE "/var/root/cecd/disabled"
#define CEC_RUNTIME_DISABLED "/var/root/cec-runtime/disabled"
#define NOHPD_SEND "/var/root/cec-runtime/cec-send-nohpd"
#define OSD_NAME "Apple TV"

#define kNotResponding 0xe00002ed

typedef struct { uint8_t data[16]; uint8_t length; } CECFrame;
typedef void *CECRef;
typedef void (*CECReceiveFn)(CECRef, CECFrame, void *);
typedef void (*CECTerminatedFn)(CECRef, void *);

static CECRef (*cecCreate)(CFAllocatorRef);
static int (*cecSetMask)(CECRef, uint32_t);
static int (*cecSend)(CECRef, CECFrame, uint8_t);
static int (*cecOpenQueue)(CECRef, uint32_t, uint32_t);
static void (*cecOnReceive)(CECRef, CECReceiveFn, void *);
static void (*cecOnTerminated)(CECRef, CECTerminatedFn, void *);
static void (*cecSchedule)(CECRef, CFRunLoopRef, CFStringRef);
static void (*cecUnschedule)(CECRef, CFRunLoopRef, CFStringRef);
static CFTypeRef (*cecCopyProperty)(CECRef, CFStringRef);
static void *(*avCreate)(CFAllocatorRef);
static CFMutableDictionaryRef (*ioMatching)(const char *);
static unsigned (*ioGetService)(mach_port_t, CFDictionaryRef);
static CFTypeRef (*ioCopyProperty)(unsigned, CFStringRef, CFAllocatorRef, unsigned);
static int (*ioRelease)(unsigned);
static int (*avForceHotPlug)(void *);

static CECRef gIface;
static uint8_t gLA = 15;            // our logical address (15 = none)
static uint16_t gPA = 0x1000;       // our physical address (1.0.0.0 until read)
static int gAwake = 1;              // display state, from the notifications
static int gActive;                 // we are the source shown by the TV
static int gWakePending;            // wake asked while no interface: replay on open
static double gWakeDeadline;

static double now(void) { struct timeval t; gettimeofday(&t, 0); return t.tv_sec + t.tv_usec / 1e6; }

static void logf_(const char *fmt, ...) {
    char ts[32]; time_t t = time(0);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    printf("%s ", ts);
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    putchar('\n');
}

static int load(void) {
    void *h = dlopen("/System/Library/PrivateFrameworks/IOCEC.framework/IOCEC", RTLD_NOW);
    void *k = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
    if (!h || !k) return 0;
    cecCreate = dlsym(h, "IOCECInterfaceCreate");
    cecSetMask = dlsym(h, "IOCECInterfaceSetLogicalAddressMask");
    cecSend = dlsym(h, "IOCECInterfaceSendFrame");
    cecOpenQueue = dlsym(h, "IOCECInterfaceOpenReceiveQueue");
    cecOnReceive = dlsym(h, "IOCECInterfaceRegisterReceiveCallback");
    cecOnTerminated = dlsym(h, "IOCECInterfaceRegisterTerminatedCallback");
    cecSchedule = dlsym(h, "IOCECInterfaceScheduleWithRunLoop");
    cecUnschedule = dlsym(h, "IOCECInterfaceUnscheduleFromRunLoop");
    cecCopyProperty = dlsym(h, "IOCECInterfaceCopyProperty");
    avCreate = dlsym(k, "IOAVControllerCreate");
    avForceHotPlug = dlsym(k, "IOAVControllerForceHotPlugDetect");
    ioMatching = dlsym(k, "IOServiceMatching");
    ioGetService = dlsym(k, "IOServiceGetMatchingService");
    ioCopyProperty = dlsym(k, "IORegistryEntryCreateCFProperty");
    ioRelease = dlsym(k, "IOObjectRelease");
    return ioMatching && ioGetService && ioCopyProperty && ioRelease && cecCreate && cecSetMask && cecSend && cecOpenQueue && cecOnReceive && cecOnTerminated &&
           cecSchedule && cecUnschedule && cecCopyProperty && avCreate && avForceHotPlug;
}

static void hex(const uint8_t *b, int n, char *out) {
    out[0] = 0;
    for (int i = 0; i < n; i++) sprintf(out + strlen(out), i ? ":%02x" : "%02x", b[i]);
}

static int tx(const uint8_t *b, int n) {
    if (!gIface) return -1;
    CECFrame f; memset(&f, 0, sizeof f);
    memcpy(f.data, b, n); f.length = n;
    int r = cecSend(gIface, f, 0);
    if (r && r != (int)kNotResponding) r = cecSend(gIface, f, 0);   // one retry (bus busy)
    char s[64]; hex(b, n, s);
    logf_("tx %s -> 0x%08x", s, r);
    return r;
}

// Header byte: source in the high nibble, destination in the low one.
static uint8_t hdr(uint8_t dst) { return (uint8_t)(gLA << 4 | dst); }

static void reportPhysicalAddress(void) {
    uint8_t m[] = {hdr(15), 0x84, gPA >> 8, gPA & 0xff, 0x04};   // device type 4: playback
    tx(m, sizeof m);
}

static void activeSource(void) {
    uint8_t m[] = {hdr(15), 0x82, gPA >> 8, gPA & 0xff};
    if (tx(m, sizeof m) == 0) gActive = 1;
}

static void tvOn(void) {
    uint8_t ivo[] = {hdr(0), 0x04};   // <Image View On>
    tx(ivo, sizeof ivo);
    activeSource();
}

static void tvStandby(void) {
    if (!gActive) { logf_("sleep: another source is on screen, TV left on"); return; }
    uint8_t m[] = {hdr(0), 0x36};     // <Standby>, to the TV only (not the whole bus)
    tx(m, sizeof m);
    gActive = 0;
}

// From the TV's EDID, published on the IOAVCECControlInterface registry entry.
static uint16_t readPhysicalAddress(void) {
    CFTypeRef v = NULL;
    unsigned service = ioGetService(0, ioMatching("IOAVCECControlInterface"));
    if (service) {
        v = ioCopyProperty(service, CFSTR("Physical Address"), kCFAllocatorDefault, 0);
        ioRelease(service);
    }
    uint16_t pa = 0;
    if (v && CFGetTypeID(v) == CFDataGetTypeID() && CFDataGetLength(v) >= 2) {
        const uint8_t *b = CFDataGetBytePtr(v);
        pa = (uint16_t)(b[0] << 8 | b[1]);
    }
    if (v) CFRelease(v);
    return pa;
}

static void reportPower(uint8_t dst) {
    uint8_t m[] = {hdr(dst), 0x90, gAwake ? 0x00 : 0x01};
    tx(m, sizeof m);
}

// LG SIMPLINK. LG TVs of this era refuse <Standby> ("refused") and ignore other
// commands from a device whose vendor ID is not LG's: cecd reports LG's ID and
// answers the TV's vendor handshake as libCEC does (SLCommandHandler.cpp):
//   01 init -> 02 05 (ack, HDD recorder);  04 connect -> 05 01 (device mode);
//   03 power on / 0b reconnect / a0 power status -> <Report Power Status>.
static void simplink(const uint8_t *p, int n) {
    switch (p[0]) {
    case 0x01: {
        uint8_t m[] = {hdr(0), 0x89, 0x02, 0x05};
        tx(m, sizeof m);
        break;
    }
    case 0x04: {
        uint8_t m[] = {hdr(0), 0x89, 0x05, 0x01};
        tx(m, sizeof m);
        if (gActive) {
            uint8_t d[] = {hdr(0), 0x1b, 0x20};
            tx(d, sizeof d);
            reportPower(0);
        }
        break;
    }
    case 0x03:
    case 0x0b:
    case 0xa0:
        reportPower(0);
        break;
    }
    (void)n;
}

static void onReceive(CECRef iface, CECFrame f, void *refcon) {
    (void)iface; (void)refcon;
    if (f.length < 1 || f.length > 16) return;
    char s[64]; hex(f.data, f.length, s);
    logf_("rx %s", s);
    if (f.length < 2) return;   // polling
    uint8_t src = f.data[0] >> 4, dst = f.data[0] & 15, op = f.data[1];
    int direct = dst == gLA, bcast = dst == 15;
    if ((!direct && !bcast) || src == gLA) return;
    uint16_t pa = f.length >= 4 ? (uint16_t)(f.data[2] << 8 | f.data[3]) : 0;
    switch (op) {
    case 0x82:   // <Active Source>: someone else is on screen now
        gActive = 0;
        break;
    case 0x80:   // <Routing Change> old, new
        if (f.length >= 6) gActive = (uint16_t)(f.data[4] << 8 | f.data[5]) == gPA;
        break;
    case 0x81:   // <Routing Information>
        if (f.length >= 4) gActive = pa == gPA;
        break;
    case 0x86:   // <Set Stream Path>: the TV selects a source
        gActive = f.length >= 4 && pa == gPA;
        if (gActive && gAwake) activeSource();
        break;
    case 0x85:   // <Request Active Source>
        if (gActive && gAwake) activeSource();
        break;
    case 0x83:   // <Give Physical Address>
        reportPhysicalAddress();
        break;
    case 0x46: { // <Give OSD Name>
        uint8_t m[16] = {hdr(src), 0x47};
        int n = (int)strlen(OSD_NAME);
        memcpy(m + 2, OSD_NAME, n);
        tx(m, 2 + n);
        break;
    }
    case 0x8c: { // <Give Device Vendor ID>: LG (see the SIMPLINK note above)
        uint8_t m[] = {hdr(15), 0x87, 0x00, 0xe0, 0x91};
        tx(m, sizeof m);
        break;
    }
    case 0x89:   // <Vendor Command>: SIMPLINK handshake
        if (src == 0 && direct && f.length >= 3) simplink(f.data + 2, f.length - 2);
        break;
    case 0x1a: { // <Give Deck Status>: LG wants "other status (LG)" from the source on screen
        uint8_t m[] = {hdr(src), 0x1b, gActive ? 0x20 : 0x1a};
        tx(m, sizeof m);
        break;
    }
    case 0x8f:   // <Give Device Power Status>: 0 on, 1 standby
        reportPower(src);
        break;
    case 0x9f: { // <Get CEC Version>: 1.4
        uint8_t m[] = {hdr(src), 0x9e, 0x05};
        tx(m, sizeof m);
        break;
    }
    case 0x8d: { // <Menu Request>: menu is always active
        uint8_t m[] = {hdr(src), 0x8e, 0x00};
        tx(m, sizeof m);
        break;
    }
    case 0x36:   // <Standby>: the TV is going off; the Apple TV follows if it was on screen
        if (gActive && gAwake && src == 0) {
            logf_("TV standby: Apple TV goes to sleep");
            notify_post(NOTE_TV_STANDBY);
        }
        gActive = 0;
        break;
    case 0x00:   // <Feature Abort>
        break;
    default:
        if (direct) {   // <Feature Abort> "unrecognized opcode", never to a broadcast
            uint8_t m[] = {hdr(src), 0x00, op, 0x00};
            tx(m, sizeof m);
        }
    }
}

static void closeInterface(void) {
    if (!gIface) return;
    cecUnschedule(gIface, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    cecSetMask(gIface, 0);
    CFRelease(gIface);
    gIface = NULL;
    gLA = 15;
}

static void onTerminated(CECRef iface, void *refcon) {
    (void)iface; (void)refcon;
    logf_("CEC interface gone (HDMI unplugged or display off)");
    // Not from inside the interface's own callback.
    dispatch_async(dispatch_get_main_queue(), ^{ closeInterface(); });
}

// Playback addresses 4, 8, 11: an address is free if polling it gets no ACK.
static uint8_t claimAddress(void) {
    static const uint8_t candidates[] = {4, 8, 11};
    for (unsigned i = 0; i < sizeof candidates; i++) {
        uint8_t la = candidates[i];
        CECFrame f; memset(&f, 0, sizeof f);
        f.data[0] = (uint8_t)(la << 4 | la); f.length = 1;
        int r = cecSend(gIface, f, 0);
        logf_("poll %x -> 0x%08x", la, r);
        if (r != 0) return la;   // not acknowledged (or poll refused): take it
    }
    return 15;
}

static int disabled(void) { return !access(DISABLED_FILE, F_OK) || !access(CEC_RUNTIME_DISABLED, F_OK); }

static void tryOpen(void) {
    if (gIface || disabled()) return;
    CECRef iface = cecCreate(NULL);
    if (!iface) return;
    gIface = iface;
    int r = cecOpenQueue(iface, 0, 100);
    if (r) { logf_("OpenReceiveQueue 0x%08x", r); CFRelease(iface); gIface = NULL; return; }
    cecOnReceive(iface, onReceive, NULL);
    cecOnTerminated(iface, onTerminated, NULL);
    cecSchedule(iface, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    uint16_t pa = readPhysicalAddress();
    if (pa && pa != 0xffff) gPA = pa;
    else logf_("no physical address from the TV, using %x.%x.%x.%x", gPA >> 12, gPA >> 8 & 15, gPA >> 4 & 15, gPA & 15);
    gLA = claimAddress();
    if (gLA == 15) { logf_("no free playback address"); closeInterface(); return; }
    r = cecSetMask(iface, 1u << gLA);
    logf_("CEC ready: logical %x, physical %x.%x.%x.%x, mask 0x%08x", gLA, gPA >> 12, gPA >> 8 & 15, gPA >> 4 & 15, gPA & 15, r);
    reportPhysicalAddress();
    // Assumption: if the display is on when CEC comes up, the Apple TV is what is watched.
    gActive = gAwake;
    if (gWakePending) {
        gWakePending = 0;
        if (now() < gWakeDeadline) tvOn();
    }
}

static void forceHotPlug(void) {
    void *c = avCreate(NULL);
    if (!c) return;
    int r = avForceHotPlug(c);
    CFRelease(c);
    logf_("hot plug detect forced: 0x%08x", r);
}

// TV in standby cuts HPD: no IOAVCECControlInterface, so no IOCEC path to
// transmit. The CEC line still works, so send <Image View On> then
// <Active Source> straight through the HDMI controller with cec-send-nohpd
// (it forces the controller's link-up flag for the duration of the send).
// Runs on a background queue: system() blocks.
static int gNohpdBusy;
static void nohpdWake(void) {
    if (gNohpdBusy) return;
    if (access(NOHPD_SEND, X_OK)) { logf_("nohpd: %s absent, TV left as is", NOHPD_SEND); return; }
    gNohpdBusy = 1;
    uint8_t la = gLA != 15 ? gLA : 4;
    char cmd[256];
    snprintf(cmd, sizeof cmd, "%s %02x 04", NOHPD_SEND, la << 4);   // <Image View On> to the TV
    int r = system(cmd);
    logf_("nohpd <Image View On> -> exit %d", r);
    sleep(3);   // the TV needs a moment to power up
    snprintf(cmd, sizeof cmd, "%s %02x 82 %02x %02x", NOHPD_SEND,
             la << 4 | 15, gPA >> 8, gPA & 0xff);                   // <Active Source>, broadcast
    r = system(cmd);
    logf_("nohpd <Active Source> -> exit %d", r);
    gNohpdBusy = 0;
}

static void onWake(void) {
    logf_("display wake");
    gAwake = 1;
    if (disabled()) return;
    tryOpen();
    if (gIface) { tvOn(); return; }
    // HDMI may come back a moment after the display: replay when the interface appears.
    gWakePending = 1;
    gWakeDeadline = now() + 20;
    forceHotPlug();
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        nohpdWake();
    });
    for (int i = 1; i <= 8; i++)
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)i * 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            if (gWakePending) tryOpen();
        });
}

static void onSleep(void) {
    logf_("display sleep");
    gAwake = 0;
    gWakePending = 0;
    if (!disabled()) {
        tryOpen();
        if (gIface) tvStandby();
        else logf_("sleep: no CEC interface, TV left as is");
    }
    notify_post(NOTE_SLEEP_DONE);
}

// Test frame written by "cecd tx": hex bytes on one line.
static void onTx(void) {
    FILE *f = fopen(TX_FILE, "r");
    if (!f) return;
    uint8_t b[16]; int n = 0; unsigned v;
    while (n < 16 && fscanf(f, "%x", &v) == 1) b[n++] = (uint8_t)v;
    fclose(f);
    unlink(TX_FILE);
    if (n) { tryOpen(); tx(b, n); }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc >= 3 && !strcmp(argv[1], "tx")) {
        FILE *f = fopen(TX_FILE, "w");
        if (!f) return 1;
        for (int i = 2; i < argc; i++) fprintf(f, "%s ", argv[i]);
        fclose(f);
        return notify_post(NOTE_TX);
    }
    if (argc == 3 && !strcmp(argv[1], "post")) {
        char name[128];
        snprintf(name, sizeof name, "fr.appletvrevival.cec.%s", argv[2]);
        return notify_post(name);
    }
    if (argc != 1) { fprintf(stderr, "usage: cecd | cecd post <wake|sleep> | cecd tx <hex bytes>\n"); return 2; }
    if (!load()) { logf_("IOCEC/IOKit symbols missing"); sleep(60); return 1; }
    logf_("cecd started");
    int t1, t2, t3;
    notify_register_dispatch(NOTE_TX, &t3, dispatch_get_main_queue(), ^(int t) { (void)t; onTx(); });
    notify_register_dispatch(NOTE_WAKE, &t1, dispatch_get_main_queue(), ^(int t) { (void)t; onWake(); });
    notify_register_dispatch(NOTE_SLEEP, &t2, dispatch_get_main_queue(), ^(int t) { (void)t; onSleep(); });
    dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(timer, DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC, NSEC_PER_SEC);
    dispatch_source_set_event_handler(timer, ^{
        if (disabled()) closeInterface();
        else tryOpen();
    });
    dispatch_resume(timer);
    CFRunLoopRun();
    return 0;
}
