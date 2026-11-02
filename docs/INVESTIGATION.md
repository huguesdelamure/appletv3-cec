# CEC investigation log

> **Status.** Apple TV connected to a CEC TV after a power cut, CEC enabled on the normal boot. After turning the TV on and re-running HDMI detection, the IOHDMI and CEC services were created. **Two direct playback-1 (4) to TV (0) pollings were acknowledged (result 0 in 41 then 46 ms)**, confirming real CEC exchange on this device and this cabling. A light watcher re-triggers HPD every ten seconds while the CEC service is absent.

## Device observed over SSH

- AppleTV3,2 / J33iAP, SoC S5L8947X (2013 Apple TV 3).
- Darwin 14.0.0, build 12H1006.
- IOKit registry read by a small armv7 program built with the local SDK; no register writes, no configuration changes.

## Results

The active service `AppleSamsungHDMITXControllerS5L8947X` declares a `CEC_BASE` register set, memory index 6, 98 32-bit registers. Its `hdmi` provider declares at that index a 4096-byte MMIO range at address 0x39770000. See `registry.txt`.

The kernelcache copied from the device and decompressed contains in particular:

- `hdmi_cec_enabled`
- `handleCECInterrupt`
- `CEC TX_STATUS_0=... RX_STATUS_0=...`
- CEC transmission and reception error messages
- `sendCEC`, `receiveCEC`, `setCECEnabled()`
- `IOAVCECService`, `IOCECService`, `IOCECUserClient`

These strings establish the presence of CEC code in this kernelcache; they do not prove that every path runs on this model. The CEC block declared by the active driver is an additional hint specific to this device.

No CEC-class service is instantiated in the IOService snapshot. The HDMI controller reports `Power=false` and an HPDState=0 event: no active HDMI link is observed in this snapshot. Do not confuse this with proof of absent CEC wiring.

The system also contains the IOCEC.framework and IOCECFamily.kext directories and CoreRC CEC plugins. IOCEC.framework has no standalone binary at the usual path; the code may be in the dyld cache.

## Conclusion

The thesis "hardware entirely devoid of CEC capability, therefore no software possible" is not supported and the collected evidence argues against this simplification. There is a declared CEC block and CEC management code in this device's firmware.

Effective CEC communication is not demonstrated: the activation mechanism still needs to be identified (`hdmi_cec_enabled` is a lead, not yet a validated parameter), the path to the connector verified, and exchanges observed with a CEC TV. No CEC command was sent, no NVRAM change or reboot performed.

## Files

- `registry.c`, `registry`: IOKit read probe and armv7 executable.
- `registry.txt`: full snapshot (may contain device identifiers; keep local).
- `kernelcache`, `kernel.dec`: local copy and decompressed version.
- `unpack.py`: LZSS decompression.
- `kernel-cec-strings.txt`: relevant strings.

The probe was dropped at `/tmp/cec-registry` on the Apple TV.

## Follow-up analysis — boot-time activation

Disassembling the driver linked `hdmi_cec_enabled` to a call to `PE_parse_boot_argn`:

- loading the string at 0x80a35398;
- zero-initializing the boolean field at object offset 0x2b4;
- calling 0x80a353ca through stub 0x80a3ea8c;
- the stub points to 0x803361dd, i.e. `_PE_parse_boot_argn` (symbol 0x803361dc, Thumb bit);
- if the argument is present, a non-zero value enables this field;
- the getter at 0x80a397bc returns this field; a CEC operation at 0x80a397c4 checks this getter before continuing.

The IOCEC framework is loadable from the dyld cache. Its exported function `IOCECInterfaceCreate(NULL)` returns NULL before activation: no usable interface confirmed at the userland level.

The jailbreak and SSH were confirmed to come back without USB after a reboot. The initial arguments were saved in `boot-args-original.txt` and on the device in `/var/root/cec-boot-args-original.txt`. The NVRAM received `hdmi_cec_enabled=1`, then a reboot was launched. The SSH port came back after about two minutes. The test result was to be filled in. A `restore-boot-args.sh` script restores the initial arguments without rebooting.

### Result of the reboot test

