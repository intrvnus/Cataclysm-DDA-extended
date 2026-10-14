#!/bin/bash
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
R=$S/HOSTCHAT_RESULT.txt; : > $R
pkill -9 cataclysm-tiles; sleep 3
rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
: > $S/ud_host/config/debug.log
NO_STAIRS=1 bash $S/pair.sh > $S/pair.out 2>&1
source $S/win.env
xdotool windowfocus $HW; sleep 2
k $HW F2; sleep 2; shot hc1
xdotool type --delay 80 "hi from the host"; sleep 1; k $HW Return; sleep 3
k $HW F4; sleep 3; shot hc2; k $HW Return; sleep 2
xdotool windowfocus $CW; sleep 1; shot hc3
grep -h "chat " $S/ud_host/config/debug.log | cut -c1-160 >> $R
echo "-- errors: $(grep -c ' ERROR ' $S/ud_host/config/debug.log)" >> $R
pkill -9 cataclysm-tiles
echo "HOSTCHAT DONE" >> $R
