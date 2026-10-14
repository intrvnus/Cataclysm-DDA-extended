#!/bin/bash
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
R=$S/SIDE_RESULT.txt; : > $R
pkill -9 cataclysm-tiles; sleep 3
rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
: > $S/ud_host/config/debug.log; : > $S/ud_client/config/debug.log
CATA_MP_TEST_NPC=1 NO_STAIRS=1 bash $S/pair.sh > $S/pair.out 2>&1
source $S/win.env
xdotool windowfocus $CW; sleep 2
k $CW Up; sleep 3; shot side0
k $CW Return; sleep 4
k $CW shift+s; sleep 4; shot side1
k $CW shift+y; sleep 4; shot side2
grep -h "answers\|answering\|talk\|ERROR\| yell\|shout" $S/ud_host/config/debug.log | tail -12 | cut -c1-170 >> $R
grep -h "answering" $S/ud_client/config/debug.log | tail -3 | cut -c1-170 >> $R
echo "-- errors: $(grep -c ' ERROR ' $S/ud_host/config/debug.log)" >> $R
pkill -9 cataclysm-tiles
echo "SIDE DONE" >> $R