- The home screen was confirmed displayed on the TV after the reboot.
- The NVRAM did contain `hdmi_cec_enabled=1`, but `sysctl kern.bootargs` still returned an empty string. This attempt therefore did not enable the driver flag: it does not test the electrical CEC capability.
- The original NVRAM arguments were restored immediately after this check. The running kernel did not receive the experimental flag.
- The new snapshot `registry-after-reboot.txt` shows EDID information and IOAV video/audio interfaces; the HDMI link is now detected. No CEC service appears.
- `cec-probe-after-reboot.txt` confirms that `IOCECInterfaceCreate(NULL)` still returns NULL.
- A permissions probe (`kernel-access.c`) tested only obtaining the kernel task port: `task_for_pid(0)` returns KERN_FAILURE (5), null port. The jailbreak therefore does not provide this access through this ordinary call. This does not prove the absence of any other kernel access path.
- No hardware register was written and no CEC frame was transmitted.

### Next technical step

The simple "NVRAM boot-args then reboot" path is not enough on this boot. One must understand how the blackb0x boot chain transmits or replaces the kernel arguments, or develop an in-memory kernel modification that exposes the feature. A simple user preference is not enough for the path identified in the driver: the boolean is initialized from the kernel arguments. Do not modify boot images before identifying the chain and planning a verified recovery.

The `cec-probe.c` tool loads the native API and tests interface creation. Its query option was NOT executed and the transmission ABI still needs full validation before use.

### Identified jailbreak chain

The `net.tihmstar.etasonuntether` 1.3.1 package is installed, in addition to the blackb0x bootstrap. `/--early-boot` points to `/untether/expl.js`, with `/untether/untether.bin`; `/etc/rc.d/daemonload` uses `orphan_commander` to load daemons. One must therefore not attribute argument filtering to blackb0x itself without proof. The next step must examine iBoot and this untether chain to determine where to actually inject the argument. No boot component was modified.

Final check: `kern.bootargs` empty, NVRAM `boot-args` restored to the initial empty value, AppleTV process active. The shared investigation lock was released.


## Next investigation — kernel access and USB preparation

The initial refusal of `task_for_pid(0)` depended on the probe's signature. With debugging entitlements, the call does return a port; the special host port 4 remains null. The earlier limited conclusion about this call must therefore be read with this correction.

Two experimental `vm_read_overwrite` reads caused two panics: at 0x81101000, then at 0x80a397bc. The first log gives kernel base 0xa0001000; the second 0x85201000. Addresses move from one boot to the next. The declared VM map may mark a range readable without guaranteeing its actual accessibility to this mechanism; the region validation added after the first panic was not enough. No write access was attempted. Both panics are attributed to `cec-kmem` in their logs. The device rebooted automatically and it was confirmed working. The remote probe was deleted; its local binary was renamed and stripped of its execute bit. No further kernel read attempts without a newly validated mechanism.

The blackb0x project's primary sources explain why the arguments must be integrated into iBEC. Version examined: https://github.com/regulad/Blackb0x--/blob/dd0c86b44f33d1aae2193d098a091c71934aebd9/src/Patcher.hpp . USB was available but not used immediately.

An experimental iBEC for AppleTV3,2 / 12H1006, with `hdmi_cec_enabled=1`, was prepared offline from the corresponding Apple firmware and verified at the format, arguments and code-selection-pointer level. See `TEST-USB.md` and `boot-test/manifest.json`. It was neither sent nor booted. The tools and other USB suite components remain to be prepared/validated before the test.

Final state confirmed over SSH in `final-state.txt`: NVRAM boot-args empty, kern.bootargs empty, AppleTV active, kernel-memory probe removed. No permanent system component of the device was modified by this investigation and no CEC frame was transmitted.


## First USB boot with CEC — actual result

After confirmed physical DFU mode, device ECID <ECID> / CPID 0x8947 / BDID 0 was identified. Checkm8 succeeded; iBSS, iBEC CEC, logo, DeviceTree and kernel were sent through blackb0x's unchanged DeviceManager methods. No ramdisk or installer sent. The device accepted bootx and left Recovery. See `usb-boot-attempt-1.log`.

SSH came back about two minutes later. `usb-boot-runtime.txt` confirms that the running kernel actually received `hdmi_cec_enabled=1`.

Proof of activation, in `registry-usb-cec.txt`:

- IOAVCECControlInterface instantiated, IOAVCECControlInterfaceUserClient exposed.
- IOAVCECService instantiated, IOCECUserClient exposed, HIDServiceSupport=true.
- CEC physical address 1.0.0.0 provided by the HDMI test dongle's EDID.
- `IOCECInterfaceCreate(NULL)` returns a non-null interface, confirmed in `cec-probe-usb.txt`.

