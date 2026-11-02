#!/bin/sh
set -eu
base=/var/root/cec-runtime
touch "$base/disabled"
launchctl unload /Library/LaunchDaemons/local.appletv.cec-runtime.plist 2>/dev/null || true
"$base/restore-hid-plugin.sh"
echo 'CEC autostart disabled. Reboot to return the controller to its original state.'
