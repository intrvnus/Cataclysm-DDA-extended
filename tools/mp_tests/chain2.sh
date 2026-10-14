#!/bin/bash
S=${MP_S:-/tmp/mp_test}
until [ -f $S/CHAIN_DONE ]; do sleep 10; done
bash $S/cartest.sh > $S/cartest.out 2>&1
echo done > $S/CHAIN2_DONE
