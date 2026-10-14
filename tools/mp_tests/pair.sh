#!/bin/bash
# starts host + client and leaves them running; prints window ids
source ${MP_S:-/tmp/mp_test}/xt.sh
pkill -9 cataclysm-tiles; sleep 1
start_host; sleep 90
HW=$(xwininfo -root -tree | grep -i cataclysm | awk '{print $1}' | head -1)
start_client; sleep 100
CW=$(xwininfo -root -tree | grep -i cataclysm | awk '{print $1}' | grep -v $HW | head -1)
echo "export HW=$HW CW=$CW" > $S/win.env
echo host=$HW client=$CW