Hardware limit still open: no CEC exchange sent or received. HDMI is only connected to the HDMI test dongle, not to a CEC TV.

Functional failure of this boot: persistent black screen, blinking LED (three quick blinks), then loss of SSH and USB lockdownd. The captured HDMI registry shows repeated ProtectLink failures (general error and I/O timeout); this suggests an HDMI protection difficulty, without establishing the cause of the blinking or the loss of access. Successful CEC activation does not equal a stable system or a usable CEC implementation.

The reboot attempt via USB diagnostics failed (lockdownd -8). A 10-second mains cut was requested to get back to the original boot. No permanent boot component was modified. Normal return still to be verified at the time of this entry.

A future attempt must compare the same USB chain without the CEC flag before attributing the anomalies to CEC. Do not repeat transmission attempts in this unstable state.

### Confirmed normal return and USB blockage log

The power had not been cut yet. The Apple TV therefore came back automatically: home screen visible in `normal-recovery-screen.jpg`, system accessible via USB/SSH, kern.bootargs empty in `normal-recovery-runtime.txt`.

The `panic-after-usb-boot.txt` log confirms `Debugger message: WDT timeout`: a watchdog rebooted the system during the test. The corresponding stack addresses notably belong to AppleARMPlatform and AppleInterruptController; they do not by themselves designate a CEC fault. This blockage must be distinguished from the ProtectLink failure observed in the HDMI registry; a causal link is not established.

An NVRAM boot-args value `hdmi_cec_enabled=1` had reappeared after the reboot although kern.bootargs was empty. It was reset to empty, with sync and verification. The shared cec-usb lock was released.

A control suite `boot-suite-baseline/` is prepared offline: same iBSS, kernel, DeviceTree and logo, NAND-root iBEC without `hdmi_cec_enabled=1`. No second USB boot was launched. Comparing against this control is necessary before attributing the instability to the CEC flag. Electrical validation on a TV remains later.

## Preparation of the control test without CEC

The local transport now exposes explicit `--validate-baseline` and `--boot-baseline` modes. The pre-check requires the absence of any `hdmi_cec_enabled` argument in control mode and its presence in CEC mode. All four mode/suite combinations were verified: each of the two suites passes only in its own mode. Fingerprints are verified; iBSS, KernelCache, DeviceTree and RestoreLogo are identical between the two suites.

Three control-launch attempts failed BEFORE sending images:

1. `usb-boot-baseline-1.log`: checkm8 stopped right after bug setup, without reaching Grooming heap. Attempt interrupted.
2. After accepted USB reset, `usb-boot-baseline-2.log`: heap hole creation error, device disappeared then came back in normal mode. Automatic retries interrupted.
3. After a fresh physical DFU sequence, `usb-boot-baseline-3.log`: again blocked after bug setup. Attempt interrupted.

These attempts are NOT a CEC/non-CEC comparison result: no control kernel was booted. A full mains + micro-USB cut of 10 seconds was requested before the next attempt. Do not attribute these transport errors to CEC, whose images were not loaded.

## Control test executed — CEC/non-CEC comparison

After a full mains + micro-USB cut then a fresh DFU, checkm8 succeeded via the standalone binary (same default parameters). The transport then found PWND:[checkm8] and sent the control suite without re-running the exploit. Logs: `checkm8-baseline-cold.log`, `usb-boot-baseline-cold.log`.

`registry-usb-baseline.txt` confirms:

- experimental kernel arguments present (`rd=disk0s1s1`, etc.), but no `hdmi_cec_enabled`; the system had therefore not automatically returned to the usual boot;
- no CEC service instantiated; `cec-probe-baseline.txt` returns NULL;
- same ProtectLink / HDCP errors as in the CEC attempt: these errors are not specific to CEC and are not enough to explain the first attempt's black screen.

Observed for more than 339 seconds after the handoff log: image visible, SSH responding, no new panic. Right then left navigation in ModernLauncher responds (captures `baseline-navigation-right.jpg` and `baseline-final-runtime.txt`). The video `baseline-observation.mp4` keeps 15 seconds of display; `baseline-observation-contact.jpg` shows four frames. Full app content was not tested; this observed stability concerns boot, display, launcher navigation and SSH.

Comparison:

