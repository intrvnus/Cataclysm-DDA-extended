#!/bin/bash
source ${MP_S:-/tmp/mp_test}/xt.sh
(cd $G; export CATA_MP_TEST_BEAT=1; ./cataclysm-tiles --userdir $S/ud_host/ --world Leeds --mp-server > $S/host.log 2>&1 & echo $! > $S/host.pid); sleep 80
start_client; sleep 100
CW=$(xwininfo -root -tree | grep -i cataclysm | awk '{print $1}' | tail -1)
echo "export CW=$CW" > $S/win.env
xdotool windowraise $CW; shot d0; sleep 20; shot d1
echo done
