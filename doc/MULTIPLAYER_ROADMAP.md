# Multiplayer roadmap (to-do list for discussion)

Status markers: **[N]** = Noah's item, **[C]** = added by Claude. Each item has a short
proposal and the decisions Noah needs to make. Nothing here is implemented yet unless noted.
Rule from now on: big batches of changes, checked by the Linux build/CI; Windows builds only
when a batch is worth playing.

## Decisions (Noah, 2026-10-13)

* Everything not commented below: implement as proposed. Order as in section D.
  No Windows builds until the whole batch is done; Linux build/CI checks every big change.
* A1: yes. Seasons keep the same number of days (SEASON_LENGTH unchanged); days are short,
  so everything (hunger, thirst, healing, evolution) moves at a playable pace.
* A2: the goal is a dedicated server.
* A3, A4: do it (A4 is important).
* A5: no windows at all: a short warning sound and a coloured message in the sidebar log.
* B2: regen slower than 5-10 s (stamina takes a while to run down); rest as proposed.
* B3: stations keep working when nobody is around.
* C1: implement. C2: every player gets their own reality bubble. C3: implement.
* C4: reworked faction menu: players can ally with each other and do missions as a faction.
* C5, C6: implement. C7: not a concern (everything is faster).
* C15: faction UI: only allied players see each other's health.

---

## A. Foundations (other items depend on these)

### A1 [N3] World clock: 1 day = 60 real minutes (Project Zomboid style)
* Today 1 game turn = 1 game second, and the realtime/fluid clock runs ~1 turn per 0.6 s.
  A 60-minute day means the calendar must run ~24x faster than movement/combat.
* Proposal: **two clocks**. *Action time* (movement, attacks, effects measured in turns, stun,
  aiming) stays as it is. *World time* (calendar, hunger/thirst/sleepiness, healing, rot,
  plant growth, weather, crafting/reading progress, monster evolution) advances by a
  `WORLD_TIME_SCALE` (default 24) per action turn.
* Clock shows minutes; the HUD can round to 10-minute steps like PZ.
* Decisions: day length (60 min default?), season length (option exists: 14-127 days; with
  1-hour days maybe 7 days per season), whether night length is scaled too.
* Risk: biggest engine change on the list. Every "per turn" vs "per game time" use must be
  sorted. Plan: introduce the scale, then audit systems one by one (needs, effects, items,
  monsters, weather, activities).

### A2 [C] Host menus must not freeze the world
* Today any menu on the host (inventory, crafting, look, popups) stops the simulation and the
  network, so every other player freezes. Already the #1 problem in the fluid prototype too.
* Proposal (short term): keep the world ticking from inside menu input loops (the prototype's
  option (a)). Long term: **dedicated server** (headless host, every human is a client), which
  also fixes "host dies -> everybody freezes".
* Decision: is a dedicated server the goal for the first public version?

### A3 [C] Remove the "one player" assumptions
* Monster/NPC AI and many systems use the single avatar (86 places in monster/NPC AI alone):
  who monsters chase, who NPCs talk to, scent (only the avatar leaves scent), noise, visibility,
  mission givers, factions, map memory, achievements.
* Proposal: a `players()` list and a "nearest/most interesting player" helper, then convert
  the AI, scent and noise first (otherwise zombies ignore clients).

### A4 [C] Real player characters, saving and reconnecting
* Today a client is a random NPC on the host and nothing is saved for clients.
* Proposal: the client's own character (made in character creation) is sent to the host on
  join; the host saves each player character in the world save; reconnect by name/key puts you
  back into your body. Map memory and overmap notes per player.