| Measurement | CEC flag active | Control without flag |
|---|---|---|
| Kernel actually booted with the experimental arguments | Yes | Yes |
| CEC services instantiated | Yes | No |
| Native IOCEC interface created | Yes | NULL |
| Observed display after boot | Persistent black | Launcher visible and navigation responsive |
| Evolution observed | Loss of access and WDT timeout | More than five minutes without a new panic |
| ProtectLink/HDCP errors | Yes | Yes |

The iBSS/kernel/DeviceTree/logo components are identical; the control iBEC reuses the same patch options and the same NAND-root arguments without the CEC flag. This result points more at CEC activation or its interaction with HDMI than at the USB chain in general. This is a single comparison, with different reset/transport conditions between sessions: it is not definitive proof of causality nor an electrical CEC validation.

No CEC frame was sent. The next work is to isolate the CEC service/controller initialization before attempting a new test on a TV.

After collection, the cec-baseline lock was released, NVRAM boot-args reset to empty and a normal reboot requested over SSH. Normal return to be confirmed in the following checks.

### Output of the control test

After requesting a normal reboot, the device showed up in Recovery (iTunes screen + cable, capture `after-baseline-normal-screen.jpg`). Its ECID and model were re-verified with irecovery. The command `irecovery -i <ECID> -n` completed successfully to leave Recovery and reboot the installed system. No restore/erase command was executed. Waiting for normal return.

### Final state after the control test

Return to the usual boot verified: home screen visible on the HDMI test dongle (`after-baseline-recovery-exit-screen.jpg`), SSH accessible, `kern.bootargs` empty, NVRAM boot-args empty, auto-boot=true, AppleTV process active (`after-baseline-restored-runtime.txt`). No permanent boot component replaced. The experimental control is no longer active.

## Isolation of the activation and the CEC HID bridge

### Kernel variant, temporary only

Suite `boot-suite-activation-stub/`, with the same CEC iBEC as the first attempt. On the kernel already prepared by CBPatcher, four bytes were replaced at the entry of the candidate CEC activation routine (VA 0x80a397c4; Samsung vtable +0x3dc). The prologue `b0 b5 02 af` becomes `00 20 70 47` (`movs r0,#0; bx lr`). This routine therefore returns success without executing its body or its command through the IOCommandGate. This does NOT disable all hardware initialization paths.

The kernelcache was recompressed/repackaged with the xpwn API already used by blackb0x. Independent decompression recovers exactly the modified raw file; length, Adler32 and IMG3 structure verified. See `experiment.json`, `validation.json`, `repack-activation-stub.log` and `activation-stub-disassembly.txt`. No kernel byte was written in memory on the device and no permanent kernelcache was replaced: the modified image was sent over USB for that single boot.

### Initial result

Standalone checkm8 succeeded; boot of the variant confirmed by the CEC arguments. The IOAVCECControlInterface and IOAVCECService services are present, SSH works, but the screen stays black. This single neutralization therefore does not restore the display.

`AppleTV` remains at the start of its initialization, before the launcher, waiting for BackBoardServices. `backboardd` is restarted regularly after user watchdog (bug_type 188 reports, notably `activation-stub-backboard-watchdog.ips`). During this variant, the kernel stayed accessible for more than six minutes without an observed kernel panic; this is not enough to establish that a possible system watchdog would be permanently eliminated.

Userland process probes `task-snapshot.c`: task_for_pid only for PID > 1, thread states and user stack frame reads, without suspension or writes. backboardd's main thread waits in `IOHIDSessionOpen` / `IOHIDEventSystemOpen` (live resolution by dladdr in `activation-stub-symbols.txt`). Another thread waits in CoreRC, after loading `CoreRCHIDService.plugin`. See the saved stacks.

### HID experiment, in the same boot

The attempt to set HIDServiceSupport=false via IORegistryEntrySetCFProperty returns unsupported (0xe00002c7), without changing the value.

The directory `/System/Library/Extensions/IOCECFamily.kext/PlugIns/CoreRCHIDService.plugin` was then temporarily renamed to `.plugin.cec-disabled`, without changing its content; backboardd was restarted. A restore command was recorded in `/var/root/cec-restore-hid-bridge.sh`.

The home screen then appeared and the launcher finished loading. Checks `hid-disabled-runtime.txt`: same kernel with hdmi_cec_enabled=1, same boot, plugin temporarily absent from the normal path, AppleTV and backboardd active, no kernel panic. `hid-disabled-cec-and-registry.txt` confirms that the CEC services remain instantiated and that the native API creates a non-null interface.

