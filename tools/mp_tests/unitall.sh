#!/bin/bash
S=${MP_S:-/tmp/mp_test}
cd /home/user/cdda-test
export DISPLAY=:99 LANG=C.UTF-8 LC_ALL=C.UTF-8 SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
timeout 7000 ./tests/cata_test --user-dir=$S/ut2/ -r compact > $S/ut_all.log 2>&1
echo "exit $?" > $S/UNITALL_DONE
