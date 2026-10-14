#!/bin/bash
# Free movement phase 2 test.  Run: CASE=door setsid nohup bash $S/freetest.sh > $S/free.out 2>&1 &
# CASE = door | attack | talk | squeeze | swim | ledge | vehicle.
# Starts host + client (pair.sh) with the host hook CATA_MP_TEST_FREE=$CASE, which builds a walled
# corridor east of the client's body with the case's obstacle 3 tiles east (squeeze: the only way
# on is a diagonal gap north-east).  Holds d (squeeze: w+d) on the client for HOLD seconds and
# checks the hook's "test free state" lines and the trace.
# Result: $S/FREE_RESULT_$CASE.txt (PASS / FAIL + evidence).  KEEP=1 does not kill the game.
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
export LANG=C.UTF-8 LC_ALL=C.UTF-8
CASE=${CASE:-door}
R=$S/FREE_RESULT_$CASE.txt; : > $R
HOSTLOG=$S/ud_host/config/debug.log
HOLD=${HOLD:-4}

pkill -9 cataclysm-tiles; sleep 3
rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
: > $HOSTLOG; : > $S/ud_client/config/debug.log
CATA_MP_TEST_TRACE=1 CATA_MP_TEST_BUBBLE=quiet CATA_MP_TEST_FREE=$CASE NO_STAIRS=1 bash $S/pair.sh > $S/pair.out 2>&1
source $S/win.env
if [ -z "$CW" ] || ! pgrep -x cataclysm-tiles > /dev/null; then
  echo "FAIL: game/client window not running (CW='$CW')" | tee -a $R
  exit 1
fi

READY=""
for i in $(seq 1 120); do
  READY=$(grep -n "Multiplayer: test free ready" $HOSTLOG | tail -1)
  [ -n "$READY" ] && break
  grep -q "test free failed" $HOSTLOG && break
  sleep 2
done
if [ -z "$READY" ]; then
  echo "FAIL: no 'test free ready' line in the host log" >> $R
  grep -h "test free\|test bubble" $HOSTLOG | tail -5 | cut -c1-200 >> $R
  [ -z "$KEEP" ] && pkill -9 cataclysm-tiles
  exit 1
fi
RLINE=${READY%%:*}
BID=$(echo "$READY" | sed -E 's/.*body=([0-9]+).*/\1/')
echo "ready: $(echo "$READY" | sed -E 's/^[0-9]+:[0-9:.]+ INFO : //')" >> $R
sleep 2
shot free_${CASE}_before

xdotool windowfocus $CW; sleep 0.3
xdotool keyup w a s d 2>/dev/null; sleep 0.3
if [ "$CASE" = squeeze ]; then
  xdotool keydown w keydown d sleep $HOLD keyup d keyup w
else
  xdotool keydown d sleep $HOLD keyup d
fi
sleep 2
shot free_${CASE}_after
# close a menu the bump may have opened (talk), without leaving the game
xdotool key --window $CW space; sleep 1

TR=$S/free_${CASE}_trace.txt
tail -n +$((RLINE + 1)) $HOSTLOG | grep "Multiplayer: \(trace\|test free\)" |
  grep "id=$BID \|test free\|npc menu id=$BID" | sed -E 's/^[0-9:.]+ INFO : Multiplayer: //' > $TR
ST=$(grep "test free state" $TR)
ERRS=$(grep -c ' ERROR ' $HOSTLOG)
maxx=$(echo "$ST" | sed -nE 's/.* off=(-?[0-9]+),.*/\1/p' | sort -n | tail -1)
case $CASE in
  door)    ok=$( [ -n "$(echo "$ST" | grep 'target_ter=t_door_o')" ] && [ "${maxx:-0}" -ge 4 ] && echo 1 ) ;;
  attack)  ok=$( echo "$ST" | grep -qE 'target_hp=([0-9]{1,4}|[0-9]{5}) ' && ! echo "$ST" | tail -1 | grep -q 'target_hp=100000 ' && echo 1 ) ;;
  talk)    ok=$( grep -q "trace npc menu id=$BID" $TR && echo 1 ) ;;
  squeeze) ok=$( echo "$ST" | grep -q ' off=1,-1,0 ' && echo 1 ) ;;
  swim)    ok=$( [ "${maxx:-0}" -ge 7 ] && echo 1 ) ;;
  ledge)   ok=$( echo "$ST" | grep -q ' off=[0-9-]*,[0-9-]*,-1 ' && echo 1 ) ;;
  vehicle) ok=$( echo "$ST" | grep -q 'in_vehicle=1' && echo 1 ) ;;
esac
{
  echo "case $CASE body $BID: furthest x offset=${maxx:-?} errors in host log=$ERRS"
  echo "-- state lines:"; echo "$ST" | cut -c1-200 | head -20
  echo "-- trace (first 15):"; grep -v "test free" $TR | head -15 | cut -c1-160
  grep ' ERROR ' $HOSTLOG | head -3 | cut -c1-200
} >> $R
if [ -n "$ok" ]; then echo "PASS" >> $R; RC=0; else echo "FAIL" >> $R; RC=1; fi
[ -z "$KEEP" ] && pkill -9 cataclysm-tiles
echo "FREE DONE $CASE (rc $RC)" >> $R
exit $RC
