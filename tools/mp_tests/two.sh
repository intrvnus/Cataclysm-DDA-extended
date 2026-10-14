#!/bin/bash
# Two clients: the first drives the test car, the second sits in it as a passenger. Results in TWO_RESULT.txt
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
R=$S/TWO_RESULT.txt
: > $R
pkill -9 cataclysm-tiles; sleep 3
rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
rm -rf $S/ud_client2/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_client2/save/Leeds
: > $S/ud_host/config/debug.log
(cd $G; export CATA_MP_TEST_NPC=1 CATA_MP_TEST_VEHICLE=1; ./cataclysm-tiles --userdir $S/ud_host/ --world Leeds --mp-host > $S/host.log 2>&1 & echo $! > $S/host.pid); sleep 90
(cd $G; ./cataclysm-tiles --userdir $S/ud_client/ --world Leeds --mp-connect 127.0.0.1 > $S/client.log 2>&1 & echo $! > $S/client.pid); sleep 100
(cd $G; ./cataclysm-tiles --userdir $S/ud_client2/ --world Leeds --mp-connect 127.0.0.1 > $S/client2.log 2>&1 & echo $! > $S/client2.pid); sleep 100
WINS=($(xwininfo -root -tree | grep -i cataclysm | awk '{print $1}')); W1=${WINS[1]}; W2=${WINS[0]}; echo "all windows: ${WINS[*]}" >> $R
echo "windows client1=$W1 client2=$W2" >> $R
xdotool windowfocus $W1; sleep 1; xdotool windowraise $W1; sleep 1; import -window $W1 $S/two_w1a.png; import -window $W2 $S/two_w2a.png; k $W1 asciicircum; sleep 18; import -window $W1 $S/two_w1b.png
for i in 1 2 3 4 5 6; do k $W1 Up; sleep 1; done
sleep 20
grep -h "test car placed\|test passenger\|no free seat\|no open ground\|joined\|left" $S/ud_host/config/debug.log | head -8 >> $R
echo "-- car + other player lines (every 4th):" >> $R
grep -h "test car at\|test other player" $S/ud_host/config/debug.log | awk 'NR%4==1' | tail -16 >> $R
echo "-- errors: $(grep -c ' ERROR ' $S/ud_host/config/debug.log)" >> $R
echo "-- client2 alive: $(pgrep -f 'ud_client2' | wc -l)" >> $R
pkill -9 cataclysm-tiles
echo "TWO DONE" >> $R
