#!/bin/bash
S=${MP_S:-/tmp/mp_test}
cp -au /home/user/cataclysm-dda-extended/src/. /home/user/cdda-test/src/
cp -au /home/user/cataclysm-dda-extended/tests/. /home/user/cdda-test/tests/ 2>/dev/null
cd /home/user/cdda-test && make -j$(nproc) TILES=1 RELEASE=1 LINTJSON=0 ASTYLE=0 PCH=1 CCACHE=1 BACKTRACE=0 LOCALIZE=0 TESTS=1 > $S/utbuild.log 2>&1
echo "exit $?" >> $S/utbuild.log
[ $? = 0 ] && bash $S/unit.sh
