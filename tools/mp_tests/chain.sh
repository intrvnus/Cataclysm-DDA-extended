#!/bin/bash
S=${MP_S:-/tmp/mp_test}
until grep -q "SWEEP DONE" $S/SWEEP_RESULT.txt 2>/dev/null; do sleep 10; done
bash $S/hostchat.sh > $S/hostchat.out 2>&1
bash $S/reconnect.sh > $S/reconnect.out 2>&1
echo CHAIN DONE > $S/CHAIN_DONE
