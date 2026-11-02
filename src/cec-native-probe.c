#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdint.h>
#include <dlfcn.h>
#include <string.h>
#include <sys/time.h>
#include <mach/mach.h>
typedef struct { uint8_t data[16]; uint8_t length; } CECFrame;
static double now(void){struct timeval t;gettimeofday(&t,0);return t.tv_sec+t.tv_usec/1e6;}
int main(int argc,char**argv){setbuf(stdout,0);if(argc>2||(argc==2&&strcmp(argv[1],"poll"))){puts("Usage: cec-native-probe [poll]");return 2;}void*h=dlopen("/System/Library/PrivateFrameworks/IOCEC.framework/IOCEC",RTLD_NOW);if(!h){puts(dlerror());return 3;}void*(*create)(CFAllocatorRef)=dlsym(h,"IOCECInterfaceCreate");void*iface=create(0);printf("CEC interface: %p\n",iface);if(!iface)return 4;CFShow(iface);
 if(argc==2){int(*mask)(void*,uint32_t)=dlsym(h,"IOCECInterfaceSetLogicalAddressMask");int(*send)(void*,CECFrame,uint8_t)=dlsym(h,"IOCECInterfaceSendFrame");int k=mask(iface,1u<<4);printf("Address mask playback-1: 0x%08x\n",k);if(k){CFRelease(iface);return 5;}
 // A single CEC polling header: playback address 4 -> TV address 0.
 // Final ABI argument is an 8-bit field restricted by this driver to 0..5;
 // it is written to TX control bits 4..6, NOT a millisecond timeout.
 CECFrame frame={{0x40},1};double start=now();puts("Sending one polling frame 40, TX field=0");k=send(iface,frame,0);printf("Send result: 0x%08x (%s), elapsed=%.3f seconds\n",k,mach_error_string(k),now()-start);int m=mask(iface,0);printf("Release address mask: 0x%08x\n",m);}
 CFRelease(iface);puts("Interface closed");return 0;}
