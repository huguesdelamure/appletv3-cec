// cec-send-nohpd: send one HDMI-CEC frame while HPD is low (TV in standby).
//
// AppleTV3,2 / 12H1006 only. When the TV cuts HPD, IOHDMIDevice and
// IOAVCECControlInterface disappear and IOCECInterfaceCreate returns NULL, so
// there is no userland path to transmit. The CEC line itself stays powered:
// a TV in standby still listens, which is exactly what <Image View On> needs.
//
// This tool reaches the HDMI controller object in the kernel (same private
// vtable technique as cec-controller-init.c), temporarily forces its link-up
// flag (offset 0x165) when it is 0, and calls the controller's native sendCEC
// routine (0x80a39978, Thumb) through IOConnectTrap6 with a private trap:
//
//   sendCEC(controller, token=0, frame*)   frame = { data[16], length, tx }
//
// sendCEC writes the frame to the CEC_BASE MMIO registers, blocks until the
// transmission finishes and returns the acknowledge code (0 = ACK,
// 0xe00002ed = not responding). The link-up flag and the connection's client
// vtable are restored afterwards; the temporary page is freed. No executable
// bytes or boot images are modified.
//
// Usage:   cec-send-nohpd 40 04            one frame, hex bytes (1..16)
//          cec-send-nohpd --inspect        read-only precondition check
// Exit 0 if the frame was acknowledged (or inspect OK), 1 if sent but not
// acknowledged, 2..40 for precondition failures (nothing was written then).
//
// Called by cecd on display wake when no CEC interface exists.
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static mach_port_t kt;

static int rd(uint32_t a, void *b, size_t n) {
  vm_size_t actual = 0;
  return vm_read_overwrite(kt, a, n, (vm_address_t)b, &actual) || actual != n;
}
static int wr(uint32_t a, uint32_t v) {
  int k = vm_write(kt, a, (vm_offset_t)&v, 4);
  uint32_t q = 0;
  return k || rd(a, &q, 4) || q != v;
}
static int wrb(uint32_t a, uint8_t v) {
  int k = vm_write(kt, a, (vm_offset_t)&v, 1);
  uint8_t q = 0;
  return k || rd(a, &q, 1) || q != v;
}

static void logf_(const char *fmt, ...) {
  char ts[32]; time_t t = time(0);
  strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
  printf("%s nohpd: ", ts);
  va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
  putchar('\n');
}

