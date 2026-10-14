#!/bin/bash
# Movement-feel test harness (WASD). Run: bash $S/movetest.sh   (start with setsid nohup ... from the caller)
# Results: $S/MOVE_RESULT.txt (analysis), $S/MOVE_MARKS.txt (scenario timing marks).
# Env knobs: SKIP_START=1 reuse an already running pair (uses win.env, no log truncation)
#            KEYMODE=window  send keys with `xdotool keydown --window $CW` (XSendEvent) instead of XTEST
#            SETTLE=2.0      quiet seconds after each scenario (inside its analysis window)
#            KEEP=1          do not kill the game at the end
#            POST_WAIT=35    seconds to wait for the quiet-field teleport after pair start
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
export LANG=C.UTF-8 LC_ALL=C.UTF-8
MARKS=$S/MOVE_MARKS.txt
HOSTLOG=$S/ud_host/config/debug.log
SETTLE=${SETTLE:-2.0}
POST_WAIT=${POST_WAIT:-35}
: > $MARKS
echo "# format: Sn start=<epoch ms> end=<epoch ms> nominal=<ms> settle_end=<epoch ms>; t0=end-nominal" >> $MARKS

if [ -z "$SKIP_START" ]; then
  pkill -9 cataclysm-tiles; sleep 3
  rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
  : > $HOSTLOG; : > $S/ud_client/config/debug.log
  CATA_MP_TEST_TRACE=1 CATA_MP_TEST_BUBBLE=quiet NO_STAIRS=1 bash $S/pair.sh > $S/pair.out 2>&1
fi
source $S/win.env
if [ -z "$CW" ] || ! pgrep -x cataclysm-tiles > /dev/null; then
  echo "MOVE ABORT: game/client window not running (CW='$CW')" | tee $S/MOVE_RESULT.txt
  exit 1
fi
# wait for the teleport into the quiet field (~20 s after join; pair.sh already waited 100 s)
sleep $POST_WAIT
shot move_start

W=""
[ "$KEYMODE" = "window" ] && W="--window $CW"
now() { date +%s%3N; }
focus() { xdotool windowfocus $CW; sleep 0.3; xdotool keyup $W w a s d 2>/dev/null; sleep 0.2; }

# scn NAME NOMINAL_MS xdotool-args...  : runs one key chain, records marks
scn() {
  local name=$1 nominal=$2; shift 2
  focus
  local st=$(now)
  xdotool "$@"
  local en=$(now)
  sleep $SETTLE
  local se=$(now)
  echo "$name start=$st end=$en nominal=$nominal settle_end=$se" >> $MARKS
}
# recenter KEYS... : unmarked movement back toward the start point (outside any analysis window)
recenter() { # $1 = hold seconds, rest = keys to hold together
  local hold=$1; shift
  local kd="" ku="" kk
  for kk in "$@"; do kd="$kd keydown $W $kk"; ku="$ku keyup $W $kk"; done
  focus
  xdotool $kd sleep $hold $ku
  sleep 1.5
}

# S1 hold d 1.2 s
scn S1 1200 keydown $W d sleep 1.2 keyup $W d
recenter 1.2 a
# S2 tap d 60 ms
scn S2 60 keydown $W d sleep 0.06 keyup $W d
recenter 0.2 a
# S3 chord w, +25 ms d, hold 1 s, release both
scn S3 1025 keydown $W w sleep 0.025 keydown $W d sleep 1.0 keyup $W d keyup $W w
recenter 1.1 s a
# S4 chord w, +70 ms d
scn S4 1070 keydown $W w sleep 0.07 keydown $W d sleep 1.0 keyup $W d keyup $W w
recenter 1.1 s a
# S5 hold w+d 1 s, release d then w 40 ms later
scn S5 1065 keydown $W w sleep 0.025 keydown $W d sleep 1.0 keyup $W d sleep 0.04 keyup $W w
recenter 1.1 s a
# S6 hold a 0.8 s, add d 0.8 s, release d, a alone 0.8 s
scn S6 2400 keydown $W a sleep 0.8 keydown $W d sleep 0.8 keyup $W d sleep 0.8 keyup $W a
recenter 0.2 d
# S7 hold d 1 s, then release d + press w at once, hold 1 s
scn S7 2000 keydown $W d sleep 1.0 keyup $W d keydown $W w sleep 1.0 keyup $W w
recenter 1.0 a
recenter 1.0 s
# S9 melee sectors: TODO - needs a helper that places a monster adjacent to the player
#    (e.g. a CATA_MP_TEST_MONSTER env hook); then swing in 8 directions and check `trace swing ... hit=`.
echo "S9 SKIPPED (TODO: no monster helper)" >> $MARKS

shot move_end
echo "errors in host log: $(grep -c ' ERROR ' $HOSTLOG)" >> $MARKS
python3 -I $S/movecheck.py > $S/movecheck.out 2>&1
rc=$?
[ -z "$KEEP" ] && pkill -9 cataclysm-tiles
echo "MOVE DONE (analyser exit $rc)" >> $S/MOVE_RESULT.txt
