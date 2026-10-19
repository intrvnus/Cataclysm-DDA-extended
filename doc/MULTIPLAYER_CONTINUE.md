# Continuing the multiplayer fork (2026-10-19)

## Decision

The main line is **intrvnus/Cataclysm-DDA-extended**, branch **`claude/fluid-proto`**. The from-scratch
Rust/TypeScript prototype **intrvnus/aftermath-mp is frozen** (last commits `51e0841` health/body
parts, `fac5d1a` working agreement). Do not resume it unless the goal changes to a browser game or
a clean-room engine.

Why: this fork already has the whole game (combat, monsters, NPCs, vehicles, crafting, mutations,
mods) and the hard multiplayer work (host/client, dedicated server, reconnect, per-player bodies,
bubbles, client menus). The remaining work is verification and polish, not reimplementation.

## State at handoff

- Branch `claude/fluid-proto`. The code is at `84c85d4` (2026-10-16), 355 commits ahead of
  `intrvnus-mod`; this handoff is the docs commit on top of it.
- Linux check passed for the handoff commit; the last Windows Tiles build of the code passed
  (runs `38960614418` / `38960612361`).
- A Windows build of that commit is unpacked at
  `A:\Games\CDDA_test\cdda-windows-build\game` (`cataclysm-tiles.exe`). Play from the main menu's
  Multiplayer entry, or:
  - host: `cataclysm-tiles.exe --world "<world>" --mp-host`
  - dedicated: `cataclysm-tiles.exe --world "<world>" --mp-server`
  - client: `cataclysm-tiles.exe --world "<any world with the same mods>" --mp-connect <host>:7777`
- Local clone: `A:\Games\CDDA_test\Cataclysm-DDA-extended` (shallow; `git fetch --unshallow` if
  upstream history or a merge is needed).
- Build tools: VS Build Tools 2022 with C++ is installed; CMake/Ninja/vcpkg are not set up yet.
  Use GitHub Actions for compile checks, or follow `doc/c++/COMPILING-VS-VCPKG.md` before local
  builds.
- Vanilla reference data: `A:\Games\CDDA_test\CDDA vanilla` (pinned `7b2efa5`).

## Next work, in order

1. **Verify the compiled-only batch; fix the scene scripts first.** Handoff section 6.2 lists the
   open items: NPC disarm/steal/use-item prompts, sort armor for a friend, computer consoles,
   repeat last craft, workout, pets/mounts, saved map memory, zone editing, melee neighbour
   tolerance. The scripts in `tools/mp_tests/` are the evidence. Fix the key sequences and expected
   lines first, then the real bugs they expose. Run them against a dedicated server
   (`pair_l.sh`-style), not a listening host.
2. **Fluid movement/combat phase 5 and feel.** Noah's hands-on items: ease-back, camera lead,
   attack slots, stagger, the zombie chase that was not reproducible, and eating/drinking as a
   client with real food.
3. **Bubble scale.** 3+ players, bubbles on different z-levels, vehicles crossing bubbles, saving
   with several far bubbles, and the "bubble timing per turn" log under load.
4. **Public-play hardening.** Server-side visibility/FOV filtering, rate limits, malformed packet
   handling, reconnect edge cases, and network deltas only if bandwidth demands it.
5. **Release engineering and docs.** A repeatable Windows artifact, versioning and log collection,
   host/join UX, and reconcile `MULTIPLAYER.md` (still says a ~60-tile limit) with the roadmap
   (says per-player bubbles are done). Update the STATUS table whenever something is actually run
   in game.

Do not start new systems before the compiled-only list is either green or explicitly deferred.

## Working rules

- Work on `claude/fluid-proto`; never push `intrvnus-mod`, no PR, no force push.
- No gameplay or balance changes without Noah; quality over quantity (see `CLAUDE.md`).
- Say how every change was verified; compile-only is not done.
- No Windows build unless Noah asks; the Linux check runs on pushes to `claude/**`.
- Keep `MULTIPLAYER_HANDOFF.md` section 0 and the roadmap STATUS current.

## First command

```powershell
cd A:\Games\CDDA_test\Cataclysm-DDA-extended
git status
# Read doc/MULTIPLAYER_HANDOFF.md section 6 and the roadmap STATUS.
Get-ChildItem tools\mp_tests
```
