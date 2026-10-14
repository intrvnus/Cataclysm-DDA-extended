#!/bin/bash
# presses many client keys one by one and logs what the host/client logged for each
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
R=$S/TOUR_RESULT.txt; : > $R
pkill -9 cataclysm-tiles; sleep 3
rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
: > $S/ud_host/config/debug.log; : > $S/ud_client/config/debug.log
NO_STAIRS=1 bash $S/pair.sh > $S/pair.out 2>&1
source $S/win.env
xdotool windowfocus $CW; sleep 2
for key in i shift+v shift+n m shift+m shift+x percent F4 shift+y backslash plus asterisk shift+o ampersand at numbersign x Return; do
  hb=$(grep -c ' ERROR \|failed' $S/ud_host/config/debug.log)
  cb=$(grep -c "not available" $S/ud_client/config/debug.log)
  hu=$(grep -c "ui action" $S/ud_host/config/debug.log)
  k $CW $key; sleep 3
  k $CW Escape; sleep 1
  k $CW Return; sleep 1
  ha=$(grep -c ' ERROR \|failed' $S/ud_host/config/debug.log)
  ca=$(grep -c "not available" $S/ud_client/config/debug.log)
  hua=$(grep -c "ui action" $S/ud_host/config/debug.log)
  echo "key $key: host ui actions +$((hua-hu)), host errors/failed +$((ha-hb)), client 'not available' +$((ca-cb)), client alive $(pgrep -c cataclysm-tiles)" >> $R
done
grep -h "not available" $S/ud_client/config/debug.log | sort | uniq -c >> $R
grep -h " ERROR \|failed" $S/ud_host/config/debug.log | cut -c1-200 | head -5 >> $R
pkill -9 cataclysm-tiles
echo "TOUR DONE" >> $R
