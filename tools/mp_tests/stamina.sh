#!/bin/bash
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
R=$S/STAMINA_RESULT.txt; : > $R
pkill -9 cataclysm-tiles; sleep 3
rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
: > $S/ud_host/config/debug.log; : > $S/ud_client/config/debug.log
CATA_MP_TEST_STAMINA=1 CATA_MP_TEST_BUBBLE=quiet NO_STAIRS=1 bash $S/pair.sh > $S/pair.out 2>&1
sleep 200
grep -h "stamina test" $S/ud_host/config/debug.log | sed 's/^[0-9:.]* INFO : Multiplayer: stamina test //' | awk 'NR%4==1' >> $R
echo "-- errors: $(grep -c ' ERROR ' $S/ud_host/config/debug.log)" >> $R
pkill -9 cataclysm-tiles
echo "STAMINA DONE" >> $R
