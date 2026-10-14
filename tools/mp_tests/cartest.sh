#!/bin/bash
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
R=$S/CAR_RESULT.txt; : > $R
pkill -9 cataclysm-tiles; sleep 3
rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
: > $S/ud_host/config/debug.log; : > $S/ud_client/config/debug.log
CATA_MP_TEST_VEHICLE=1 bash $S/pair.sh > $S/pair.out 2>&1
source $S/win.env
xdotool windowfocus $CW; sleep 2
k $CW asciicircum; sleep 18
shot car1
for i in $(seq 1 45); do k $CW Up; sleep 1; done
sleep 45
shot car2
grep -h "test car at" $S/ud_host/config/debug.log | grep -v "velocity 0 engine" | awk 'NR%4==1' | tail -12 | cut -c1-170 >> $R
echo "-- drive: $(grep -c "ui action 'drive'" $S/ud_host/config/debug.log) control: $(grep -c "ui action 'control_vehicle'" $S/ud_host/config/debug.log) errors: $(grep -c ' ERROR ' $S/ud_host/config/debug.log)" >> $R
grep -h "control\|vehicle\|Multiplayer: .*(replay|failed)" $S/ud_host/config/debug.log | grep -v "test car at" | head -8 | cut -c1-200 >> $R
grep -h "" $S/ud_client/config/debug.log | grep -i "vehicle\|control" | head -5 | cut -c1-200 >> $R
pkill -9 cataclysm-tiles
echo "CAR DONE" >> $R
