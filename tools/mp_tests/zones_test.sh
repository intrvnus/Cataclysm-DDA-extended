#!/bin/bash
# Zones window of a client: the host holds a zone (made by the hook CATA_MP_TEST_ZONE), the client
# presses the zones key (shift+y), disables the first zone with D, leaves the window (Escape, then
# y = save the changes).  Host log must show: zone created, "zones sent N", "zones set by ...", and
# the report line with TESTZONE enabled=0 afterwards.
# Run: bash $S/zones_test.sh      Result: $S/ZONES_RESULT.txt (screenshots zones0..zones4.png)
S=${MP_S:-/tmp/mp_test}
source $S/mp_menu_lib.sh
R=$S/ZONES_RESULT.txt
: > $R

start_fresh CATA_MP_TEST_ZONE=1
ok=1
if waitlog $HL "Multiplayer: test zone created" 60; then
  echo "host: $(grep -h 'Multiplayer: test zone created' $HL | head -1 | strip)" >> $R
else
  echo "MISSING: no 'test zone created' line in the host log (did the client join?)" >> $R
  ok=0
fi
sleep 3
shot zones0

focus_client
k $CW shift+y            # zones window
sleep 5
shot zones1              # the window with TESTZONE in the list
if grep -q "Multiplayer: zones sent [1-9]" $HL; then
  echo "host: $(grep -h 'Multiplayer: zones sent' $HL | tail -1 | strip)" >> $R
else
  echo "MISSING: host never logged 'zones sent N' with N >= 1 (the zones_req/zones round trip)" >> $R
  ok=0
fi
k $CW d                  # DISABLE_ZONE on the first zone
sleep 1
shot zones2
k $CW Escape             # leave: "Save changes?"
sleep 2
shot zones3
k $CW y                  # yes: the client sends zones_set
sleep 4
shot zones4
if waitlog $HL "Multiplayer: zones set by .*now holds [1-9]" 20; then
  echo "host: $(grep -h 'Multiplayer: zones set by' $HL | tail -1 | strip)" >> $R
else
  echo "MISSING: host never logged 'zones set by ... N zones' (the zones_set message)" >> $R
  ok=0
fi
sleep 8
report=$(grep -h "Multiplayer: test zone report" $HL | tail -1)
echo "host (last report): $(echo "$report" | strip)" >> $R
if ! echo "$report" | grep -q "TESTZONE"; then
  echo "MISSING: TESTZONE is not among the zones of the host after the round trip" >> $R
  ok=0
fi
if echo "$report" | grep -q "TESTZONE enabled=0"; then
  echo "edit visible on the host: yes (TESTZONE was disabled in the client window)" >> $R
else
  echo "edit visible on the host: NO (round trip worked but D did not disable the zone: look at zones1..zones4.png)" >> $R
fi
echo "client 'not available' lines: $(grep -c 'client action not available' $CL)" >> $R
echo "host errors: $(grep -c ' ERROR ' $HL)" >> $R
echo "screenshots: zones0 (before) zones1 (window open) zones2 (after D) zones3 (Save changes?) zones4 (after y) in $S" >> $R
pkill -9 cataclysm-tiles
finish ZONES $R $ok