int main(int argc, char **argv) {
  setbuf(stdout, 0);
  int inspect = argc == 2 && !strcmp(argv[1], "--inspect");
  uint8_t frame[16];
  int n = 0;
  for (int i = inspect ? 2 : 1; i < argc && n < 16; i++) {
    char *end = 0;
    long v = strtol(argv[i], &end, 16);
    if (end == argv[i] || v < 0 || v > 0xff) {
      puts("Usage: cec-send-nohpd [--inspect | hex byte ...]  (1..16 bytes)");
      return 2;
    }
    frame[n++] = (uint8_t)v;
  }
  if (!n && !inspect) { puts("Usage: cec-send-nohpd [--inspect | hex byte ...]"); return 2; }
  char hexs[64] = {0};
  for (int i = 0; i < n; i++) snprintf(hexs + strlen(hexs), sizeof hexs - strlen(hexs), i ? ":%02x" : "%02x", frame[i]);
  logf_("frame %s (%d bytes)", hexs, n);

  struct utsname u;
  if (uname(&u) || strcmp(u.machine, "AppleTV3,2") ||
      (!strstr(u.version, "xnu-2784.40.6~93/MarijuanARM_S5L8947X") &&
       !strstr(u.version, "xnu-2784.40.6~93/RELEASE_ARM_S5L8947X"))) {
    puts("Unsupported device/kernel");
    return 3;
  }
  // Boot identity: same checkpoint rules as cec-controller-init (read only here).
  char boot_uuid[80];
  size_t uuid_size = sizeof boot_uuid;
  if (sysctlbyname("kern.bootsessionuuid", boot_uuid, &uuid_size, 0, 0)) {
    puts("No boot session identity");
    return 4;
  }
  boot_uuid[sizeof boot_uuid - 1] = 0;
  char saved_uuid[80] = {0};
  unsigned saved_records = 0;
  FILE *checkpoint = fopen("/var/root/cec-runtime/boot-log-state", "r");
  if (!checkpoint) { puts("No verified boot-log checkpoint"); return 4; }
  int checkpoint_ok = fscanf(checkpoint, "%79s %u", saved_uuid, &saved_records) == 2;
  fclose(checkpoint);
  if (!checkpoint_ok) { puts("Invalid boot-log checkpoint"); return 4; }
  FILE *f = fopen("/var/logs/untetherhomedepot.log", "r");
  if (!f) return 4;
  char line[512];
  unsigned base = 0, v, boot_records = 0;
  while (fgets(line, sizeof line, f)) {
    if (strstr(line, "[*] executing main")) { base = 0; boot_records++; }
    if (sscanf(line, "[*] kernelbase=0x%x", &v) == 1) base = v;
  }
  fclose(f);
  if (boot_records < saved_records ||
      (strcmp(saved_uuid, boot_uuid) && boot_records <= saved_records)) {
    puts("No new untether record for this boot session");
    return 4;
  }
  if (base < 0x80001000 || base > 0xb0001000 ||
      ((base - 0x80001000) & 0x1fffff)) {
    puts("Invalid kernel base in log");
    return 5;
  }
  unsigned slide = base - 0x80001000;
  printf("Boot session=%s untether record=%u base=%08x slide=%08x\n",
         boot_uuid, boot_records, base, slide);
  if (task_for_pid(mach_task_self(), 0, &kt)) {
    puts("No kernel task access");
    return 6;
  }
  uint32_t hdr[7];
  if (rd(base, hdr, sizeof hdr) || hdr[0] != 0xfeedface || hdr[1] != 12) {
    puts("Kernel identity mismatch");
    return 7;
  }
  // Identity checks before any write: CEC capability getter, the three CEC
  // vtable slots, the sendCEC prologue, and the private-vtable gadget.
  unsigned char getter[6], truth[4], sendpro[8], gadget[4];
  const unsigned char expected[] = {0x90, 0xf8, 0xb4, 2, 0x70, 0x47},
                      truecode[] = {1, 0x20, 0x70, 0x47},
                      prologue[] = {0xf0, 0xb5, 0x03, 0xaf, 0x2d, 0xe9, 0x00, 0x05},
                      expectedg[] = {8, 0x46, 0x70, 0x47};
  if (rd(0x80a397bc + slide, getter, 6) || memcmp(getter, expected, 6) ||
      rd(0x80a3e5c4 + slide, truth, 4) || memcmp(truth, truecode, 4) ||
      rd(0x80a39978 + slide, sendpro, 8) || memcmp(sendpro, prologue, 8) ||
      rd(0x800266f6 + slide, gadget, 4) || memcmp(gadget, expectedg, 4)) {
    puts("CEC code identity mismatch; no writes");
    return 8;
  }
  uint32_t va[] = {0x80a41570, 0x80a41a70, 0x80a41f70}, orig = 0x80a397bd + slide;
  for (int i = 0; i < 3; i++) {
    uint32_t p;
    if (rd(va[i] + slide, &p, 4) || p != orig) {
      puts("Unexpected CEC vtable entry; no writes");
      return 9;
    }
  }
  // Find our own task, then our IOAV connection and the HDMI controller.
  uint32_t p = 0;
  if (rd(0x804041a4 + slide, &p, 4)) return 11;
  uint32_t connection = 0, obj0 = 0, provider = 0;
  for (int j = 0; j < 2048; j++) {
    uint32_t q[5];
    if (!p || rd(p, q, sizeof q)) return 12;
    if (q[2] == (uint32_t)getpid()) {
      uint32_t t[0x220 / 4];
      if (rd(q[3], t, sizeof t)) return 13;
      void *h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
      void *(*create)(CFAllocatorRef) = dlsym(h, "IOAVControllerCreate");
      if (!h || !create) return 17;
      void *c = create(0);
      if (!c) return 17;
      connection = ((uint32_t *)c)[3];
      uint32_t space = t[0x1a8 / 4], table;
      uint32_t sp[10];
      if (rd(space, sp, sizeof sp)) { CFRelease(c); return 15; }
      if (sp[7] != q[3]) { CFRelease(c); return 16; }
      table = sp[6];
      if ((connection >> 8) >= sp[4]) { CFRelease(c); return 18; }
      uint32_t entry[4];
      if (rd(table + (connection >> 8) * 16, entry, sizeof entry)) { CFRelease(c); return 18; }
      uint32_t port[0x48 / 4];
      if (rd(entry[0], port, sizeof port)) { CFRelease(c); return 19; }
      uint32_t obj[0x100 / 4];
      if (rd(port[0x44 / 4], obj, sizeof obj)) { CFRelease(c); return 20; }
      uint32_t prov[0x2e0 / 4];
      if (rd(obj[0x78 / 4], prov, sizeof prov)) { CFRelease(c); return 21; }
      obj0 = obj[0];
      provider = obj[0x78 / 4];
      if (obj0 != 0x80984e38 + slide || prov[0] != 0x80a41b98 + slide) {
        puts("Unexpected HDMI objects; no writes");
        CFRelease(c);
        return 22;
      }
      unsigned capability = prov[0x2b4 / 4] & 0xff;
      uint32_t source = prov[0x148 / 4];
      printf("HDMI provider=%08x vtable=%08x capability=%u source=%08x gate=%08x\n",
             provider, prov[0], capability, source, prov[0x128 / 4]);
      if (inspect) {
        uint8_t linkup = 0;
        if (!rd(provider + 0x165, &linkup, 1))
          printf("link-up flag (0x165)=%u\n", linkup);
        CFRelease(c);
        puts("INSPECT_OK");
        return 0;
      }
      if (!prov[0x128 / 4]) {   // no command gate: sendCEC would dereference NULL
        puts("HDMI controller has no command gate; no writes");
        CFRelease(c);
        return 23;
      }
      // Private vtable page: copy the client object vtable, point slot 0x384 at
      // the "mov r8, r0; bx lr" gadget, as in cec-controller-init.
      uint32_t vt[0x390 / 4];
      if (rd(obj0, vt, sizeof vt) || vt[0x384 / 4] != 0x8031f6d9 + slide ||
          vt[0x388 / 4] != 0x8031f6dd + slide) { CFRelease(c); return 24; }
      uint8_t linkup = 0;
      int forced = 0;
      if (rd(provider + 0x165, &linkup, 1)) { CFRelease(c); return 25; }
      printf("link-up flag (0x165)=%u\n", linkup);
      vm_address_t page = 0;
      if (vm_allocate(kt, &page, 4096, VM_FLAGS_ANYWHERE)) { CFRelease(c); return 26; }
      vt[0x384 / 4] = 0x800266f7 + slide;
      uint8_t fbuf[18];
      memset(fbuf, 0, sizeof fbuf);
      memcpy(fbuf, frame, n);
      fbuf[16] = (uint8_t)n;   // length
      fbuf[17] = 0;            // TX field (0..5), as IOCECInterfaceSendFrame
      uint32_t trap[3] = {provider, 0x80a39979 + slide, 0};   // sendCEC, Thumb
      if (vm_write(kt, page, (vm_offset_t)vt, sizeof vt) ||
          vm_write(kt, page + 0x200, (vm_offset_t)fbuf, sizeof fbuf) ||
          vm_write(kt, page + 0x400, (vm_offset_t)trap, sizeof trap)) {
        vm_deallocate(kt, page, 4096); CFRelease(c); return 27;
      }
      if (wr(port[0x44 / 4], (uint32_t)page)) {
        vm_deallocate(kt, page, 4096); CFRelease(c); return 28;
      }
      // While HPD is low the controller's link-up flag is 0 and sendCEC would
      // bail out: force it for the duration of the call only.
      if (!linkup) {
        if (wrb(provider + 0x165, 1)) {
          wr(port[0x44 / 4], obj0);   // restore before bailing out
          vm_deallocate(kt, page, 4096); CFRelease(c);
          puts("Could not force link-up flag; no frame sent");
          return 29;
        }
        forced = 1;
      }
      unsigned (*trap6)(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned) =
          dlsym(h, "IOConnectTrap6");
      if (!trap6) {
        if (forced) wrb(provider + 0x165, linkup);
        wr(port[0x44 / 4], obj0);
        vm_deallocate(kt, page, 4096); CFRelease(c);
        return 30;
      }
      // sendCEC(provider, token=0, frame at page+0x200, 0, 0, 0, 0)
      unsigned result = trap6(connection, (unsigned)(page + 0x400),
                              0, (unsigned)(page + 0x200), 0, 0, 0, 0);
      int restore_failed = wr(port[0x44 / 4], obj0);
      int flag_failed = forced ? wrb(provider + 0x165, linkup) : 0;
      printf("sendCEC result=0x%08x client-restored=%s linkup-restored=%s\n",
             result, restore_failed ? "NO" : "yes", flag_failed ? "NO" : "yes");
      if (restore_failed) {
        // Keep the clone page mapped: freeing it would leave a dangling pointer.
        CFRelease(c);
        return 31;
      }
      vm_deallocate(kt, page, 4096);
      CFRelease(c);
      logf_("frame %s -> 0x%08x (%s)", hexs, result,
            result == 0 ? "ACK" : result == 0xe00002ed ? "not responding" : "error");
      return result == 0 ? 0 : 1;
    }
    p = q[1];
  }
  return 14;
}
