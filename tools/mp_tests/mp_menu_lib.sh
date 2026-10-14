#!/bin/bash
# Shared helpers of zones_test.sh, pet_test.sh, factions_test.sh, mapmem_test.sh (source this).
S=${MP_S:-/tmp/mp_test}
source $S/xt.sh
HL=$S/ud_host/config/debug.log
CL=$S/ud_client/config/debug.log

# waitlog FILE REGEX SECONDS: returns 0 as soon as the regex is found in FILE, 1 after SECONDS
waitlog() {
  local i=0
  while [ $i -lt $3 ]; do
    grep -q -E "$2" "$1" && return 0
    sleep 1
    i=$((i+1))
  done
  return 1
}

# strip: a log line without the time and level, shortened
strip() { sed 's/^[0-9:.]* [A-Z]* *: //' | cut -c1-260; }

# start_fresh [ENV=VALUE ...]: kills the games, gives the host a copy of the client's world,
# empties both logs and starts host + client with the given environment (see pair.sh)
start_fresh() {
  pkill -9 cataclysm-tiles; sleep 3
  rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
  : > $HL
  : > $CL
  env "$@" NO_STAIRS=1 bash $S/pair.sh > $S/pair.out 2>&1
  source $S/win.env
}

# focus_client: the keys of xdotool go to the client window
focus_client() { xdotool windowfocus $CW; sleep 1; }

# finish NAME RESULTFILE ok: writes the verdict line at the top of RESULTFILE (evidence stays below)
finish() {
  local name=$1 file=$2 ok=$3
  local verdict=FAIL
  [ "$ok" = 1 ] && verdict=PASS
  { echo "$name $verdict"; cat "$file"; } > "$file.new"
  mv "$file.new" "$file"
  echo "$name DONE" >> "$file"
}
