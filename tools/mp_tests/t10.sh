#!/bin/bash
source ${MP_S:-/tmp/mp_test}/xt.sh
source $S/win.env
xdotool windowraise $CW
shot g0
k $CW shift+m; sleep 4; shot g1
