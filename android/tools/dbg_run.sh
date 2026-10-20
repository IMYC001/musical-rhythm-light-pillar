#!/usr/bin/env bash
# 依次跑若干诊断模式，各截一张图。
# 用法: dbg_run.sh <mode> [<mode> ...]
set -u
source "$(dirname "${BASH_SOURCE[0]}")/_common.sh"

"$A" connect $DEV >/dev/null 2>&1

for m in "$@"; do
  printf '[android]\naudioSourceIndex=0\ndebugMode=%s\n' "$m" > "$D/shots/dbg_$m.ini"
  MSYS_NO_PATHCONV=1 "$A" -s $DEV push "$D/shots/dbg_$m.ini" /data/local/tmp/dbg.ini >/dev/null 2>&1
  MSYS_NO_PATHCONV=1 "$A" -s $DEV shell "run-as com.lightpillar cp /data/local/tmp/dbg.ini files/settings.ini" >/dev/null 2>&1
  "$A" -s $DEV shell am force-stop com.lightpillar
  sleep 1
  "$A" -s $DEV shell am start -n com.lightpillar/.MainActivity >/dev/null
  sleep 3
  "$A" -s $DEV shell input tap 240 140
  sleep 4
  "$A" -s $DEV shell input keyevent KEYCODE_HOME
  sleep 3
  "$A" -s $DEV exec-out screencap -p > "$D/shots/mode_$m.png"
  echo "mode=$m -> shots/mode_$m.png  ($(stat -c%s "$D/shots/mode_$m.png") bytes)"
done
