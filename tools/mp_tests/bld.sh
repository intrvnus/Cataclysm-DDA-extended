#!/bin/bash
# copy changed sources to the build copy, build there, copy the binary back (edit the main tree meanwhile)
S=${MP_S:-/tmp/mp_test}
# copy only files that differ and give them a fresh time stamp: `cp -a` kept the old time, so a
# file edited while the build copy was compiling looked older than its object and was skipped
# (mismatched headers between objects -> heap corruption at start)
( cd /home/user/cataclysm-dda-extended/src && find . -type f | while read -r f; do
    cmp -s "$f" "/home/user/cdda-build/src/$f" || { mkdir -p "$(dirname "/home/user/cdda-build/src/$f")"; cp "$f" "/home/user/cdda-build/src/$f"; }
  done )
cd /home/user/cdda-build && make -j$(nproc) TILES=1 RELEASE=1 LINTJSON=0 ASTYLE=0 PCH=1 CCACHE=1 BACKTRACE=0 LOCALIZE=0 TESTS=0 > $S/bld.log 2>&1
r=$?
[ $r = 0 ] && cp cataclysm-tiles /home/user/cataclysm-dda-extended/cataclysm-tiles.new && mv -f /home/user/cataclysm-dda-extended/cataclysm-tiles.new /home/user/cataclysm-dda-extended/cataclysm-tiles.next
echo "exit $r" >> $S/bld.log
