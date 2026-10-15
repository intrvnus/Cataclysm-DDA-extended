#!/bin/bash
# helpers: source this. DISPLAY=:99 Xvfb must be running.
S=${MP_S:-/tmp/mp_test}
export DISPLAY=:99 SDL_AUDIODRIVER=dummy LANG=C.UTF-8 LC_ALL=C.UTF-8
G=/home/user/cataclysm-dda-extended
shot() { import -window root "$S/$1.png"; }
# send keys to a given window id (host/client are separate X windows, no WM => use --window)
k() { local w=$1; shift; xdotool key --window "$w" "$@"; }
winid() { xdotool search --pid "$1" 2>/dev/null | tail -1; }
start_host() { (cd $G; export CATA_MP_TEST_NPC=1; [ -z "$NO_STAIRS" ] && export CATA_MP_TEST_STAIRS=1; ./cataclysm-tiles --userdir $S/ud_host/ --world Leeds --mp-host-classic > $S/host.log 2>&1 & echo $! > $S/host.pid); }
start_client() { (cd $G; ./cataclysm-tiles --userdir $S/ud_client/ --world Leeds --mp-connect 127.0.0.1 > $S/client.log 2>&1 & echo $! > $S/client.pid); }

# make sure the virtual display exists (it can disappear when the machine restarts)
if ! pgrep -x Xvfb > /dev/null; then
  rm -f /tmp/.X11-unix/X99 /tmp/.X99-lock
  (Xvfb :99 -screen 0 1280x800x24 > ${MP_S:-/tmp/mp_test}/xvfb.log 2>&1 &)
  sleep 3
fi
