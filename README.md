# appletv3-cec — HDMI-CEC for the Apple TV 3

Enables the factory-disabled HDMI-CEC controller of the **Apple TV 3**
(AppleTV3,2 / S5L8947X, iOS 8.4.2 build 12H1006, kernel xnu-2784.40.6~93) and
ships a complete CEC daemon: waking the TV when the Apple TV wakes (even when
the TV is in standby and drops HPD), putting the TV in standby, and answering
the TV's CEC queries (physical address, name, vendor, LG SIMPLINK…).

No kernel code bytes and no boot images are modified: activation happens in
memory, at boot, through `task_for_pid(0)` (tfp0).

> **Warning.** These tools use full kernel access (tfp0). Accessing a wrong
> address can panic the kernel and reboot the device. CEC activation caused a
> black screen and a watchdog on at least one unit during an experimental USB
> boot (see `docs/INVESTIGATION.md`). Use at your own risk, on a jailbroken
> device whose temporary loss you accept. Rollback procedure at the bottom of
> this document.

## Hardware and prerequisites

- 2013 Apple TV 3 (`AppleTV3,2`, S5L8947X SoC), iOS 8.4.2 (12H1006).
  The kernel offsets are specific to this model and build: the tools check
  `uname` and the expected in-memory bytes before any write and refuse to run
  anywhere else.
- An untethered jailbreak providing `task_for_pid(mach_task_self(), 0, &kt)`
  (tested with etasonuntether 1.3.1) and the log
  `/var/logs/untetherhomedepot.log` containing the current boot's kernel base
  (`[*] kernelbase=0x...`). If the file is missing, create it empty as root:
  `install -o root -g wheel -m 600 /dev/null /var/logs/untetherhomedepot.log`.
- Binaries must be signed with `entitlements.plist` (`task_for_pid-allow`,
  `get-task-allow`), otherwise `task_for_pid(0)` fails.
- Root SSH access to the Apple TV.

## Build (macOS)

Prerequisites: Xcode (clang), `ldid` (`brew install ldid`), the iPhoneOS 8.4
SDK and the `ld64-242` linker from Xcode 6.4 (the modern linker emits armv7
binaries that crash on the iOS 8 dyld). Put them in a `toolchain/` directory
containing `iPhoneOS8.4.sdk/` and `bin/ld64-242`, then:

```sh
make TOOLCHAIN=/path/to/toolchain
```

This produces in `build/`: `cec-controller-init`, `cec-send-nohpd`,
`cec-native-probe`, `cec-hdmi-watch`, `cecd`.

> Note: building with `-O0` is on purpose. With clang 21, `-Os` produces a
> `cec-send-nohpd` binary that gets SIGKILLed on launch on the device.

## Installation on the Apple TV

```sh
ATV=root@<appletv-address>
ssh $ATV 'mkdir -p /var/root/cec-runtime /var/root/cecd'
scp build/cec-controller-init build/cec-send-nohpd build/cec-native-probe \
    build/cec-hdmi-watch $ATV:/var/root/cec-runtime/
scp build/cecd $ATV:/var/root/cecd/cecd
scp launchd/activate.sh launchd/disable.sh launchd/restore-hid-plugin.sh \
    $ATV:/var/root/cec-runtime/
scp launchd/local.appletv.cec-runtime.plist launchd/fr.appletvrevival.cecd.plist \
    $ATV:/Library/LaunchDaemons/
ssh $ATV 'chown root:wheel /var/root/cec-runtime/* /var/root/cecd/cecd \
    /Library/LaunchDaemons/*.plist; chmod 755 /var/root/cec-runtime/cec-* \
    /var/root/cec-runtime/*.sh /var/root/cecd/cecd; chmod 644 /Library/LaunchDaemons/*.plist; \
    launchctl load /Library/LaunchDaemons/local.appletv.cec-runtime.plist; \
    launchctl load /Library/LaunchDaemons/fr.appletvrevival.cecd.plist'
```

At boot, `activate.sh` runs `cec-controller-init --activate` (creates the
native CEC event source, attaches it to the workloop, sets the controller's
capability byte, reinitializes HDMI power and HPD), then checks the interface
with `cec-native-probe`, then execs `cec-hdmi-watch`, which monitors the HDMI
connection. Logs: `/var/logs/cec-runtime.log` and `/var/logs/cecd.log`.

## Usage

- `cec-native-probe [poll]` — opens the native CEC interface; with `poll`,
  sends one polling frame (header `0x40`) and prints the result (`0` = ACK,
  `0xe00002ed` = no response).
- `cec-send-nohpd <hex bytes>` — sends **one CEC frame even when HPD is low**
  (TV in standby, `IOAVCECControlInterface` absent) by calling the HDMI
  controller's `sendCEC` routine directly in the kernel, with the link-up flag
  forced for the duration of the send. E.g. `cec-send-nohpd 40 04`
  (`<Image View On>`). Exit code 0 if acknowledged. `cec-send-nohpd --inspect`
  performs a read-only check.
- `cecd` — the CEC daemon (LaunchDaemon `fr.appletvrevival.cecd`). On display
  wake, sends `<Image View On>` + `<Active Source>` to the TV (through
  `cec-send-nohpd` when HPD is low); on display sleep, `<Standby>`; when the TV
  turns off, puts the Apple TV to sleep. Test commands: `cecd post wake|sleep`,
  `cecd tx 40 36`.
- `cec-hdmi-watch` — re-triggers HDMI hot-plug detection every 10 s while the
  CEC service is absent; never transmits a frame.

TV volume is CEC-controllable (`<User Control Pressed>` `0x41`/`0x42`/`0x43`):
`cecd tx 40 44 41` (up), `cecd tx 40 44 42` (down), `cecd tx 40 44 43` (mute).

Disable: `touch /var/root/cec-runtime/disabled` (and/or
`/var/root/cecd/disabled`), `launchctl unload` both plists, or run
`/var/root/cec-runtime/disable.sh` then reboot.

## Full rollback

```sh
ssh root@<atv> 'launchctl unload /Library/LaunchDaemons/local.appletv.cec-runtime.plist \
  /Library/LaunchDaemons/fr.appletvrevival.cecd.plist; \
  rm -f /Library/LaunchDaemons/local.appletv.cec-runtime.plist \
        /Library/LaunchDaemons/fr.appletvrevival.cecd.plist; \
  rm -rf /var/root/cec-runtime /var/root/cecd; reboot'
```

Nothing is permanently modified (no kernel, no boot images), so a plain reboot
restores the device to its original state.

## Known limitations

- Tested with an LG 39LB5700 (2014): it **refuses** `<Standby>` from a device
  (`Feature Abort`) and ignores CEC power keys; putting the Apple TV to sleep
  therefore does not turn off this TV model.
- One CEC client at a time: while `cecd` holds the interface,
  `IOCECInterfaceCreate` returns NULL for other processes.
- `cec-send-nohpd` only works while the HDMI controller object is alive
  (command gate present at offset 0x128); it cannot revive a fully stopped
  controller.

## Contents

```
src/cec-controller-init.c   in-memory CEC activation at boot (tfp0)
src/cec-send-nohpd.c        CEC frame with HPD low (direct sendCEC call)
src/cecd.c                  CEC daemon (wake/sleep/SIMPLINK)
src/cec-native-probe.c      native interface probe (poll)
src/cec-hdmi-watch.c        HPD / CEC service watcher
launchd/                    LaunchDaemon plists + activate.sh / disable.sh
docs/INVESTIGATION.md       investigation log (identifiers anonymized)
```
