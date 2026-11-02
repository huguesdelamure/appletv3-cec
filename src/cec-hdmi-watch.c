// Recheck hot-plug only while the native CEC service is absent.
// Does not open a CEC connection or transmit any CEC frame.
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <stdio.h>
#include <unistd.h>
int main(void) {
  setbuf(stdout, 0);
  void *h =
      dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
  CFMutableDictionaryRef (*matching)(const char *) =
      dlsym(h, "IOServiceMatching");
  unsigned (*get)(mach_port_t, CFDictionaryRef) =
      dlsym(h, "IOServiceGetMatchingService");
  int (*release)(unsigned) = dlsym(h, "IOObjectRelease");
  void *(*create)(CFAllocatorRef) = dlsym(h, "IOAVControllerCreate");
  int (*force)(void *) = dlsym(h, "IOAVControllerForceHotPlugDetect");
  if (!matching || !get || !release || !create || !force)
    return 2;
  int previous = -1;
  while (access("/var/root/cec-runtime/disabled", F_OK)) {
    unsigned service = get(0, matching("IOAVCECControlInterface"));
    int available = service != 0;
    if (service)
      release(service);
    if (available != previous) {
      puts(available ? "HDMI_CEC_INTERFACE_AVAILABLE"
                     : "HDMI_CEC_WAITING_FOR_DISPLAY");
      previous = available;
    }
    if (!available) {
      void *controller = create(0);
      if (controller) {
        force(controller);
        CFRelease(controller);
      }
    }
    sleep(10);
  }
  return 0;
}
