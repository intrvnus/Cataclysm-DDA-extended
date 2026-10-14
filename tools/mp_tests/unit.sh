#!/bin/bash
S=${MP_S:-/tmp/mp_test}
cd /home/user/cdda-test
export DISPLAY=:99 LANG=C.UTF-8 LC_ALL=C.UTF-8 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
: > $S/UNIT_RESULT.txt
for tag in "[vehicle]" "[monster]" "[npc]" "[construction]" "[iuse_actor]" "[melee]" "[mission]" "[faction]"; do
  echo "===== $tag" >> $S/UNIT_RESULT.txt
  timeout 1200 ./tests/cata_test "$tag" --user-dir=$S/ut1/ > $S/ut_last.log 2>&1
  echo "exit $?" >> $S/UNIT_RESULT.txt
  tail -4 $S/ut_last.log | cut -c1-160 >> $S/UNIT_RESULT.txt
done
echo "UNIT DONE" >> $S/UNIT_RESULT.txt
