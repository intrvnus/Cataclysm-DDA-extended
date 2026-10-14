#!/bin/bash
# Factions window of a client: key # (ACTION_FACTIONS) sends factions_req, the host answers with the text
# of the factions the group knows (the hook CATA_MP_TEST_FACTIONS makes two known) and the client shows
# it in a popup.  Host log: "factions sent to NAME: N factions, text length L".
# Run: bash $S/factions_test.sh      Result: $S/FACTIONS_RESULT.txt (screenshots factions0, factions1)
S=${MP_S:-/tmp/mp_test}
source $S/mp_menu_lib.sh
R=$S/FACTIONS_RESULT.txt
: > $R

start_fresh CATA_MP_TEST_FACTIONS=1
ok=1
if waitlog $HL "Multiplayer: test factions:" 60; then
  echo "host: $(grep -h 'Multiplayer: test factions:' $HL | head -1 | strip)" >> $R
else
  echo "MISSING: no 'test factions' line in the host log (did the client join?)" >> $R
  ok=0
fi
sleep 3
shot factions0

focus_client
k $CW numbersign         # the factions key (#; shift+3)
sleep 4
shot factions1           # the popup with the faction list
if waitlog $HL "Multiplayer: factions sent to" 10; then
  line=$(grep -h "Multiplayer: factions sent to" $HL | tail -1)
  echo "host: $(echo "$line" | strip)" >> $R
  shown=$(echo "$line" | sed -n 's/.*: \([0-9]*\) factions, text length.*/\1/p')
  len=$(echo "$line" | sed -n 's/.*text length \([0-9]*\).*/\1/p')
  if [ "${shown:-0}" -lt 1 ]; then
    echo "NOTE: the host sent 0 factions (the hook could not make any known: see the 'test factions' line); the window shows the 'not met any factions' text" >> $R
    ok=0
  fi
  echo "text length $len" >> $R
else
  echo "MISSING: the host never logged 'factions sent' (the factions_req did not arrive)" >> $R
  ok=0
fi
k $CW Escape
sleep 1
echo "client 'not available' lines: $(grep -c 'client action not available' $CL)" >> $R
echo "the text itself is only on screen: look at factions1.png (it must not say 'The host did not answer')" >> $R
echo "host errors: $(grep -c ' ERROR ' $HL)" >> $R
echo "screenshots: factions0 (before) factions1 (popup open) in $S" >> $R
pkill -9 cataclysm-tiles
finish FACTIONS $R $ok
