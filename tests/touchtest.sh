#!/bin/sh
# Exercise every touch gesture against herdr.
# Liveness is checked against /proc/<pid> directly: busybox pgrep/ps on this
# device do not behave like the GNU ones and gave false negatives.
cd /mnt/us/extensions/kterm || exit 1
export DISPLAY=:0 TERM=xterm TERMINFO=/mnt/us/extensions/kterm/vte/terminfo
export ENV=/var/local/kssh/profile HOME=/var/local/kssh
export PATH=/var/local/kssh/bin:$PATH

# clear out anything left from earlier runs, by pid so we never match ourselves
for p in /proc/[0-9]*; do
    c=$(cat "$p/comm" 2>/dev/null)
    if [ "$c" = "kterm" ] || [ "$c" = "ktsh" ]; then kill "$(basename "$p")" 2>/dev/null; fi
done
sleep 2

rm -f /tmp/touch.log
setsid ./bin/kterm -k 0 -l layouts/keyboard-300dpi.xml -e 'ssh -t trashcan herdr' \
    > /tmp/touch.log 2>&1 < /dev/null &
sleep 14

KPID=""
for p in /proc/[0-9]*; do
    [ "$(cat "$p/comm" 2>/dev/null)" = "kterm" ] && KPID=$(basename "$p")
done
if [ -z "$KPID" ]; then echo "kterm never started"; tail -5 /tmp/touch.log; exit 1; fi
echo "kterm pid $KPID"

check() {
    if [ -d "/proc/$KPID" ]; then echo "  $1: alive"; else echo "  $1: *** CRASHED ***"; exit 1; fi
}
check "after launch"

/tmp/xtouch tap 600 700;               sleep 2; check "tap centre"
/tmp/xtouch tap 120 300;               sleep 2; check "tap sidebar"
/tmp/xtouch tap 120 300;               sleep 2; check "tap sidebar again"
/tmp/xtouch drag 600 900 600 400;      sleep 2; check "drag up"
/tmp/xtouch drag 600 400 600 900;      sleep 2; check "drag down"
/tmp/xtouch hold 600 700 900;          sleep 2; check "hold"
/tmp/xtouch hdrag 400 600 800 600 900; sleep 3; check "hold then drag"
i=0; while [ $i -lt 8 ]; do /tmp/xtouch tap 500 600; i=$((i+1)); done
sleep 2; check "8 rapid taps"
/tmp/xtouch drag 600 900 600 200;      sleep 2
/tmp/xtouch drag 600 200 600 900;      sleep 2; check "long drags"

dd if=/dev/fb0 of=/tmp/fbtouch.raw bs=1248 count=1648 2>/dev/null
echo "ALL GESTURES SURVIVED"
kill "$KPID" 2>/dev/null
