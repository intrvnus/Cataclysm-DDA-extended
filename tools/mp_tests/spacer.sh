#!/bin/bash
S=${MP_S:-/tmp/mp_test}
export DISPLAY=:99
while [ ! -s $S/client.pid ]; do sleep 1; done
P=$(cat $S/client.pid)
for i in $(seq 1 100); do
  W=$(xdotool search --pid $P 2>/dev/null | tail -1)
  [ -n "$W" ] && xdotool key --window $W space
  sleep 2
done
