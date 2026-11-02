// AppleTV3,2 / 12H1006 only. Initialize the native CEC receive event source,
// set the controller capability byte, and reinitialize HDMI through its API.
// The private client vtable is restored before releasing the client.
// No executable bytes or boot images are modified.
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>
static mach_port_t kt;
static char boot_uuid[80];
static unsigned boot_records;
static int save_boot_state(void) {
  const char *tmp = "/var/root/cec-runtime/boot-log-state.new";
  FILE *f = fopen(tmp, "w");
  if (!f)
    return 41;
  fprintf(f, "%s %u\n", boot_uuid, boot_records);
  int failed = fflush(f) || fsync(fileno(f));
  if (fclose(f))
    failed = 1;
  if (failed || rename(tmp, "/var/root/cec-runtime/boot-log-state"))
    return 41;
  return 0;
}

static int rd(uint32_t a, void *b, size_t n) {
  vm_size_t actual = 0;
  return vm_read_overwrite(kt, a, n, (vm_address_t)b, &actual) || actual != n;
}
static int wr(uint32_t a, uint32_t v) {
  int k = vm_write(kt, a, (vm_offset_t)&v, 4);
  uint32_t q = 0;
  return k || rd(a, &q, 4) || q != v;
}
int main(int argc, char **argv) {
  setbuf(stdout, 0);
  int activate = argc == 2 && !strcmp(argv[1], "--activate");
  if (argc != 2 || (!activate && strcmp(argv[1], "--inspect"))) {
    puts("Usage: cec-controller-init --inspect|--activate");
    return 2;
  }
  struct utsname u;
  if (uname(&u) || strcmp(u.machine, "AppleTV3,2") ||
      (!strstr(u.version, "xnu-2784.40.6~93/MarijuanARM_S5L8947X") &&
       !strstr(u.version, "xnu-2784.40.6~93/RELEASE_ARM_S5L8947X"))) {
    puts("Unsupported device/kernel");
    return 3;
  }
  size_t uuid_size = sizeof boot_uuid;
  if (sysctlbyname("kern.bootsessionuuid", boot_uuid, &uuid_size, 0, 0)) {
    puts("No boot session identity");
    return 4;
  }
  boot_uuid[sizeof boot_uuid - 1] = 0;
  char saved_uuid[80] = {0};
  unsigned saved_records = 0;
  FILE *checkpoint = fopen("/var/root/cec-runtime/boot-log-state", "r");
  if (!checkpoint) {
    puts("No verified boot-log checkpoint");
    return 4;
  }
  int checkpoint_ok =
      fscanf(checkpoint, "%79s %u", saved_uuid, &saved_records) == 2;
  fclose(checkpoint);
  if (!checkpoint_ok) {
    puts("Invalid boot-log checkpoint");
    return 4;
  }
  FILE *f = fopen("/var/logs/untetherhomedepot.log", "r");
  if (!f)
    return 4;
  char line[512];
  unsigned base = 0, v;
  while (fgets(line, sizeof line, f)) {
    if (strstr(line, "[*] executing main")) {
      base = 0;
      boot_records++;
    }
    if (sscanf(line, "[*] kernelbase=0x%x", &v) == 1)
      base = v;
  }
  fclose(f);
  if (boot_records < saved_records ||
      (strcmp(saved_uuid, boot_uuid) && boot_records <= saved_records)) {
    puts("No new untether record for this boot session");
    return 4;
  }
  printf("Boot session=%s untether record=%u\n", boot_uuid, boot_records);
  if (base < 0x80001000 || base > 0xb0001000 ||
      ((base - 0x80001000) & 0x1fffff)) {
    puts("Invalid kernel base in log");
    return 5;
  }
  unsigned slide = base - 0x80001000;
  printf("Kernel base=%08x slide=%08x\n", base, slide);
  if (task_for_pid(mach_task_self(), 0, &kt)) {
    puts("No kernel task access");
    return 6;
  }
  uint32_t hdr[7];
  if (rd(base, hdr, sizeof hdr) || hdr[0] != 0xfeedface || hdr[1] != 12) {
    puts("Kernel identity mismatch");
    return 7;
  }
  unsigned char getter[6], truth[4];
  const unsigned char expected[] = {0x90, 0xf8, 0xb4, 2, 0x70, 0x47},
                      truecode[] = {1, 0x20, 0x70, 0x47};
  if (rd(0x80a397bc + slide, getter, 6) || memcmp(getter, expected, 6) ||
      rd(0x80a3e5c4 + slide, truth, 4) || memcmp(truth, truecode, 4)) {
    puts("CEC code identity mismatch");
    return 8;
  }
  uint32_t addr[] = {0x80a41570 + slide, 0x80a41a70 + slide,
                     0x80a41f70 + slide},
           old[3], orig = 0x80a397bd + slide, on = 0x80a3e5c5 + slide;
  for (int i = 0; i < 3; i++) {
    if (rd(addr[i], &old[i], 4) || (old[i] != orig)) {
      puts("Unexpected CEC vtable entry; no writes");
      return 9;
    }
    printf("CEC vtable %08x = %08x (%s)\n", addr[i], old[i],
           old[i] == on ? "enabled" : "original");
  }
  {
    uint32_t p = 0;
    if (rd(0x804041a4 + slide, &p, 4))
      return 11;
    for (int j = 0; j < 2048; j++) {
      uint32_t q[5];
      if (!p || rd(p, q, sizeof q))
        return 12;
      if (q[2] == (uint32_t)getpid()) {
        uint32_t t[0x220 / 4];
        printf("Own proc=%08x task=%08x\n", p, q[3]);
        if (rd(q[3], t, sizeof t))
          return 13;
        void *h = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",
                         RTLD_NOW);
        void *(*create)(CFAllocatorRef) = dlsym(h, "IOAVControllerCreate");
        void *c = create(0);
        if (!c)
          return 17;
        uint32_t connection = ((uint32_t *)c)[3];
        uint32_t space = t[0x1a8 / 4], sp[10];
        if (rd(space, sp, sizeof sp))
          return 15;
        for (int x = 0; x < 10; x++)
          printf("space+%02x=%08x\n", x * 4, sp[x]);
        printf("connection=%x self=%x\n", connection, mach_task_self());
        if (sp[7] != q[3])
          return 16;
        printf("table=%08x\n", sp[6]);
        uint32_t entry[4];
        if ((connection >> 8) >= sp[4] ||
            rd(sp[6] + (connection >> 8) * 16, entry, sizeof entry))
          return 18;
        printf("entry=%08x %08x %08x %08x\n", entry[0], entry[1], entry[2],
               entry[3]);
        uint32_t port[0x48 / 4];
        if (rd(entry[0], port, sizeof port))
          return 19;
        printf("port bits=%08x object=%08x\n", port[0], port[0x44 / 4]);
        uint32_t obj[0x100 / 4];
        if (rd(port[0x44 / 4], obj, sizeof obj))
          return 20;
        uint32_t provider[0x2e0 / 4];
        if (rd(obj[0x78 / 4], provider, sizeof provider))
          return 21;
        printf("HDMI provider=%08x vtable=%08x\n", obj[0x78 / 4], provider[0]);
        if (obj[0] != 0x80984e38 + slide || provider[0] != 0x80a41b98 + slide) {
          puts("Unexpected HDMI objects");
          CFRelease(c);
          return 22;
        }
        unsigned capability = provider[0x2b4 / 4] & 0xff;
        printf("CEC capability=%u source=%08x\n", capability,
               provider[0x148 / 4]);
        if (!activate || (capability == 1 && provider[0x148 / 4])) {
          CFRelease(c);
          return activate ? save_boot_state() : 0;
        }
        uint8_t gadget[4], expectedg[] = {8, 0x46, 0x70, 0x47};
        if (rd(0x800266f6 + slide, gadget, 4) || memcmp(gadget, expectedg, 4))
          return 23;
        uint32_t vt[0x390 / 4];
        if (rd(obj[0], vt, sizeof vt) || vt[0x384 / 4] != 0x8031f6d9 + slide ||
            vt[0x388 / 4] != 0x8031f6dd + slide)
          return 24;
        vm_address_t page = 0;
        if (vm_allocate(kt, &page, 4096, VM_FLAGS_ANYWHERE))
          return 25;
        vt[0x384 / 4] = 0x800266f7 + slide;
        uint32_t trap[3] = {obj[0x78 / 4], 0x80a397bd + slide, 0};
        if (vm_write(kt, page, (vm_offset_t)vt, sizeof vt) ||
            vm_write(kt, page + 0x400, (vm_offset_t)trap, sizeof trap)) {
          vm_deallocate(kt, page, 4096);
          return 26;
        }
        unsigned (*fixedtrap)(unsigned, unsigned, unsigned, unsigned, unsigned,
                              unsigned, unsigned, unsigned) =
            dlsym(h, "IOConnectTrap6");
        if (!fixedtrap) {
          vm_deallocate(kt, page, 4096);
          return 27;
        }
        if (wr(port[0x44 / 4], page)) {
          // Retain the clone if restoration cannot be verified.
          if (!wr(port[0x44 / 4], obj[0]))
            vm_deallocate(kt, page, 4096);
          return 28;
        }
        unsigned result = fixedtrap(connection, page + 0x400, 0, 0, 0, 0, 0, 0);
        printf("Original capability=%u\n", result);
        int initialization = 0, changed = 0;
        uint32_t source = provider[0x148 / 4];
        if (activate && result == 0 && !source) {
          changed = 1;
          trap[1] = 0x809718f5 + slide;
          if (vm_write(kt, page + 0x400, (vm_offset_t)trap, sizeof trap))
            initialization = 30;
          else {
            source = fixedtrap(connection, page + 0x400, 0x80a367ed + slide, 1,
                               0, 0, 0, 0);
            printf("CEC async source=%08x\n", source);
            if (source < 0x80000000 || source >= 0xc0000000)
              initialization = 31;
          }
          uint32_t sourcevt = 0, othervt = 0;
          if (!initialization &&
              (rd(source, &sourcevt, 4) ||
               rd(provider[0x144 / 4], &othervt, 4) || sourcevt != othervt))
            initialization = 32;
          if (!initialization) {
            trap[0] = provider[0x124 / 4];
            trap[1] = 0x80302d75 + slide;
            if (vm_write(kt, page + 0x400, (vm_offset_t)trap, sizeof trap))
              initialization = 33;
            else {
              unsigned add =
                  fixedtrap(connection, page + 0x400, source, 0, 0, 0, 0, 0);
              printf("Attach CEC source=%08x\n", add);
              if (add)
                initialization = 34;
            }
          }
          if (!initialization && wr(obj[0x78 / 4] + 0x148, source))
            initialization = 35;
          if (!initialization) {
            uint8_t one = 1;
            if (vm_write(kt, obj[0x78 / 4] + 0x2b4, (vm_offset_t)&one, 1))
              initialization = 36;
          }
        } else if (activate && !(result == 1 && source)) {
          puts("Inconsistent CEC state; activation refused");
          initialization = 37;
        }
        int failed = wr(port[0x44 / 4], obj[0]);
        printf("Original client restored=%s initialization=%d\n",
               failed ? "NO" : "yes", initialization);
        if (failed)
          return 29;
        vm_deallocate(kt, page, 4096);
        if (activate && changed && !initialization) {
          uint8_t power = 0;
          int (*getpower)(void *, uint8_t *) =
              dlsym(h, "IOAVControllerGetPower");
          int (*setpower)(void *, uint8_t) = dlsym(h, "IOAVControllerSetPower");
          int (*hotplug)(void *) = dlsym(h, "IOAVControllerForceHotPlugDetect");
          int k = getpower(c, &power);
          printf("HDMI power=%u result=%08x\n", power, k);
          if (k)
            initialization = 38;
          if (!k && power) {
            int off = setpower(c, 0);
            int on = setpower(c, 1);
            printf("HDMI reinitialization off=%08x on=%08x\n", off, on);
            if (off || on)
              initialization = 38;
          }
          int hp = hotplug(c);
          printf("HDMI hotplug=%08x\n", hp);
          if (hp)
            initialization = 39;
        }
        CFRelease(c);
        if (!initialization)
          initialization = save_boot_state();
        return initialization;
      }
      p = q[1];
    }
    return 14;
  }
}
