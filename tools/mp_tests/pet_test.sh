#!/bin/bash
# Pets: the host hook (CATA_MP_TEST_PET) puts a tame, tied dog next to the client's body.  The client
# examines it (e + the direction of the dog) and must get the pet menu ("What to do with your
# ...?"); it chooses "Swap positions" (s) and the host must replay the examine ('examine' ui action,
# not failed).  Set PET_MODE=free to leave the dog untied (it may wander off before the keys).
# Run: bash $S/pet_test.sh      Result: $S/PET_RESULT.txt (screenshots pet0..pet3.png)
S=${MP_S:-/tmp/mp_test}
source $S/mp_menu_lib.sh
R=$S/PET_RESULT.txt
: > $R

start_fresh CATA_MP_TEST_PET=${PET_MODE:-tied}
ok=1
dir=""
if waitlog $HL "Multiplayer: test pet spawned" 60; then
  line=$(grep -h "Multiplayer: test pet spawned" $HL | head -1)
  echo "host: $(echo "$line" | strip)" >> $R
  dir=$(echo "$line" | sed 's/.*test pet spawned \([a-z]*\) of the body.*/\1/')
else
  echo "MISSING: no 'test pet spawned' line in the host log (no room, or no client joined)" >> $R
  ok=0
fi
sleep 6            # the next state message gives the client the dog (with its tame state)
shot pet0
case "$dir" in
  east) key=Right ;;
  west) key=Left ;;
  south) key=Down ;;
  north) key=Up ;;
  *) key=Right ;;
esac

focus_client
before=$(grep -c "sending ui action 'examine'" $CL)
k $CW e                  # examine: "Examine where?" (or the dog directly when it is the only choice)
sleep 2
k $CW $key               # the direction of the dog (harmless in the menu)
sleep 3
shot pet1                # the pet menu
k $CW s                  # Swap positions
sleep 4
shot pet2
sent=$(grep -h "sending ui action 'examine' with" $CL | tail -1)
echo "client: $(echo "$sent" | strip)" >> $R
n=$(echo "$sent" | sed -n "s/.*with \([0-9]*\) answers.*/\1/p")
after=$(grep -c "sending ui action 'examine'" $CL)
if [ "$after" -gt "$before" ] && [ "${n:-0}" -ge 2 ]; then
  echo "client menu: yes (the examine sent $n answers: the direction and the choice in the pet menu)" >> $R
else
  echo "MISSING: the client sent no examine with the pet menu choice (answers=${n:-none}); the dog is not a pet on the client or the keys missed: see pet1.png" >> $R
  ok=0
fi
if waitlog $HL "ui action 'examine' of .* replayed" 15; then
  echo "host: $(grep -h "ui action 'examine' of" $HL | tail -1 | strip)" >> $R
else
  echo "MISSING: no 'ui action examine ... replayed' on the host (it may say 'failed (ran out of answers)')" >> $R
  grep -h "ui action 'examine' of" $HL | tail -1 | strip >> $R
  ok=0
fi
sleep 6
echo "host (pet after): $(grep -h 'Multiplayer: test pet report' $HL | tail -1 | strip)" >> $R
echo "host: the offset of the dog from the body flips sign when the swap worked (a swap can also fail by chance: 'You fail to budge')" >> $R
echo "client 'not available' lines: $(grep -c 'client action not available' $CL)" >> $R
echo "host errors: $(grep -c ' ERROR ' $HL)" >> $R
echo "screenshots: pet0 (before) pet1 (pet menu open) pet2 (after s) in $S" >> $R
pkill -9 cataclysm-tiles
finish PET $R $ok
