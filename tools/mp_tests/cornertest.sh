#!/bin/bash
# Corner-slide test.  Run: setsid nohup bash $S/cornertest.sh > $S/corner.out 2>&1 &
# Starts host + client (pair.sh), waits for the host hook CATA_MP_TEST_CORNER to build the wall
# row north of the client's body (after the quiet-field teleport), holds w+d (north-east) for
# HOLD seconds and checks the host trace: the blocked diagonal must slide east
# (`trace slide ... wanted=1,-1 took=1,0`), never `trace blocked ... dx=1 dy=-1`.
# Result: $S/CORNER_RESULT.txt (PASS / FAIL + evidence lines).
# Env knobs: SKIP_START=1 reuse a running pair (win.env, no log truncation; the ready line must
#            already be in the log), HOLD=1.5 seconds to hold, KEEP=1 do not kill the game.
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
export LANG=C.UTF-8 LC_ALL=C.UTF-8
R=$S/CORNER_RESULT.txt; : > $R
HOSTLOG=$S/ud_host/config/debug.log
HOLD=${HOLD:-1.5}

if [ -z "$SKIP_START" ]; then
  pkill -9 cataclysm-tiles; sleep 3
  rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
  : > $HOSTLOG; : > $S/ud_client/config/debug.log
  CATA_MP_TEST_TRACE=1 CATA_MP_TEST_BUBBLE=quiet CATA_MP_TEST_CORNER=1 NO_STAIRS=1 bash $S/pair.sh > $S/pair.out 2>&1
fi
source $S/win.env
if [ -z "$CW" ] || ! pgrep -x cataclysm-tiles > /dev/null; then
  echo "FAIL: game/client window not running (CW='$CW')" | tee -a $R
  exit 1
fi

# the hook fires ~30 s after the join (and after the teleport); pair.sh has mostly waited that out
READY=""
for i in $(seq 1 120); do
  READY=$(grep -n "Multiplayer: test corner ready" $HOSTLOG | tail -1)
  [ -n "$READY" ] && break
  grep -q "test corner failed" $HOSTLOG && break
  sleep 2
done
if [ -z "$READY" ]; then
  echo "FAIL: no 'test corner ready' line in the host log (hook not built, or the body was never teleported)" >> $R
  grep -h "test corner\|test bubble" $HOSTLOG | tail -5 | cut -c1-200 >> $R
  [ -z "$KEEP" ] && pkill -9 cataclysm-tiles
  exit 1
fi
RLINE=${READY%%:*}
BID=$(echo "$READY" | sed -E 's/.*body=([0-9]+).*/\1/')
echo "ready: $(echo "$READY" | sed -E 's/^[0-9]+:[0-9:.]+ INFO : //')" >> $R
sleep 2
shot corner_before

# hold north-east (w+d) with XTEST keys on the focused client window
xdotool windowfocus $CW; sleep 0.3
xdotool keyup w a s d 2>/dev/null; sleep 0.3
xdotool keydown w keydown d sleep $HOLD keyup d keyup w
sleep 1.5
shot corner_after

# evidence: only the trace lines of the client's body after the ready line
TR=$S/corner_trace.txt
tail -n +$((RLINE + 1)) $HOSTLOG | grep "Multiplayer: trace" | grep "id=$BID " | sed -E 's/^[0-9:.]+ INFO : Multiplayer: //' > $TR
SLIDE=$(grep -c "trace slide .*wanted=1,-1 took=1,0" $TR)
BLOCKED=$(grep -c "trace blocked .*dx=1 dy=-1" $TR)
EAST=$(grep -c "trace step .* dx=1 dy=0 " $TR)
NORTH=$(grep -c "trace step .* dy=-1 " $TR)
ERRS=$(grep -c ' ERROR ' $HOSTLOG)
{
  echo "body id $BID: slides(1,-1 -> 1,0)=$SLIDE east steps=$EAST blocked(1,-1)=$BLOCKED steps with dy=-1 (through the wall!)=$NORTH errors in host log=$ERRS"
  echo "-- first trace lines:"
  head -12 $TR | cut -c1-160
} >> $R
if [ "$SLIDE" -ge 1 ] && [ "$EAST" -ge 1 ] && [ "$BLOCKED" -eq 0 ] && [ "$NORTH" -eq 0 ]; then
  echo "PASS" >> $R
  RC=0
else
  echo "FAIL" >> $R
  RC=1
fi
[ -z "$KEEP" ] && pkill -9 cataclysm-tiles
echo "CORNER DONE (rc $RC)" >> $R
exit $RC
