#!/bin/bash
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
R=$S/GRAB_RESULT.txt; : > $R
pkill -9 cataclysm-tiles; sleep 3
rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
: > $S/ud_host/config/debug.log; : > $S/ud_client/config/debug.log
CATA_MP_TEST_FURN=${FURN:-1} NO_STAIRS=1 bash $S/pair.sh > $S/pair.out 2>&1
source $S/win.env
xdotool windowfocus $CW; sleep 2
grep -h "test table" $S/ud_host/config/debug.log | head -2 | cut -c1-170 >> $R
shot gr0
k $CW shift+g; sleep 2; shot gr1
k $CW l; sleep 3; shot gr2
for i in 1 2 3; do k $CW h; sleep 2; done
shot gr3
for i in 1 2; do k $CW l; sleep 2; done
sleep 6
echo "-- table/body lines (distinct):" >> $R
grep -h "test table at" $S/ud_host/config/debug.log | sed 's/^[0-9:.]* INFO : //' | uniq -c | head -12 >> $R
grep -h "ui action 'grab'\|grab" $S/ud_host/config/debug.log | grep -v "test table" | head -3 | cut -c1-170 >> $R
echo "-- errors: $(grep -c ' ERROR ' $S/ud_host/config/debug.log)" >> $R
pkill -9 cataclysm-tiles
echo "GRAB DONE" >> $R
