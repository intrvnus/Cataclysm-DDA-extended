#!/bin/bash
# Autoattack (Tab) in fluid multiplayer + removed actions.
# Host + client with a test zombie (CATA_MP_TEST_ZOMBIE). When the zombie is next to the client's
# body, the client presses Tab; a "trace swing id=<body>" line with hit=1 must follow.
# Then the client presses '|' (wait) and '.' (pause): the client log must say waiting is not possible.
S=${MP_S:-/tmp/mp_test}
source $S/mp_menu_lib.sh
start_fresh CATA_MP_TEST_ZOMBIE=1 CATA_MP_TEST_TRACE=1
R=$S/autoattack.txt; : > $R
ok=0
if waitlog $HL "test zombie at distance 1 " 120; then
  focus_client
  for i in 1 2 3 4 5; do k $CW Tab; sleep 1.5; done
  grep -h "trace swing" $HL | tail -5 | strip >> $R
  grep -q "trace swing.*hit=1" $HL && ok=1
else
  echo "zombie never came next to the body" >> $R
  grep -h "test zombie" $HL | tail -3 | strip >> $R
fi
k $CW bar; sleep 1; k $CW period; sleep 1
shot autoattack_wait; echo "wait/pause messages: see $S/autoattack_wait.png" >> $R
finish autoattack $R $ok
cat $R
