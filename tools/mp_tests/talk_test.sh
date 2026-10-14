#!/bin/bash
# dialogue test: pair must be running (pair.sh). Presses C on the client, shows the window,
# picks the first answer, shows the next turn.
source ${MP_S:-/tmp/mp_test}/xt.sh
source $S/win.env
k $CW c; sleep 4; shot talk1
k $CW Return; sleep 4; shot talk2
grep -h "Multiplayer\|ERROR" $S/ud_host/config/debug.log | tail -5 | cut -c1-160
