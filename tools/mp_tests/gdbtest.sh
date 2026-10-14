#!/bin/bash
source ${MP_S:-/tmp/mp_test}/xt.sh
pkill -9 cataclysm-tiles; sleep 3
rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
(cd $G; export CATA_MP_TEST_NPC=1 CATA_MP_TEST_VEHICLE=1; ./cataclysm-tiles --userdir $S/ud_host/ --world Leeds --mp-host > $S/host.log 2>&1 & echo $! > $S/host.pid); sleep 90
(cd $G; gdb -batch -x $S/gdbcmds.txt --args ./cataclysm-tiles --userdir $S/ud_client/ --world Leeds --mp-connect 127.0.0.1 > $S/gdb.out 2>&1 &) ; sleep 110
echo done
