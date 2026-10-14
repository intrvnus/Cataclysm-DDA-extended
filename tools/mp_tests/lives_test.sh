#!/bin/bash
# Multiplayer lives (src/mp_lives.cpp): CATA_MP_TEST_DOWN=1|bed|host|nolives (see test_downed_cycle).
# Kill after 20 s, help up after 8 s, kill again, bleed out (60 s), report at 75 s.
# Usage: lives_test.sh MODE   -> result in $S/lives_MODE.txt
S=${MP_S:-/tmp/mp_test}
source $S/mp_menu_lib.sh
MODE=${1:-1}
R=$S/lives_$MODE.txt; : > $R
start_fresh CATA_MP_TEST_DOWN=$MODE
waitlog $HL "test: after bleeding out|out of lives" 200
sleep 5
grep -h "test: \|lives of\|respawn\|out of lives\|host is down\|helped up" $HL | strip >> $R
grep -h "out of lives\|Multiplayer: test" $CL | strip >> $R
shot lives_$MODE
ok=0
case $MODE in
  1)   grep -q "after bleeding out downed=0 allow_death=0 dead=0.*lives 1 extra life left.*incorporeal 1" $R && ok=1 ;;
  bed) grep -q "respawns at the bed" $R && grep -q "after bleeding out.*dead=0" $R && ok=1 ;;
  host) grep -q "host is down" $R && grep -q "after bleeding out.*lives 1 extra life left.*incorporeal 1" $R && ok=1 ;;
  nolives) grep -q "out of lives" $R && ok=1 ;;
esac
finish lives_$MODE $R $ok
cat $R
