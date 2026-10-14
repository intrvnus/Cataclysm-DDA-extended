#!/bin/bash
# copy changed sources to the build copy, build there, copy the binary back (edit the main tree meanwhile)
S=${MP_S:-/tmp/mp_test}
cp -au /home/user/cataclysm-dda-extended/src/. /home/user/cdda-build/src/
cd /home/user/cdda-build && make -j$(nproc) TILES=1 RELEASE=1 LINTJSON=0 ASTYLE=0 PCH=1 CCACHE=1 BACKTRACE=0 LOCALIZE=0 TESTS=0 > $S/bld.log 2>&1
r=$?
[ $r = 0 ] && cp cataclysm-tiles /home/user/cataclysm-dda-extended/cataclysm-tiles.new && mv -f /home/user/cataclysm-dda-extended/cataclysm-tiles.new /home/user/cataclysm-dda-extended/cataclysm-tiles.next
echo "exit $r" >> $S/bld.log
