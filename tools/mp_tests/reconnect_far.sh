#!/bin/bash
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
R=$S/RECONNECT_FAR_RESULT.txt; : > $R
pkill -9 cataclysm-tiles; sleep 3
rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
: > $S/ud_host/config/debug.log
CATA_MP_TEST_BUBBLE=quiet NO_STAIRS=1 bash $S/pair.sh > $S/pair.out 2>&1
source $S/win.env
sleep 100
echo "-- client killed" >> $R
kill -9 $(cat $S/client.pid) 2>/dev/null; sleep 15
echo "-- after kill: host alive $(pgrep -c -f 'mp-host')" >> $R
start_client; sleep 110
grep -h "Multiplayer" $S/ud_host/config/debug.log | grep -v "test " | cut -c1-170 >> $R
echo "-- errors: $(grep -c ' ERROR ' $S/ud_host/config/debug.log)" >> $R
echo "-- processes: $(pgrep -c cataclysm-tiles)" >> $R
shot rc1
pkill -9 cataclysm-tiles
echo "RECONNECT FAR DONE" >> $R
