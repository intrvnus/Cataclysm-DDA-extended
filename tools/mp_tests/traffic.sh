#!/bin/bash
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
R=$S/TRAFFIC_RESULT.txt; : > $R
run() {
  label=$1
  pkill -9 cataclysm-tiles; sleep 3
  rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
  : > $S/ud_host/config/debug.log
  CATA_MP_TEST_BEAT=1 CATA_MP_TEST_VEHICLE=1 bash $S/pair.sh > $S/pair.out 2>&1
  source $S/win.env
  xdotool windowfocus $CW; sleep 2
  k $CW asciicircum; sleep 18
  for i in 1 2 3 4 5 6; do k $CW Up; sleep 1; done
  sleep 40
  echo "== $label" >> $R
  grep -h "Multiplayer: traffic" $S/ud_host/config/debug.log | sed 's/^[0-9:.]* INFO : //' | tail -12 >> $R
  echo "-- errors: $(grep -c ' ERROR ' $S/ud_host/config/debug.log) drive: $(grep -c "ui action 'drive'" $S/ud_host/config/debug.log)" >> $R
  pkill -9 cataclysm-tiles
}
run "compressed (default)"
export CATA_MP_NO_COMPRESS=1
run "NOT compressed"
echo "TRAFFIC DONE" >> $R