Inverse control: plugin put back in its normal place then a new backboardd restart, without a new USB boot. The screen went black again; the IOHIDEventSystemOpen / IOHIDSessionOpen and CoreRC waits reappear. Captures and stacks `hid-bridge-restored-control-*` / `hid-restored-control-backboard-*`.

Limited but reproducible conclusion in this variant: the HID/CoreRC bridge tied to CEC blocks interface startup; temporarily removing it frees this startup. This does NOT yet prove that removing this plugin is enough with the original hardware routine. The kernel used here still contains the four-byte neutralization, and no CEC frame was sent or received. The electrical hardware remains to be validated.

### Cleanup started

The plugin was permanently put back in its original place to exit the experiment; absence of the `.cec-disabled` directory verified, NVRAM boot-args empty, auto-boot=true, cec-hid-isolation lock released. Normal reboot launched; return checks to follow. See `hid-isolation-cleanup-before-reboot.txt`.

Next useful attempt: kernel without the hardware-activation neutralization, CEC HID bridge temporarily excluded, then native interface creation and ABI validation. Only then will an electrical test on a CEC TV be interpretable.

### End of session and preparation of the next suite

Normal return confirmed in `after-hid-isolation-normal-runtime.txt`: kern.bootargs empty, NVRAM boot-args empty, CoreRCHIDService plugin at its original location, no `.cec-disabled` directory, AppleTV active. The home screen is visible on the HDMI test dongle. No experimental kernel is installed permanently.

A new suite is prepared offline in `boot-suite-cec-no-hid/`, NOT booted: it starts from the reference CBPatcher kernel without the four-byte neutralization and only changes the IOCECFamily merge personality's HIDServiceSupport property to false. The activation routine's code bytes are original. The insertion of one XML byte was absorbed into the __PRELINK_INFO padding, the two affected section/segment sizes were adjusted, the total Mach-O size is unchanged. XML and ID/IDREF references parsed and verified, boolean false confirmed in the CEC personality. LZSS/Adler32 recompression and exact equality with the prepared raw verified. This suite should exclude the HID bridge without renaming the plugin on the system, but its real behavior remains to be tested.


## Native CEC variant without HID bridge — attempt performed

Suite `boot-suite-cec-no-hid` verified by fingerprints, IMG3 formats, LZSS/Adler32 decompression and equality with the prepared raw. Activation routine code is original: no four-byte stub. Only the CEC merge metadata HIDServiceSupport is false. The CoreRCHIDService plugin still exists at its original location on the system.

Standalone checkm8 then USB transport succeeded (`checkm8-cec-no-hid.log`, `usb-boot-cec-no-hid.log`). `registry-usb-cec-no-hid.txt` confirms the CEC arguments, the IOAVCECControlInterface / IOAVCECService services, and HIDServiceSupport=false. The home screen appeared; right/left navigation of the launcher responds (`cec-no-hid-navigation.jpg` and logs).

### Native API and first transmission attempt

`cec-native-probe.c` uses the original IOCEC API, without CoreRC and without reimplemented CEC transmission code. The 17-byte structure ABI was cross-checked with the API and the IOCECUserClient table. The last argument of SendFrame is an 8-bit field accepted from 0 to 5 by the driver, written into bits 4..6 of the TX control: it is not a millisecond delay. The old query option of cec-probe.c was never executed; do not use it. The new probe uses the value 0.

- Native interface open/close succeeded (`cec-no-hid-open-interface.txt`).
- Playback-1 logical address mask (address 4) accepted.
- A single CEC polling request, header 0x40 (source 4 to TV address 0), no opcode, was passed to the API.
- Return after 0.041 s: 0xe00002ed, "device not responding"; no external acknowledgement confirmed.
- Mask released to zero, interface closed. See `cec-no-hid-poll.txt`.

The result is compatible with the absence of a CEC responder on the HDMI test dongle link, but does not demonstrate that a correct waveform reaches the HDMI pin. It is not an electrical validation.

### Stability and practical limit

At the final check `cec-no-hid-final-runtime.txt`, same experimental kernel, same AppleTV PID 117 and backboardd PID 125 processes, no new panic, display and navigation functional after the polling, more than six minutes since boot. The cec-native-test lock was released. No plugin file was moved and no permanent kernel was replaced.

