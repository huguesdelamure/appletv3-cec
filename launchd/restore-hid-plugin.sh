#!/bin/sh
set -eu
original=/System/Library/Extensions/IOCECFamily.kext/PlugIns/CoreRCHIDService.plugin
disabled=/System/Library/Extensions/IOCECFamily.kext/PlugIns/CoreRCHIDService.plugin.cec-disabled
if [ -d "$disabled" ] && [ ! -e "$original" ]; then mv "$disabled" "$original"; fi
[ -d "$original" ] && [ ! -e "$disabled" ]
sync
echo CEC_HID_PLUGIN_RESTORED
