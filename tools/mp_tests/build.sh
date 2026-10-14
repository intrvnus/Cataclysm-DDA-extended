#!/bin/bash
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
R=$S/BUILD_RESULT.txt; : > $R
pkill -9 cataclysm-tiles; sleep 3
rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
: > $S/ud_host/config/debug.log
CATA_MP_TEST_BUILD=1 NO_STAIRS=1 bash $S/pair.sh > $S/pair.out 2>&1
sleep 150
grep -h "test build" $S/ud_host/config/debug.log | sed 's/^[0-9:.]* INFO : //' | uniq -c | head -14 | cut -c1-200 >> $R
echo "-- errors: $(grep -c ' ERROR ' $S/ud_host/config/debug.log)" >> $R
grep -h " ERROR " $S/ud_host/config/debug.log | head -3 | cut -c1-200 >> $R
pkill -9 cataclysm-tiles
echo "BUILD DONE" >> $R
