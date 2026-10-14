#!/bin/bash
source ${MP_S:-/tmp/mp_test}/xt.sh
source $S/win.env
xdotool windowraise $CW
shot n0
# walk into the NPC north of us: menu
k $CW k; sleep 4; shot n1
# examine wounds
k $CW w; sleep 5; shot n2
k $CW Escape; sleep 2
# again: examine status
k $CW k; sleep 3; k $CW e; sleep 5; shot n3
k $CW Escape; sleep 2
# push away
k $CW k; sleep 3; k $CW p; sleep 4; shot n4
# local views
k $CW shift+v; sleep 3; shot n5; k $CW Escape; sleep 1
k $CW shift+n; sleep 3; shot n6; k $CW Escape; sleep 1
k $CW z; sleep 2; shot n7
grep -h "Multiplayer\|ERROR" $S/ud_host/config/debug.log | tail -5 | cut -c1-140
