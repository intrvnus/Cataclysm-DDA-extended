#!/bin/bash
source ${MP_S:-/tmp/mp_test}/xt.sh
source $S/win.env
xdotool windowraise $CW
k $CW shift+m; sleep 4; shot f1; k $CW Escape; sleep 2
k $CW equal; sleep 4; shot f2; k $CW Escape; sleep 2
k $CW shift+y; sleep 4; shot f3; k $CW Escape; sleep 3; k $CW Escape; sleep 1
k $CW m; sleep 5; shot f4; k $CW Escape; sleep 2
grep -h "Multiplayer\|ERROR" $S/ud_host/config/debug.log | tail -6 | cut -c1-170
