#!/bin/bash
# inventory screen: i, pick the first item, open its menu, wield it (w)
source ${MP_S:-/tmp/mp_test}/xt.sh
source $S/win.env
k $CW i; sleep 3; shot inv1; k $CW Return; sleep 3; shot inv2; k $CW w; sleep 6; shot inv3
grep -h "Multiplayer" $S/ud_host/config/debug.log | tail -3 | cut -c1-160
