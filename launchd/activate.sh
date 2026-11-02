#!/bin/sh
base=/var/root/cec-runtime
if [ -e "$base/disabled" ]; then echo CEC_AUTOSTART_DISABLED; exit 0; fi
if [ -e "$base/in-progress" ]; then
    touch "$base/disabled"
    echo CEC_PREVIOUS_START_INCOMPLETE_AUTOSTART_DISABLED
    exit 1
fi
touch "$base/in-progress"
date
attempt=0
while [ "$attempt" -lt 45 ]; do
    "$base/cec-controller-init" --activate
    result=$?
    if [ "$result" -eq 0 ]; then
        "$base/cec-native-probe"
        result=$?
        if [ "$result" -eq 0 ]; then
            rm -f "$base/in-progress"
            echo CEC_AUTO_ACTIVATION_OK
            exec "$base/cec-hdmi-watch"
        fi
        if [ "$result" -eq 4 ]; then
            rm -f "$base/in-progress"
            echo CEC_CONTROLLER_READY_WAITING_FOR_HDMI
            exec "$base/cec-hdmi-watch"
        fi
    fi
    case "$result" in
        4|5|6|17) attempt=$((attempt + 1)); sleep 2 ;;
        *) break ;;
    esac
done
touch "$base/disabled"
rm -f "$base/in-progress"
echo "CEC_AUTO_ACTIVATION_FAILED result=$result; disabled until manual inspection"
exit 1
