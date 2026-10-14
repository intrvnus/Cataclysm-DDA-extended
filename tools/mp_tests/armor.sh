#!/bin/bash
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
R=$S/ARMOR_RESULT.txt; : > $R
pkill -9 cataclysm-tiles; sleep 3
rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
: > $S/ud_host/config/debug.log
NO_STAIRS=1 CATA_MP_TEST_ARMOR=1 bash $S/pair.sh > $S/pair.out 2>&1
source $S/win.env
xdotool windowfocus $CW; sleep 1
sleep 8
k $CW plus; sleep 4; shot ar1
k $CW Down; sleep 1; k $CW Return; sleep 1; k $CW Down; sleep 1; k $CW Down; sleep 1; k $CW Return; sleep 2; shot ar2
k $CW Escape; sleep 15
echo "-- distinct worn orders on the host:" >> $R
grep -h "test worn order" $S/ud_host/config/debug.log | sed 's/^[0-9:.]* INFO : //' | uniq -c >> $R
grep -h "sort_armor" $S/ud_host/config/debug.log | cut -c1-160 >> $R
echo "-- errors: $(grep -c ' ERROR ' $S/ud_host/config/debug.log)" >> $R
pkill -9 cataclysm-tiles
echo "ARMOR DONE" >> $R
