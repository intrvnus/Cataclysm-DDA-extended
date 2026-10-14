#!/bin/bash
# Host + client under Xvfb, one CATA_MP_TEST_SCENE scene (see src/mp_test_npc.h), keys driven with
# xdotool, verdict from the host log.   Usage: bash $S/scene.sh <disarm|steal|useitem|armor|console|recraft|workout>
# Result: $S/<NAME>_RESULT.txt   (DISARM, STEAL, USEITEM, ARMORNPC, CONSOLE, RECRAFT, WORKOUT), shots $S/<name>_*.png
# Env: SKIP_START=1  reuse a running pair (it must have been started with the same CATA_MP_TEST_SCENE)
#      KEEP=1        leave the game running at the end
# Note: it kills every cataclysm-tiles on the machine when it starts (like pair.sh).
S=${MP_S:-/tmp/mp_test}
SCENE=$1
case "$SCENE" in
  disarm|steal|useitem|console|recraft|workout) NAME=$(echo $SCENE | tr a-z A-Z) ;;
  armor) NAME=ARMORNPC ;;
  *) echo "usage: $0 <disarm|steal|useitem|armor|console|recraft|workout>"; exit 2 ;;
esac
source $S/xt.sh
R=$S/${NAME}_RESULT.txt
HL=$S/ud_host/config/debug.log
CL=$S/ud_client/config/debug.log
KB=$S/ud_client/config/keybindings.json
KB_SAVED=0
: > $R
say() { echo "$*" >> $R; }
cleanup() {
  [ -z "$KEEP" ] && pkill -9 cataclysm-tiles
  if [ "$KB_SAVED" = 1 ]; then mv -f $KB.scene_bak $KB; elif [ "$KB_SAVED" = 2 ]; then rm -f $KB; fi
}
trap cleanup EXIT

# host log lines (without the time stamp) from line $1 on that match $2
hl() { tail -n +$(( $1 + 1 )) $HL | grep -a -- "$2" | sed 's/^[0-9:.]* [A-Z]* *: //'; }
hl_lines() { wc -l < $HL; }
# newest value of "key=" in the newest host line matching $1
last_state() { grep -a -- "$1" $HL | tail -1 | sed 's/^[0-9:.]* [A-Z]* *: //'; }
field() { echo "$1" | sed -n "s/.*[ ]$2=\([^ ]*\).*/\1/p"; }

# ---------------------------------------------------------------- start
if [ -z "$SKIP_START" ]; then
  pkill -9 cataclysm-tiles; sleep 3
  rm -rf $S/ud_host/save/Leeds && cp -a $S/ud_client/save/Leeds $S/ud_host/save/Leeds
  : > $HL; : > $CL
  if [ "$SCENE" = workout ]; then
    # the workout action has no key by default: F9 on the client
    if [ -f $KB ]; then cp $KB $KB.scene_bak; KB_SAVED=1; else KB_SAVED=2; fi
    echo '[ { "type": "keybinding", "id": "workout", "category": "DEFAULTMODE", "name": "Workout", "bindings": [ { "input_method": "keyboard_code", "key": "f9" } ] } ]' > $KB
  fi
  CATA_MP_TEST_SCENE=$SCENE NO_STAIRS=1 bash $S/pair.sh > $S/pair.out 2>&1
fi
source $S/win.env
if [ -z "$CW" ] || ! pgrep -x cataclysm-tiles > /dev/null; then
  say "FAIL: game/client window not running (CW='$CW')"; exit 1
fi
xdotool windowfocus $CW; sleep 1

# the hook sets the scene up ~10 s after the player joined
READY=""
for i in $(seq 1 75); do
  READY=$(grep -a "Multiplayer: test $SCENE ready" $HL | tail -1)
  [ -n "$READY" ] && break
  sleep 2
done
if [ -z "$READY" ]; then
  say "FAIL: the host never logged 'test $SCENE ready'"
  grep -a "Multiplayer: test\|Multiplayer: no \| ERROR " $HL | tail -8 | sed 's/^[0-9:.]* //' >> $R
  shot ${SCENE}_noready; exit 1
