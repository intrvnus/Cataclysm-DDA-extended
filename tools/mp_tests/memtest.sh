#!/bin/bash
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
R=$S/MEM_RESULT.txt; : > $R
pkill -9 cataclysm-tiles; sleep 3
rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
: > $S/ud_host/config/debug.log
echo "before: $(find $S/ud_client/save/Leeds -newermt '-3 minutes' | wc -l) files newer than 3 min" >> $R
ls -la --time-style=full-iso $S/ud_client/save/Leeds | grep -i "mm1\|memory" | head -3 >> $R
NO_STAIRS=1 bash $S/pair.sh > $S/pair.out 2>&1
source $S/win.env
xdotool windowfocus $CW; sleep 25
k $CW Escape; sleep 2; k $CW Escape; sleep 5
shot mem1
echo "client alive after leaving: $(pgrep -c -f 'mp-connect')" >> $R
ls -la --time-style=full-iso $S/ud_client/save/Leeds | grep -i "mm1\|memory" | head -3 >> $R
find $S/ud_client/save/Leeds -newermt '-3 minutes' | head -5 >> $R
date >> $R
pkill -9 cataclysm-tiles
echo "MEM DONE" >> $R
