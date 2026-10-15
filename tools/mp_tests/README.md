# Multiplayer test scripts

Log-based in-game tests for the multiplayer fork. They drive two real game windows (host + client)
under Xvfb with xdotool and decide pass/fail from `config/debug.log` lines (`Multiplayer: ...`).
They were written in a scratch directory and copied here unchanged apart from the path.

## Setup
1. `export MP_S=/some/scratch/dir` (default `/tmp/mp_test`), copy this folder's files into it
   (`cp tools/mp_tests/* $MP_S/`). Every script sources `$MP_S/xt.sh`.
2. Needs `Xvfb`, `xdotool`, ImageMagick `import`, a TILES build in the repo root (`cataclysm-tiles`).
   `xt.sh` starts Xvfb :99 if it is missing.
3. Two user dirs: `$MP_S/ud_host/` and `$MP_S/ud_client/`, each with a world called `Leeds`
   (create the world once by hand; the client's character is created on first join). Tests copy
   the client's world over the host's (see `start_fresh` in `mp_menu_lib.sh`).
4. Building: `bld.sh` copies `src/` into a build copy `/home/user/cdda-build` (a full clone of the
   repo that was built once), builds there and promotes the binary with `mv -f` (so the main tree
   can be edited meanwhile). `utbuild.sh`/`unit.sh` do the same for `tests/cata_test` in
   `/home/user/cdda-test`.

## Scripts (what they test)
| Script | What |
|---|---|
| `xt.sh`, `pair.sh`, `mp_menu_lib.sh` | helpers: start host/client, `k <win> keys`, screenshots, `waitlog`, `start_fresh` |
| `movetest.sh`, `movecheck.py`, `stam_trace.sh`, `stamina.sh` | movement harness (hold/tap/chord/release grace/opposing keys) and stamina numbers |
| `cornertest.sh`, `melee_sector_test.sh`, `meleecheck.py` | corner sliding; cursor-sector melee (`CATA_MP_TEST_CORNER`) |
| `freetest.sh` | free movement phase 2: CASE=door\|attack\|talk\|squeeze\|swim\|ledge\|vehicle; host hook `CATA_MP_TEST_FREE` (`src/mp_test_free.cpp`) builds a walled corridor with the obstacle, the script holds D (squeeze: W+D) and decides from `test free state` lines; result in `$S/FREE_RESULT_<case>.txt` |
| `bubble.sh`, `two.sh`, `cartest.sh`, `stairs.sh`, `reconnect*.sh` | reality bubbles: far, overlap, car, two clients, stairs, reconnect |
| `zones_test.sh`, `pet_test.sh`, `factions_test.sh`, `mapmem_test.sh` | zones, pets, factions window, map memory |
| `scene.sh`, `scenes_all.sh` | NPC actions: disarm, steal, useitem, armor, console, recraft, workout (`CATA_MP_TEST_SCENE=`) |
| `inv_test.sh`, `grab.sh`, `smash_test.sh`, `talk_test.sh`, `chat.sh`, `apply_test.sh`, `armor.sh`, ... | menu round trips |
| `unit.sh`, `unitall.sh` | `tests/cata_test` with tags [vehicle] [monster] [npc] [construction] [iuse_actor] [melee] [mission] [faction] |

Test hooks live in the game as env vars `CATA_MP_TEST_*` (see `src/mp_test_*.cpp`, `net_server.cpp`).

## Lessons
- Never `pkill -f <pattern>` with a pattern that appears in your own command line (kills the shell, exit 144).
- Start long jobs with `(setsid nohup cmd < /dev/null > log 2>&1 &)`.
- Escape twice makes the client leave the game; use `space` to close things in scripts.
- The container restarts when the session idles: the scratch dir and Xvfb may vanish.
- Quote the exact log lines when reporting a result. Check a first "pass"/"fail" against the logs.
- If the build copy was first built with other flags than `bld.sh` uses (e.g. without `BACKTRACE=0`),
  ccache can hand back the old precompiled header and every file fails with
  "main-pch.hpp.gch: not used because `BACKTRACE' not defined". Fix: delete `pch/main-pch.hpp.gch`
  and build it once with `CCACHE=0` and the same flags as `bld.sh`.
- Making a test world: start the game with a fresh `--userdir`, choose New Game, "Play Now!
  (Default Scenario)", then Escape, `f` (save and quit); rename `save/<world>` to `save/Leeds` and
  copy the user dir to `ud_client` and `ud_host`. Starting with `--world Leeds` when that world does
  not exist shows only a popup (with some old option files a black window).