* **Done:** the client's character is sent on join; the host keeps each player's body in
  `<world>/mp_players/<key>.json` (the key lives in the client's `config/mp_identity.txt`);
  rejoining puts you back into your body where you left (if that spot is loaded, else next to
  the host); the client gets its full character every 3 s. Open: map memory / overmap notes
  per player.

### A5 [C] 353 yes/no popups and "Really do X?" prompts
* Every `query_yn` stops time on the host. Most must become non-blocking (default answer,
  toast message, or a confirm key that does not pause).

### A6 [C] Client UI (inventory, crafting, eat, wear, wield, examine, ...)
* The client can only move/attack/wait/pick up. Every menu has to run on the client and send
  the chosen action to the host. Needed before most of the items below are playable as a
  client. Also needed for mod features (music player etc., already on the list).
* **Done:** wield, wear, take off, eat/drink, read, drop, pick up (everything on the tile),
  crafting, open/close doors, look, character sheet, sleep, movement modes; apply only for
  uses that never ask anything (switching things on/off).
* **Done (record/replay, `src/remote_ui.*`, tested with Xvfb host + client):** apply with
  targets/choices (the lighter asks for a direction: the host uses it and the charge drops),
  examine terrain/furniture, choosing the tile and the items to pick up (`g`, `e`),
  advanced inventory (`/`: the client captures the item-move activities its menu starts and
  the host starts them on the body; only pickup/drop/move/insert/wear/wield/stash are accepted).
  The client does the action on its copy of the character and records every menu answer; the
  host repeats it on the body without opening a window.
* **Done (also tested):** bionics (`p`) and mutations (`[`): the client records which one it
  (de)activated, the host repeats it on the body. Actions that only start an activity run
  their menus on the client and the host starts the captured activity: unload (`U`),
  disassemble (`(`), butcher, mend, insert (`v`), reload (`r`). Views that need nothing
  from the host run locally: messages (`P`), morale, help.
* **Done and tested in game (Xvfb host + client):** smash (`s`), the inventory screen (`i`:
  its item menu sends wield/wear/eat/read/drop/apply/unload/reload/mend/disassemble/insert/
  favorite/change-side to the host; wield was tried), throwing in fluid mode (the throw key
  throws the wielded item at the mouse cursor; the classic fire key counts as the left mouse
  button), talking to NPCs (`C`) and trading: the host runs the conversation turn by turn, the
  client shows the normal dialogue window, the trading window opens on the client and the host
  carries out the agreed trade (a knife for a volleyball was tried). While a player talks they
  cannot be seen, targeted or hurt (portal storms etc.): this part is only code-checked, no
  hazard was tried. Mend only goes through the same capture path as disassemble (no mendable
  item was available). Classic (non-fluid) shooting and throwing are not available to clients:
  they need the mouse-aimed fluid mode. Test helper: with `CATA_MP_TEST_NPC` set the host
  spawns an NPC beside every player who joins.
* **Open:** examine creatures/vehicles/consoles (say "not available in multiplayer yet"),
  talking to NPCs and trading (C4), the inventory screen (`i`, its item menu has
  immediate effects), throw/fire/smash and the other remaining basic menus.

---

## B. Your list, refined

### B1 [N1] Safe mode: warn, never stop
* Replace the "safe mode stops the game" with: warning sound + message + flashing edge arrow
  pointing at the threat (per player, client side). Optional: auto-cancel the current timed
  action (setting).
* Same system serves B9 (distractions) and B10 (turret warnings).

### B2 [N2] Stamina that does not need minutes of standing still
* Ideas (from Dark Souls/PZ/Tarkov style games): short regen delay (~1-2 s) after spending,
  then fast regen (full bar in ~5-10 s walking, faster standing); sprint drains, walking is
  neutral, crouch/prone regen faster; cardio raises the max and the regen speed; heavy load and
  thirst/hunger slow regen. "Exhausted" state at 0 (cannot sprint until 30% back).
* Long-term tiredness moves to weariness/sleepiness (B11), not stamina.

### B3 [N4] Crafting for multiplayer: stations + minigame
* **Passive stations**: workbench/forge/kiln/chem lab etc. take a queue of recipes and work on
  their own (with fuel/power where it makes sense), progress shown on the station. The player
  can walk away.
* **Active boost**: standing at the station and playing a short minigame (timing bar / hold-
  release / sequence) speeds it up (e.g. x2-x4), skill makes the minigame easier and the
  passive speed higher.
* Hand crafting (no station) stays short and active.
* Decisions: which recipes need a station, crafting time scale with A1, can stations work
  while nobody is in the reality bubble.

### B4 [N5] Movement modes in the fluid system
* Shift held = sprint (drains stamina, B2), C = toggle crouch, Z = toggle prone (keys
  configurable). Walk/crouch/prone change speed, noise and visibility like now.
* Fits the fluid prototype's WASD layout (TAB is "toggle run" there; would become free).

### B5 [N6] Reading and martial arts training
* Reading: progress bar in world time (A1) plus a focus minigame (e.g. keep a marker in a
  moving zone, or pick key words) that speeds learning; interruptions do not lose progress.
  Bonus: group reading (one reads, nearby players learn slower) for co-op.
* Martial arts: practice sequences (input combos at a rhythm), success speeds learning; can
  also train against a dummy/sparring partner (another player!).

### B6 [N7] Sleep with several players
* Minecraft style: when all players (or a configurable %) sleep, time skips to morning; anyone
  awake cancels the skip for everyone. Otherwise a sleeping player sleeps in normal world time
  (with 1-hour days that is only a few real minutes).
* While sleeping: screen dims, you can still see the chat; damage/noise wakes you.

### B7 [N8] Timed actions short and cancellable
* All "quick" actions (pick up, eat, drink, wear, reload, open containers) shortened to
  ~0.3-2 s real time with a progress bar above the character. Cancel with movement keys,
  Esc or the cancel key; taking damage cancels optional per action.
* Long activities (butchering, first aid, disassembly, construction) become stations/minigames
  (B3) or short repeating steps.

### B8 [N9] Remove all "time stop" interruptions
* Every distraction (monster spotted, hostile in view, noise, pain, hunger...) that today asks
  "Stop doing X?" becomes a B1 warning. The action keeps going unless the player cancels.

### B9 [N10] Turrets and auto weapons: telegraphed attacks
* "Hostile detected" voice/beep + warning arrow, then an aim period (e.g. 1.5-2.5 s, shorter
  for military turrets) with a visible laser/aim line; leaving line of sight resets it.
  Damage stays as is (still deadly), only the instant-kill-on-sight goes away.
* [C] Same for other ranged monsters (spitters, soldier zombies, robots): short wind-up so
  fluid combat is fair. Also melee monsters get small wind-ups (grabs, big hits telegraphed)
  to match the fluid melee system.

### B10 [N11] Weariness
* Ideas: weariness only from heavy work (crafting, construction, carrying heavy loads,
  sprinting), not from walking; drops fast while resting at a camp/bed/campfire (not only
  sleep); a short "take a break" action (sit down 30-60 s) clears a chunk. Ties into A1
  so it is measured in world time.

---

## C. More issues found [C]

| # | Issue | Proposal |
|---|---|---|
| C1 | **Death and respawn** | Downed state with bleed-out timer, other players can revive; otherwise respawn (new character in the same world, or spectator). Host death must not end the server (A2). |
| C2 | Several bubbles | Players far apart: today the client is tied to the host's bubble. Later: one bubble per player group (expensive) or a hard tether distance. Decision needed. |
| C3 | Vehicles | Driving in realtime, several players in one vehicle (driver + passengers/gunners), entering/leaving. |
| C4 | NPCs and dialogue | Talking to NPCs opens menus (A6); missions/followers per player or shared faction. |
| C5 | Shared faction and ownership | All players one faction, so items in your friends' base are not "stolen". |
| C6 | Basecamps, zones, loot sorting | Shared or per player. |
| C7 | Monster evolution/spawning | Tied to days; with 1-hour days zombies evolve 24x faster in real time. Needs its own scale. |
| C8 | Item rot, plant growth, fermenting | Follow world time (fine), check balance with short days. |
| C9 | Healing, hunger, thirst, sleepiness rates | Follow world time; with 1-hour days you eat several times per real hour. Check feels right. |
| C10 | "Wait" and "rest" menus | Disabled in MP (or only "wait until everyone sleeps"). |
| C11 | Auto-travel, auto-move, auto-pickup | Work in realtime, cancel on danger warning. |
| C12 | Look/examine/aim modes | Do not freeze the world (A2); look mode on clients. |
| C13 | Sound and music per player | Sounds and music played on each client for its own position. |
| C14 | Weather, light and time on clients | Today the client uses its own world's weather/light. Sync from host. |
| C15 | Chat, player list, ping | Basic chat (Enter/T), list of players and their health. |
| C16 | Server rules/options | Who may use the debug menu, PvP on/off, friendly fire, difficulty options set by the server. |
| C17 | Network efficiency | Only send changed parts of submaps, compress (zstd is already in the game), send monsters as deltas. Needed before playing over the internet. |
| C18 | Throwing, spells, reach weapons, turrets you own | Not in the fluid prototype yet. |
| C19 | Bionics and mutations activation | Menus on the client (A6); activation costs in realtime. |
| C20 | Traps and stealth | Traps you placed are visible to friends; noise from one player aggroes for all. |
| C21 | Achievements, scores, memorial | Per player. |
| C22 | Fire, explosions, fields | Already work (host simulates); just need syncing visuals on clients. |
| C23 | Encumbrance/speed fairness | Fluid speed comes from move cost; check that heavy/slow characters are not unplayable in realtime. |

---

## D. Suggested order

1. Fluid prototype: build, link, run and fix (in progress).
2. A2 (world keeps running behind menus) + A5 (no blocking popups) + B1/B8 (warnings instead
   of stops). Without these nothing else feels right in realtime.
3. A3 (multi-player AI/scent/noise) + C1 (death/respawn).
4. A1 (world clock) + B2/B10 (stamina/weariness) + C7-C9 tuning.
5. B7 (short timed actions) + B4 (movement modes) + B9 (turrets/monster wind-ups).
6. A4 (real characters, saving) + A6 (client UI).
7. B3 (crafting stations/minigames) + B5 (reading/training) + B6 (sleep skip).
8. Dedicated server, network efficiency (C17), chat (C15), the rest of C.