fi
sleep 8   # the client gets the changed character / NPC
DIR=$(field "$(last_state "Multiplayer: test $SCENE .*dir=")" dir)
say "scene $SCENE: $(echo "$READY" | sed 's/^[0-9:.]* [A-Z]* *: //')"
say "direction from the body: ${DIR:-unknown}"
shot ${SCENE}_0
BASE=$(hl_lines)
EBASE=$(grep -ac ' ERROR ' $HL)

# ---------------------------------------------------------------- helpers for the scenes
# presses the keys of one attempt at an NPC action; $1 = pattern of the host line that shows it
# arrived, the rest: key sequence ("sleep:N" waits)
press() { for key in "$@"; do case "$key" in sleep:*) sleep ${key#sleep:} ;; *) k $CW $key; sleep 1.2 ;; esac; done; }
replay_verdict() {  # replay_verdict <what>: PASS/FAIL text from the host lines since BASE
  local what=$1
  local ok=$(hl $BASE "ui action '$what'.*replayed" | wc -l)
  local bad=$(hl $BASE "ui action '$what'.*failed" | wc -l)
  hl $BASE "ui action '$what'" | head -5 >> $R
  echo "$ok $bad"
}
finish() {  # finish <ok> <message>
  local errs=$(( $(grep -ac ' ERROR ' $HL) - EBASE ))
  say "host ERROR lines since the scene: $errs"
  grep -a ' ERROR ' $HL | tail -n +$((EBASE + 1)) | head -3 | cut -c1-200 >> $R
  say "client 'sending ui action' lines:"
  grep -a "sending ui action" $CL | tail -3 | sed 's/^[0-9:.]* [A-Z]* *: /  /' >> $R
  say "client 'not available' lines: $(grep -ac 'not available' $CL)"
  if [ "$1" = 1 ] && [ "$errs" -eq 0 ]; then say "PASS: $2"; else say "FAIL: $2 (host errors: $errs)"; fi
  say "screenshots: $S/${SCENE}_*.png"
}

# ---------------------------------------------------------------- the scenes
case $SCENE in
  disarm|steal|useitem)
    PRE=$(last_state "Multiplayer: test $SCENE state:")
    say "state before: ${PRE:-none yet}"
    ok=0
    for n in 1 2 3; do
      before=$(grep -ac "ui action 'npc_act'" $HL)
      k $CW space; sleep 1
      DIR=$(field "$(last_state "Multiplayer: test $SCENE .*dir=")" dir); [ -z "$DIR" -o "$DIR" = none ] && DIR=Up
      case $SCENE in
        disarm)  press $DIR sleep:3 d sleep:2 y sleep:1 y sleep:2 ;;
        steal)   press $DIR sleep:3 shift+s sleep:2 y sleep:1 y sleep:2 Return sleep:2 ;;
        useitem) press $DIR sleep:3 i sleep:2 Return sleep:2 Return sleep:2 Return ;;
      esac
      shot ${SCENE}_try${n}
      sleep 8
      after=$(grep -ac "ui action 'npc_act'" $HL)
      if [ "$after" -gt "$before" ]; then say "attempt $n: the host did 'npc_act'"; ok=1; break; fi
      say "attempt $n: no 'npc_act' on the host (menu not open? see ${SCENE}_try${n}.png)"
    done
    sleep 6
    shot ${SCENE}_after
    POST=$(last_state "Multiplayer: test $SCENE state:")
    say "state after:  ${POST:-none}"
    read okc badc <<< "$(replay_verdict npc_act)"
    say "replayed: $okc, failed: $badc"
    case $SCENE in
      useitem)
        b0=$(echo "$PRE" | sed -n 's/.*bandages body=\([0-9]*\).*/\1/p'); b1=$(echo "$POST" | sed -n 's/.*bandages body=\([0-9]*\).*/\1/p')
        say "bandages on the body: ${b0:-?} -> ${b1:-?}"
        if [ "$okc" -ge 1 ] && [ "$badc" -eq 0 ] && [ -n "$b0" ] && [ -n "$b1" ] && [ "$b1" -lt "$b0" ]; then good=1; else good=0; fi
        finish $good "use item on an NPC: replayed=$okc failed=$badc, bandage used=$([ "$good" = 1 ] && echo yes || echo no)" ;;
      steal)
        say "(stealing is a dice roll: the pack may stay as it is; the replay is what is tested)"
        [ "$okc" -ge 1 ] && [ "$badc" -eq 0 ] && good=1 || good=0
        finish $good "steal from an NPC: replayed=$okc failed=$badc" ;;
      disarm)
        say "(disarming is a dice roll: the weapon may stay in the NPC's hands; the replay is what is tested)"
        [ "$okc" -ge 1 ] && [ "$badc" -eq 0 ] && good=1 || good=0
        finish $good "disarm an NPC: replayed=$okc failed=$badc" ;;
    esac ;;

  armor)
    say "worn order before: $(grep -a 'test npc worn order' $HL | tail -1 | sed 's/^[0-9:.]* [A-Z]* *: //')"
    ok=0
    for n in 1 2 3; do
      before=$(grep -ac "ui action 'npc_sort_armor'" $HL)
      k $CW space; sleep 1
      DIR=$(field "$(last_state "Multiplayer: test armor .*dir=")" dir); [ -z "$DIR" -o "$DIR" = none ] && DIR=Up
      press $DIR sleep:3 r sleep:3
      shot armor_try${n}_ui
      # in the sort armor window: pick the item under the cursor, move the cursor, put it there
      press Down Return Down Down Return sleep:1
      shot armor_try${n}_moved
      press Escape sleep:6
      after=$(grep -ac "ui action 'npc_sort_armor'" $HL)
      if [ "$after" -gt "$before" ]; then say "attempt $n: the host did 'npc_sort_armor'"; ok=1; break; fi
      say "attempt $n: no 'npc_sort_armor' on the host (see armor_try${n}_ui.png)"
    done
    sleep 8
    shot armor_after
    say "distinct worn orders on the host:"
    grep -a "test npc worn order" $HL | sed 's/^[0-9:.]* [A-Z]* *: //' | uniq -c >> $R
    orders=$(grep -a "test npc worn order" $HL | sed 's/^[0-9:.]* [A-Z]* *: //' | uniq | wc -l)
    read okc badc <<< "$(replay_verdict npc_sort_armor)"
    say "replayed: $okc, failed: $badc, distinct orders: $orders"
    if [ "$okc" -ge 1 ] && [ "$badc" -eq 0 ] && [ "$orders" -ge 2 ]; then good=1; else good=0; fi
    [ "$okc" -ge 1 ] && [ "$orders" -lt 2 ] && say "(replayed, but the order did not change: the sort-window keys Down/Return/Down/Down/Return may not have moved a layer: look at armor_try*_moved.png)"
    finish $good "sort armor of a friendly NPC: replayed=$okc failed=$badc, order changed=$([ "$orders" -ge 2 ] && echo yes || echo no)" ;;

  console)
    # examine (e), the direction of the console, then the terminal: login, first entry, any key, leave
    ok=0
    for n in 1 2; do
      before=$(grep -ac "ui action 'examine'" $HL)
      k $CW space; sleep 1
      DIR=$(field "$(last_state "Multiplayer: test console ready")" dir); [ -z "$DIR" -o "$DIR" = none ] && DIR=Right
      press e sleep:2
      shot console_try${n}_aim
      press $DIR sleep:3
      shot console_try${n}_login
      press Return sleep:2           # "Login successful.  Press any key"
      shot console_try${n}_menu
      press Return sleep:2           # the first entry
      shot console_try${n}_entry
      press Return sleep:2           # "Press any key to continue"
      press Escape sleep:2           # leave the root menu
      shot console_try${n}_left
      sleep 6
      after=$(grep -ac "ui action 'examine'" $HL)
      if [ "$after" -gt "$before" ]; then say "attempt $n: the host did 'examine'"; ok=1; break; fi
      say "attempt $n: no 'examine' on the host (see console_try${n}_*.png)"
    done
    read okc badc <<< "$(replay_verdict examine)"
    say "replayed: $okc, failed: $badc"
    say "console state: $(last_state 'Multiplayer: test console state')"
    say "client said: $(grep -a "doesn't display anything coherent\|display anything" $CL | tail -1)"
    if [ "$okc" -ge 1 ] && [ "$badc" -eq 0 ]; then good=1; else good=0; fi
    finish $good "examine a console (game::use_computer for a body): replayed=$okc failed=$badc" ;;

  recraft)
    # '-' repeats the last craft: the host starts it again (rolling paper, 1 minute), twice
    sleep 4
    say "state before: $(last_state 'Multiplayer: test recraft state')"
    started=0
    for round in 1 2; do
      b=$(hl_lines)
      for n in 1 2 3; do
        k $CW space; sleep 1
        k $CW minus; sleep 5
        shot recraft_r${round}_$n
        if [ "$(hl $b 'craft of rolling_paper' | wc -l)" -ge 1 ]; then break; fi
      done
      hl $b "craft of" | head -2 >> $R
      if [ "$(hl $b 'craft of rolling_paper' | wc -l)" -ge 1 ]; then started=$((started + 1)); else say "round $round: no craft started"; break; fi
      # wait for the craft to end (activity none again) before the next repeat
      for i in $(seq 1 60); do
        st=$(last_state 'Multiplayer: test recraft state')
        echo "$st" | grep -q "activity=none" && [ $i -gt 3 ] && break
        sleep 3
      done
      say "round $round end: $(last_state 'Multiplayer: test recraft state')"
    done
    seen_act=$(hl $BASE "recraft state: activity=ACT_CRAFT" | wc -l)
    say "state lines with activity ACT_CRAFT: $seen_act"
    say "paper / rolling paper at the end: $(last_state 'Multiplayer: test recraft state')"
    if [ "$started" -ge 1 ] && [ "$seen_act" -ge 1 ]; then good=1; else good=0; fi
    finish $good "repeat last craft: crafts started by the key=$started, craft activity seen=$seen_act" ;;

  workout)
    # F9 (bound by this script on the client): intensity menu -> Light ('l'), minutes -> 1
    ok=0
    for n in 1 2 3; do
      before=$(grep -ac "ui action 'workout'" $HL)
      k $CW space; sleep 1
      press F9 sleep:3
      shot workout_try${n}_menu
      press l sleep:2
      shot workout_try${n}_minutes
      xdotool type --window $CW --delay 150 "1"; sleep 1
      press Return sleep:3
      shot workout_try${n}_done
      sleep 5
      after=$(grep -ac "ui action 'workout'" $HL)
      if [ "$after" -gt "$before" ]; then say "attempt $n: the host did 'workout'"; ok=1; break; fi
      say "attempt $n: no 'workout' on the host (see workout_try${n}_*.png)"
    done
    sleep 6
    read okc badc <<< "$(replay_verdict workout)"
    say "replayed: $okc, failed: $badc"
    act=$(hl $BASE "workout state: activity=ACT_WORKOUT" | tail -1)
    say "host activity: ${act:-none seen}"
    grep -a "sending ui action 'workout'" $CL | tail -1 | sed 's/^[0-9:.]* [A-Z]* *: /client: /' >> $R
    if [ "$okc" -ge 1 ] && [ "$badc" -eq 0 ] && [ -n "$act" ]; then good=1; else good=0; fi
    finish $good "workout: replayed=$okc failed=$badc, host started a workout activity=$([ -n "$act" ] && echo yes || echo no)" ;;
esac
shot ${SCENE}_end
echo "${NAME} DONE" >> $R
