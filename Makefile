# Build the CEC tools for the Apple TV 3 (armv7, iOS 8).
#
# TOOLCHAIN must point to a directory containing:
#   iPhoneOS8.4.sdk/   iOS 8.4 SDK (ships with Xcode 6.4 / 7.x)
#   bin/ld64-242       the ld64 from Xcode 6.4 — the current linker emits
#                      armv7 binaries that crash on the iOS 8 dyld
# ldid (brew install ldid) signs the binaries; the two kernel tools need the
# entitlements in entitlements.plist (task_for_pid-allow) or task_for_pid(0)
# fails on the device.
#
#   make TOOLCHAIN=/path/to/toolchain

TOOLCHAIN ?= $(abspath ../toolchain)
SDK      ?= $(TOOLCHAIN)/iPhoneOS8.4.sdk
LD64     ?= $(TOOLCHAIN)/bin/ld64-242
CC       := xcrun clang
TARGET   := -target armv7-apple-ios8.0 -isysroot $(SDK)
LDFLAGS  := $(TARGET) -mlinker-version=242 -fuse-ld=$(LD64) -Wl,-w \
            -framework CoreFoundation

# -O0 on purpose: with clang 21, -Os miscompiles cec-send-nohpd.c for this
# target (process is SIGKILLed on launch, verified on device). -O0 runs fine.
CFLAGS   := $(TARGET) -O0 -g -Wall

TOOLS := build/cec-controller-init build/cec-send-nohpd build/cec-native-probe \
         build/cec-hdmi-watch build/cecd

all: $(TOOLS)

build:
	mkdir -p build

build/cec-controller-init: src/cec-controller-init.c entitlements.plist | build
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@
	ldid -S entitlements.plist $@

build/cec-send-nohpd: src/cec-send-nohpd.c entitlements.plist | build
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@
	ldid -S entitlements.plist $@

build/cec-native-probe: src/cec-native-probe.c | build
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@
	ldid -S $@

build/cec-hdmi-watch: src/cec-hdmi-watch.c | build
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@
	ldid -S $@

build/cecd: src/cecd.c | build
	$(CC) $(CFLAGS) $(LDFLAGS) $< -o $@
	ldid -S $@

clean:
	rm -rf build

.PHONY: all clean
