#!/bin/bash
# usage: apply_test.sh  -- starts host+client, applies a dime on the client, greps host debug log
source ${MP_S:-/tmp/mp_test}/xt.sh
pkill cataclysm-tiles; sleep 1
start_host; sleep 90
HW=$(xwininfo -root -tree | grep -i cataclysm | awk '{print $1}' | head -1)
start_client; sleep 100
CW=$(xwininfo -root -tree | grep -i cataclysm | awk '{print $1}' | grep -v $HW | head -1)
echo host=$HW client=$CW
k $CW a; sleep 3; k $CW Down Down Down; sleep 1; k $CW Return; sleep 5
shot t_client
pkill -TERM cataclysm-tiles; sleep 4
echo ---- host debug.log
grep -ih "multiplayer" $S/ud_host/config/debug.log $S/ud_client/config/debug.log | cut -c1-200 | tail -10
