#!/bin/bash
# Cursor-sector melee test.  Run: setsid nohup bash $S/melee_sector_test.sh > $S/melee.out 2>&1 &
# Starts host + client (pair.sh, CATA_MP_TEST_MONSTER=1: the host hook puts an immobilised, tough
# zombie on the south-east tile of the client's body after the quiet-field teleport), finds the
# pixel of the player's tile centre from the client's `trace cursor dx= dy=` lines, then clicks
# (left button) with the cursor at 14 angles on a circle around the player:
#   the 8 octant centres, plus boundary angles 20/70 (within the 12-degree tolerance towards the
#   SE target: must pick SE), 5/85/340/110 (must stay in their own sector).
# The host's `trace cursor` (cursor vector, sector) and `trace swing dx dy hit` lines are checked
# by meleecheck.py: the swing direction must be the sector of the cursor angle.
# Result: $S/MELEE_RESULT.txt.
# Env knobs: SKIP_START=1 reuse a running pair (win.env; the monster-ready line must be in the
#            host log), RADIUS_TILES=2 circle radius, GAP=2.0 seconds between clicks, KEEP=1.
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
export LANG=C.UTF-8 LC_ALL=C.UTF-8
R=$S/MELEE_RESULT.txt; : > $R
HOSTLOG=$S/ud_host/config/debug.log
CLOG=$S/ud_client/config/debug.log
RADIUS_TILES=${RADIUS_TILES:-2}
GAP=${GAP:-2.0}
MC="python3 -I $S/meleecheck.py"

if [ -z "$SKIP_START" ]; then
  pkill -9 cataclysm-tiles; sleep 3
  rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
  : > $HOSTLOG; : > $CLOG
  CATA_MP_TEST_TRACE=1 CATA_MP_TEST_BUBBLE=quiet CATA_MP_TEST_MONSTER=1 NO_STAIRS=1 bash $S/pair.sh > $S/pair.out 2>&1
fi
source $S/win.env
finish() { [ -z "$KEEP" ] && pkill -9 cataclysm-tiles; exit $1; }
if [ -z "$CW" ] || ! pgrep -x cataclysm-tiles > /dev/null; then
  echo "FAIL: game/client window not running (CW='$CW')" | tee -a $R
  exit 1
fi

READY=""
for i in $(seq 1 120); do
  READY=$(grep -n "Multiplayer: test monster ready" $HOSTLOG | tail -1)
  [ -n "$READY" ] && break
  grep -q "test monster failed" $HOSTLOG && break
  sleep 2
done
if [ -z "$READY" ]; then
  echo "FAIL: no 'test monster ready' line in the host log" >> $R
  grep -h "test monster\|test bubble" $HOSTLOG | tail -5 | cut -c1-200 >> $R
  finish 1
fi
RLINE=${READY%%:*}
BID=$(echo "$READY" | sed -E 's/.*body=([0-9]+).*/\1/')
echo "ready: $(echo "$READY" | sed -E 's/^[0-9]+:[0-9:.]+ INFO : //')" >> $R
sleep 3
shot melee_before
xdotool windowfocus $CW; sleep 0.3

# ---- find the pixel of the player's tile centre (root coordinates)
eval $(xdotool getwindowgeometry --shell $CW)   # X Y WIDTH HEIGHT
echo "client window: ${X},${Y} ${WIDTH}x${HEIGHT}" >> $R
ccount() { grep -c "Multiplayer: trace cursor" $CLOG; }
# move the cursor to root (x, y) and print "dx dy tilex tiley" as the client computed them
probe() {
  xdotool mousemove $(( $1 + 50 )) $2; sleep 0.4
  local n; n=$(ccount)
  xdotool mousemove $1 $2
  for i in $(seq 1 40); do sleep 0.1; [ "$(ccount)" -gt "$n" ] && break; done
  sleep 0.2
  grep "Multiplayer: trace cursor" $CLOG | tail -1 | \
    sed -E 's/.* dx=([-0-9.e+]+) dy=([-0-9.e+]+) .* tile=([0-9]+),([0-9]+).*/\1 \2 \3 \4/'
}
# first guess: centre of the map area, the sidebar (~352 px) is on the right
PX=$(( X + (WIDTH - 352) / 2 ))
PY=$(( Y + HEIGHT / 2 ))
for round in 1 2 3; do
  read DX DY TX TY <<< "$(probe $PX $PY)"
  if [ -z "$DX" ]; then
    echo "FAIL: no 'trace cursor' line from the client (cursor outside the map window? CATA_MP_TEST_TRACE not set?)" >> $R
    finish 1
  fi
  echo "calibration $round: mouse $PX,$PY -> cursor dx=$DX dy=$DY tile=${TX}x${TY}" >> $R
  read NPX NPY <<< "$($MC calib $PX $PY $DX $DY $TX $TY)"
  PX=$NPX; PY=$NPY
done
read DX DY TX TY <<< "$(probe $PX $PY)"
echo "player tile centre at root pixel $PX,$PY (verify: dx=$DX dy=$DY, should be ~0)" >> $R
RADIUS_PX=$(python3 -I -c "print(int(round($RADIUS_TILES * $TX)))")

# ---- the clicks
$MC plan > $S/melee_plan.txt
shot melee_start
NCLICK=0
while read ANG EX EY; do
  read MX MY <<< "$($MC point $PX $PY $RADIUS_PX $ANG)"
  xdotool mousemove $MX $MY
  sleep 0.5
  xdotool mousedown 1; sleep 0.1; xdotool mouseup 1
  NCLICK=$((NCLICK + 1))
  sleep $GAP
done < $S/melee_plan.txt
shot melee_end
echo "clicks sent: $NCLICK, radius ${RADIUS_PX}px" >> $R
echo "-- the monster afterwards:" >> $R
grep -h "Multiplayer: test monster at" $HOSTLOG | tail -1 | sed -E 's/^[0-9:.]+ INFO : //' >> $R

$MC check $HOSTLOG $BID $RLINE $S/melee_plan.txt $R
RC=$?
echo "errors in host log: $(grep -c ' ERROR ' $HOSTLOG)" >> $R
echo "MELEE DONE (rc $RC)" >> $R
finish $RC
