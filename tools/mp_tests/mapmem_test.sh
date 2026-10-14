#!/bin/bash
# Map memory of a client across two visits.  Run A: the client's saved map memory is emptied, it joins,
# walks about (so it sees and memorizes terrain) and leaves (Escape, y: net_client.cpp saves the memory).
# Run B: host + client start again; the client's hook (CATA_MP_TEST_MAPMEM) counts the tiles that are
# in its map memory around it but out of sight.  They can only come from the saved memory, so a
# count clearly above run A's first count proves that the memory of the last visit was loaded back.
# Also shows whether "Loaded mm_region" (memory read from disk) is logged on the client in run B.
# Your own client map memory is backed up first and put back at the end (KEEP_MEM=1: not put back).
# Run: bash $S/mapmem_test.sh     Result: $S/MAPMEM_RESULT.txt (screenshots mem_a0, mem_a1, mem_b0, mem_b1)
S=${MP_S:-/tmp/mp_test}
source $S/mp_menu_lib.sh
R=$S/MAPMEM_RESULT.txt
: > $R
SAVE=$S/ud_client/save/Leeds
BAK=$S/mapmem_bak_client
ok=1

# hidden NUMBER of a "test mapmem" line
hidden_of() { sed -n 's/.*out of sight \([0-9]*\).*/\1/p'; }
remembered_of() { sed -n 's/.*remembered \([0-9]*\) tiles.*/\1/p'; }
# max of a list of numbers on stdin
maxof() { sort -n | tail -1; }

rm -rf $BAK && mkdir -p $BAK
find $SAVE -maxdepth 1 -name '*.mm1' -exec cp -a {} $BAK/ \;
find $SAVE -maxdepth 1 -name '*.mm1' -exec rm -rf {} +
echo "backup of the client's map memory: $(ls $BAK | wc -l) dirs in $BAK; emptied for run A" >> $R

# ---- run A: first visit ----
start_fresh CATA_MP_TEST_MAPMEM=1
waitlog $CL "Multiplayer: test mapmem" 60
sleep 5
shot mem_a0
focus_client
xdotool keyup w a s d 2>/dev/null
xdotool keydown d sleep 8 keyup d
sleep 1
xdotool keydown s sleep 8 keyup s
sleep 1
xdotool keydown d sleep 8 keyup d
sleep 6
shot mem_a1
cp $CL $S/mapmem_A_client.log
A_first=$(grep -h "Multiplayer: test mapmem" $CL | head -3 | hidden_of | maxof)
A_end=$(grep -h "Multiplayer: test mapmem" $CL | tail -2 | hidden_of | maxof)
echo "run A (fresh memory): out of sight at the start (first 3 reports) = ${A_first:-none}, after walking = ${A_end:-none}" >> $R
grep -h "Multiplayer: test mapmem" $CL | head -2 | strip | sed 's/^/  A first: /' >> $R
grep -h "Multiplayer: test mapmem" $CL | tail -1 | strip | sed 's/^/  A last:  /' >> $R
k $CW Escape                 # "Leave the multiplayer game?"
sleep 2
k $CW y
sleep 10
if grep -q "\[SAVE\] Done" $CL; then
  echo "client saved its map memory on leaving: yes ($(grep -c '\[SAVE\] Saving memory map' $CL) save, $(grep -h 'N submaps after save' $CL | tail -1 | strip))" >> $R
else
  echo "client saved its map memory on leaving: NOT SEEN in the client log (no '[SAVE] Done'); check the leave keys in mem_a1.png" >> $R
  ok=0
fi
echo "memory files now: $(find $SAVE -maxdepth 3 -path '*.mm1*' -type f | wc -l) files under *.mm1" >> $R
pkill -9 cataclysm-tiles
sleep 3

# ---- run B: second visit ----
start_fresh CATA_MP_TEST_MAPMEM=1
waitlog $CL "Multiplayer: test mapmem" 60
sleep 20
shot mem_b0
B_first=$(grep -h "Multiplayer: test mapmem" $CL | head -8 | hidden_of | maxof)
B_rem=$(grep -h "Multiplayer: test mapmem" $CL | head -8 | remembered_of | maxof)
echo "run B (saved memory): out of sight in the first 8 reports = ${B_first:-none} (remembered at most ${B_rem:-none})" >> $R
grep -h "Multiplayer: test mapmem" $CL | head -3 | strip | sed 's/^/  B first: /' >> $R
echo "client log run B: $(grep -c '\[LOAD\] Loading memory map' $CL) '[LOAD] Loading memory map' (game::load) and $(grep -c 'Loaded mm_region' $CL) 'Loaded mm_region' (regions read from disk, also lazily while drawing after the join)" >> $R
if [ "${B_first:-0}" -ge 10 ] && [ "${B_first:-0}" -gt $(( ${A_first:-0} + 5 )) ]; then
  echo "map memory loaded back on the next visit: YES (B ${B_first} out-of-sight remembered tiles vs A ${A_first:-0} at the start)" >> $R
else
  echo "map memory loaded back on the next visit: NO or not provable (B ${B_first:-none} vs A ${A_first:-none}; need B >= 10 and B > A + 5): see mem_b0.png vs mem_a0.png" >> $R
  ok=0
fi
echo "host errors: $(grep -c ' ERROR ' $HL), client errors: $(grep -c ' ERROR ' $CL)" >> $R
pkill -9 cataclysm-tiles

if [ -z "$KEEP_MEM" ]; then
  find $SAVE -maxdepth 1 -name '*.mm1' -exec rm -rf {} +
  find $BAK -maxdepth 1 -name '*.mm1' -exec cp -a {} $SAVE/ \;
  echo "client map memory put back from $BAK" >> $R
fi
finish MAPMEM $R $ok