Moving the setup to the TV requires a power cut, so the temporary state cannot be moved while keeping it. The real acknowledgement test remains to be organized: USB boot near the TV, or a separate development of an activation after the usual boot. The functional experimental state was left active for local diagnostics; it is not made persistent.

### On Apple's historical intent

The presence of the code, the flag and the services demonstrates a disabled CEC infrastructure, not a documented commercial intent. An abandoned experimental feature or shared code are hypotheses, not established conclusions. The claim of absolute hardware impossibility is still not demonstrated; the wiring and exchanges with a TV remain to be verified.


## Re-launchable activation after a power cut

The file `/var/logs/untetherhomedepot.log` was missing. Analysis of the existing payload shows an open for write/append without creation, followed by a stdout redirection. Creating this root 0600 file allowed the unchanged jailbreak to log its kernel base at normal boot: `0x9b801000`, slide `0x1b800000`. Reads at the derived addresses confirmed the ARM Mach-O header, the CEC getter and the three vtable entries. No new panic was observed.

The first attempt, only redirecting the capability getters to return-true then forcing HPD, exposed the API but blocked sending. These three modifications were restored. Notably missing was the receive object at controller offset `0x148`, created only if CEC is active at boot. Service restart via IOCatalogueTerminate is refused (bad argument) on this kernel.

The final tool `cec-controller-init.c` is limited to AppleTV3,2 / xnu-2784.40.6~93. It checks the current boot's log, the expected bytes and vtables. It finds its own IOAV connection and its controller; a temporary private vtable of this connection allows calling only the native routines needed for initialization. The original table is restored and the temporary page freed. It creates the IOAVAsyncEventSource with the native receive callback `0x80a367ed`, calls IOWorkLoop::addEventSource, fills field `0x148`, enables byte `0x2b4`, then reinitializes HDMI power and HPD through the IOAV APIs. No executable byte or boot image is modified. Addresses are adjusted to the current boot's slide.

Full manual validation: `runtime-initialized-poll.txt`, `cec-controller-init-first.txt`, `runtime-initialized-home.jpg`. Idempotent re-execution: `cec-controller-init-idempotent.txt`. Install and script test: `persistent-install-test.txt`.

Installation: `/Library/LaunchDaemons/local.appletv.cec-runtime.plist`, RunAtLoad without KeepAlive; files under `/var/root/cec-runtime`. The script creates an in-progress marker; if it survives into the next boot, activation disables itself instead of looping. A definitive error also disables the next attempts. The script transmits no frame at boot: it opens and closes the native interface to verify availability.

Rollback: `/var/root/cec-runtime/disable.sh`, then a normal reboot. This disables the launch, restores the HID plugin and lets the controller return to its original configuration at reboot. The jailbreak and its files were not modified.


### Result of the autonomous reboot

The LaunchDaemon ran the activation without USB intervention. New kernel base `0x8d801000` (instead of `0x9b801000`): the new boot's addresses are correctly found. Reception source created and attached, HDMI power reinitialized, native interface opened then closed; `CEC_AUTO_ACTIVATION_OK`. The launchd job finished with status 0. No disabled/in-progress marker remains.

A polling after this reboot again returns `0xe00002ed` in 41 ms, expected on the HDMI test dongle without a CEC receiver. The home screen is visible in `persistent-autostart-1-screen.jpg`, backboardd is running, bootargs stays empty. Files: `persistent-autostart-1-status.txt`, `persistent-autostart-1-poll.txt`, `registry-usb-persistent-autostart-1.txt`.

The final tool adds protections on the error paths (keep the temporary table if its restoration cannot be verified, reject an allocation result that is an error code). Its inspect mode is now read-only; an activation already done returns immediately without new temporary access or HDMI cycle. Both modes were revalidated: `persistent-final-helper-status.txt`.

**Remaining limits:** normal software reboot is tested; a full mains cut has not yet been tested with this installation. No CEC TV is connected during these checks. Electrical acknowledgement, reception of TV messages and TV control functions remain to be validated after moving. The automatic HID bridge remains disabled to preserve the display; native CEC opening is separate from TV-remote navigation.


## Move to the TV and power cut

After moving, untether log mtime January 1970, then system clock synchronized to 2026. The mtime>=kern.boottime test therefore rejected a log that was actually new. Three successive sessions are present in the log: bases 9b801000, 8d801000, then 93201000. The CEC controller had remained inactive (byte0/sourceNULL).

