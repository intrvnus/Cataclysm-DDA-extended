#!/bin/bash
# One unattended run: A) car, B) zombie + downed cycle, C) dedicated server. Results in SWEEP_RESULT.txt
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
R=$S/SWEEP_RESULT.txt
: > $R
fresh() {
  pkill -9 cataclysm-tiles; sleep 3
  rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
  : > $S/ud_host/config/debug.log
  : > $S/ud_client/config/debug.log
}
section() { echo "" >> $R; echo "===== $1 =====" >> $R; }

# ---- A: car
fresh
section "A car (CATA_MP_TEST_NPC CATA_MP_TEST_VEHICLE)"
CATA_MP_TEST_VEHICLE=1 bash $S/pair.sh > $S/pair.out 2>&1
source $S/win.env
xdotool windowfocus $CW; sleep 1
k $CW asciicircum; sleep 18
shot sw_a1
for i in 1 2 3 4 5 6; do k $CW Up; sleep 1; done
sleep 15
shot sw_a2
grep -h "test car\|test player\|test NPC\|no open ground\|ERROR" $S/ud_host/config/debug.log | head -3 >> $R
echo "-- car lines (every 6th):" >> $R
grep -h "test car at\|test body is not" $S/ud_host/config/debug.log | awk 'NR%6==1' | head -24 >> $R
echo "-- drive actions: $(grep -c "ui action 'drive'" $S/ud_host/config/debug.log)  control_vehicle: $(grep -c "ui action 'control_vehicle'" $S/ud_host/config/debug.log)  failed: $(grep -c "failed" $S/ud_host/config/debug.log)" >> $R
echo "-- errors: $(grep -c ' ERROR ' $S/ud_host/config/debug.log) (NC_NONE: $(grep -c NC_NONE $S/ud_host/config/debug.log))" >> $R

# ---- B: zombie + downed cycle
fresh
section "B zombie + downed cycle (NO_STAIRS NPC ZOMBIE DOWN)"
NO_STAIRS=1 CATA_MP_TEST_ZOMBIE=1 CATA_MP_TEST_DOWN=1 bash $S/pair.sh > $S/pair.out 2>&1
sleep 170
grep -h "test zombie spawned\|test NPC\|no free spot\|no place" $S/ud_host/config/debug.log | head -3 >> $R
echo "-- zombie lines (every 20th):" >> $R
grep -h "test zombie at" $S/ud_host/config/debug.log | awk 'NR%20==1' | head -12 >> $R
echo "-- downed cycle:" >> $R
grep -h "Multiplayer: test: \|is down\|Respawning\|joined" $S/ud_host/config/debug.log | head -12 >> $R
echo "-- errors: $(grep -c ' ERROR ' $S/ud_host/config/debug.log) (NC_NONE: $(grep -c NC_NONE $S/ud_host/config/debug.log))" >> $R
echo "-- client alive: $(pgrep -c cataclysm-tiles) processes" >> $R

# ---- C: dedicated
fresh
section "C dedicated server (heartbeat)"
bash $S/ded.sh > $S/ded.out 2>&1
grep -h "dedicated heartbeat" $S/ud_host/config/debug.log | sed -n '1p;$p' >> $R
echo "-- heartbeat lines: $(grep -c 'dedicated heartbeat' $S/ud_host/config/debug.log)  errors: $(grep -c ' ERROR ' $S/ud_host/config/debug.log)" >> $R
pkill -9 cataclysm-tiles
echo "SWEEP DONE" >> $R
