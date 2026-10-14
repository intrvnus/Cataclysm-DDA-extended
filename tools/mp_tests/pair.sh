#!/bin/bash
# starts host + client and leaves them running; prints window ids
source ${MP_S:-/tmp/mp_test}/xt.sh
pkill -9 -x cataclysm-tiles; sleep 1
start_host; sleep 90
HW=$(xdotool search --pid $(cat $S/host.pid) | tail -1)
start_client; sleep 100
CW=$(xdotool search --pid $(cat $S/client.pid) | tail -1)
echo "export HW=$HW CW=$CW" > $S/win.env
echo host=$HW client=$CW
