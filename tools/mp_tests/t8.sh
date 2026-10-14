#!/bin/bash
source ${MP_S:-/tmp/mp_test}/xt.sh
source $S/win.env
xdotool windowraise $CW
shot e0
k $CW m; sleep 4; shot e1; k $CW Escape; sleep 2
k $CW shift+5; sleep 3; shot e2; k $CW Escape; sleep 2
k $CW asterisk; sleep 3; shot e3; k $CW Escape; sleep 2
k $CW shift+x; sleep 3; shot e4; k $CW Escape; sleep 2
grep -h "Multiplayer\|ERROR" $S/ud_host/config/debug.log | tail -5 | cut -c1-160
