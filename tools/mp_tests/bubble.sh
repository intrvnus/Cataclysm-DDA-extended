#!/bin/bash
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
R=$S/BUBBLE_RESULT.txt; : > $R
pkill -9 cataclysm-tiles; sleep 3
rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
: > $S/ud_host/config/debug.log; : > $S/ud_client/config/debug.log
CATA_MP_TEST_BUBBLE=${MODE:-1} NO_STAIRS=1 bash $S/pair.sh > $S/pair.out 2>&1
source $S/win.env
sleep 40
shot bub1
sleep 15
shot bub2
sleep ${WAIT:-120}
grep -h "test bubble\|opened an extra" $S/ud_host/config/debug.log | sed 's/^[0-9:.]* INFO : //' | awk 'NR<=4 || NR%3==0' | head -30 | cut -c1-220 >> $R
echo "-- errors: $(grep -c ' ERROR ' $S/ud_host/config/debug.log)" >> $R
grep -h " ERROR " $S/ud_host/config/debug.log | sort | uniq -c | head -5 | cut -c1-220 >> $R
echo "-- alive: $(pgrep -c cataclysm-tiles)" >> $R
pkill -9 cataclysm-tiles
echo "BUBBLE DONE" >> $R
