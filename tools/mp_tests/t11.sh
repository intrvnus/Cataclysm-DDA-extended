#!/bin/bash
source ${MP_S:-/tmp/mp_test}/xt.sh
source $S/win.env
xdotool windowraise $CW
shot h0
k $CW shift+m; sleep 4; shot h1
k $CW Return; sleep 2
k $CW plus; sleep 4; shot h2