Fix: `kern.bootsessionuuid` provides a stable boot identity despite clock changes. The root 0600 file `/var/root/cec-runtime/boot-log-state` keeps the UUID and a counter of "executing main" markers. A new identity requires a counter strictly greater than the last accepted one; for the same identity, the log must not have regressed. The last base must follow the last marker. The state is recorded atomically after a successful activation. Initial migration anchored on the two previous sessions already verified; the third current session was accepted then verified in memory. No address check was removed.

Corrected helper deployed, launch re-triggered on this session: new CEC source `0xae35fe70`, attached to the workloop; capability byte active; original private table restored; HDMI reinitialization without error. However the controller reports HPD=0, and no IOHDMIDevice is created. IOCECInterfaceCreate returns NULL; no polling was therefore sent to the TV. See `tv-cold-boot-inspect.txt`, `tv-cec-first-poll.txt`, `registry-tv-after-init.txt`.

The boot script now treats this case as "controller ready, waiting for HDMI" and exits with status 0, without disabling the next boot. The absence of an interface linked to HPD=0 is not a controller initialization error. The next cold boot with this new check remains to be verified. No additional reboot was imposed while waiting for confirmation of the TV image.


## First real acknowledgement from the TV

The TV was turned on; no image appeared. The initial snapshot still showed HPD=0. An IOAVControllerForceHotPlugDetect call made IOHDMIDevice, IOHDMIService, IOAVCECControlInterface and IOAVCECService appear; the native API became available.

A CEC polling with header 0x40 (playback-1 to TV address 0), length 1, TX field 0, returned **0x00000000 successful in 41 ms**. The logical mask was released and the interface closed. Unlike the HDMI test dongle without a destination (NotResponding), this direct frame gets an acknowledgement. File `tv-on-real-poll-and-registry.txt`.

Added `cec-hdmi-watch.c`, executable `/var/root/cec-runtime/cec-hdmi-watch`, launched by exec after activation. It only checks the existence of the IOAVCECControlInterface service (without opening a CEC connection); if absent, it calls the HDMI detection API, then waits ten seconds. It transmits no CEC frame. The launchd job now stays active to watch for a late connection / TV power-on. Disabling via disabled/unload remains available. Log: HDMI_CEC_INTERFACE_AVAILABLE. Second polling acknowledged in 46 ms with the watcher active, `tv-cec-watch-validation.txt`.

Confirmation of the TV image is requested separately: a CEC acknowledgement does not guarantee the video display. No standby, power-on, source-selection or volume command was sent during these tests.


## Waking the TV with HPD low — cec-send-nohpd

The TV in standby cuts HPD: `IOHDMIDevice` and `IOAVCECControlInterface` disappear, `IOCECInterfaceCreate` returns NULL, and there is no userland path to transmit — yet the CEC line stays powered and a standby TV still listens, which is exactly what `<Image View On>` is for. `sendCEC` (kernel 0x80a39978, Thumb) refuses to send while the controller's link-up flag (offset 0x165) is 0.

`cec-send-nohpd.c` reuses the private-vtable technique of `cec-controller-init.c`: it forces the link-up flag for the duration of the call, builds the 18-byte frame (`data[16], length, TX field`) in a kernel page, and calls `sendCEC(controller, 0, frame)` through `IOConnectTrap6`, then restores the flag and the connection's client vtable and frees the page. Exit 0 if acknowledged; `--inspect` is a read-only precondition check. Note: build with `-O0` — with clang 21, `-Os` produces a binary that is SIGKILLed on launch on the device.

`cecd` calls it on display wake when no CEC interface exists: `cec-send-nohpd <la> 04` (`<Image View On>`) then, 3 s later, `cec-send-nohpd <la>f 82 10 00` (`<Active Source>`).

Observed on device (TV off, Apple TV asleep, `link-up=0`): `cecd post wake` -> both nohpd frames sent (sendCEC returned 0xe00002c9, an error code, but the frames went out) -> 4 s later HPD rose and the CEC interface appeared (`CEC ready: logical 4, physical 1.0.0.0`) -> cecd sent `40:04` and `4f:82:10:00` through the interface, both acknowledged (`0x00000000`) -> the TV turned on and displayed the Apple TV (confirmed on screen). The wake path therefore works end to end.
