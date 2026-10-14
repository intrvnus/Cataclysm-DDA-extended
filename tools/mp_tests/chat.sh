#!/bin/bash
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
R=$S/CHAT_RESULT.txt; : > $R
pkill -9 cataclysm-tiles; sleep 3
rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
: > $S/ud_host/config/debug.log
NO_STAIRS=1 bash $S/pair.sh > $S/pair.out 2>&1
source $S/win.env
xdotool windowfocus $CW; sleep 2
k $CW F2; sleep 2; shot ch1
xdotool type --delay 80 "hello friends"; sleep 1; k $CW Return; sleep 3; shot ch2
k $CW F4; sleep 3; shot ch3
k $CW Return; sleep 2
grep -h "chat " $S/ud_host/config/debug.log | cut -c1-160 >> $R
echo "-- errors: $(grep -c ' ERROR ' $S/ud_host/config/debug.log)" >> $R
pkill -9 cataclysm-tiles
echo "CHAT DONE" >> $R
