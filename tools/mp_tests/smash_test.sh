#!/bin/bash
# smash test: s, then a direction (h = west, towards the lockers)
source ${MP_S:-/tmp/mp_test}/xt.sh
source $S/win.env
k $CW s; sleep 2; shot smash1; k $CW h; sleep 6; shot smash2
grep -h "Multiplayer" $S/ud_host/config/debug.log | tail -3 | cut -c1-160
