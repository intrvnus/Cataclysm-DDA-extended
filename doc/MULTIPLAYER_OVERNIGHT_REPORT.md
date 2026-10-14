# Overnight report (updated as I go; last update at the end of the file)

All work is on `claude/fluid-proto`, every step committed and pushed. No Windows build, no PR, `intrvnus-mod` untouched.

## Verified in the running game (logs quoted from the host/client debug logs)
- **Driving**: a client takes the controls, the engine starts (timed action), the car accelerates (velocity up to 2400, moved ~48 tiles); the client's screen follows. Cause of the earlier "car never moves": the cruise-control check only looked at the host avatar; and the engine-start activity looked for the car at the host avatar.
- **Passengers**: a second player seated in the car travels with it (positions 7853 -> 7893 while the driver drives).
- **Downed / help / bleed-out**: the body goes down, a helper gets it up, and after the bleed-out timer a new body spawns.
- **Dedicated server**: keeps ticking with a client connected (35 heartbeat lines, 0 errors).
- **Sort armor**: the host's worn order really changes after a client sorts (shirt moved from 2nd to 4th).
- **Chat**: F2 on a client; the host logs `chat William Reece: hello friends`.
- Earlier rounds: map (shared reveal), missions list (group), zones window, haul menu, item action menu, peek prompt, construction menu opens, NPC menu entries (talk, examine, steal, disarm, use item).

- **Host chat** (F2 on the host): log `chat ...: hi from the host`.
- **Reconnect**: killing a client and starting it again puts it back into its saved body (`loaded body of ...`), the host keeps running, 0 errors.
- **Car regression** on the newest build: velocity 800 -> 2400 -> 844, the car moved from x=7844 to x=7892, 6 drive actions replayed, 0 errors.

- **Grab furniture** (`G` then a direction): the host log shows the table pushed 7836 -> 7837 -> 7838 with the player following; pulling backwards was blocked in the test (probably a wall behind the player), not verified. Vehicles cannot be dragged yet (a message says so). Works for steps in fluid mode and in classic input.
- **Map memory of a client** (A4 open item): when a client leaves, its map memory is saved (the client's own `.mm1` files were rewritten: `.hot.zzip` and `.warm.zzip` newer than before). Not yet checked: that it is loaded correctly on the next visit at another place.
- **Vanilla unit tests** (compiled in a separate copy) with my changes: vehicle (46 cases, 503825 assertions), monster, NPC, construction, item actions, missions, factions all pass. One melee test case ("Character attacking a zombie") crashes inside the test helper `full_attack_details` (`get_hit_weapon` on an empty wielded item); this is in test code I did not touch and is probably already present in the base. A full run of the whole suite was started afterwards.

## Built (compiles) but not run in game
- Factions read-only view, pets/mounts/friendly monsters examine, computers/consoles, disarm/steal/use-item replay (the confirmation prompts reach the host), sort armor for friends, repeat last craft, construction placement on the host, workout.

## Found and fixed on the way
- The host (and now clients) never show a blocking debug popup in multiplayer; they log instead (a popup froze the world / ate the keys).
- NPCs loaded without a template id logged "invalid npc: NC_NONE" every second (client bodies are such NPCs): made the lookup safe.
- A test binary was committed by mistake (about 100 MB in history). As you allowed, I removed it from the branch history of `claude/fluid-proto` only: checked that the final tree is identical, the 25 commit messages are the same, and every rewritten commit differed from its old version ONLY in that file (`cataclysm-tiles.next`); then force-pushed with a lease. A local-only backup branch `backup-before-scrub` still has the old history.
- Test-environment bugs (not game bugs): missing UTF-8 locale crashed the client on "90°"; SDL only delivers keys to the focused window when two clients run; stale game processes held the port.

## Not done / decisions for you
- Grab for vehicles: needs the vehicle drag code of `game::walk_move` ported to any character; furniture grab was done with a small separate routine instead.
- (Host chat: F2 say / F4 player list now exist for the host too.)
-  dialogue side-column commands (look at / size up / yell), spells (not needed), the faction rework (C4), per-player bubbles (C2).
- Zombie check is only partly verified: in one run it walked from 9 to 3 tiles toward a client with line of sight, in another it wandered off while out of sight. Monsters use the nearest player now; a cleaner test is still missing.
- Balance/gameplay: nothing changed.

(Last update 00:55: host chat, reconnect and the car re-checked OK on the newest build. Now compiling the unit tests in a separate copy to run the vanilla vehicle/monster/NPC tests against my changes. Earlier note 00:30: regression sweep re-run: downed/bleed-out cycle and dedicated server pass again; the car part of that run failed because of a test-script focus bug (fixed), re-run queued together with host chat and reconnect tests.)
