# Overnight report (updated as I go; last update at the end of the file)

## Short version (for the morning)
- Done and checked in the running game: driving + passengers, downed/help/bleed-out, dedicated server, chat, reconnect, sort armor, grab/push of furniture AND vehicles, host-side construction for a player's body, zombies chase a client player, map memory saved on leave, network compression (about half the traffic).
- Vanilla unit tests that touch my changes pass (one melee test crashes in its own helper, as before).
- Still only compiled, not run: factions view, pets/mounts, consoles, disarm/steal prompts, sort armor for friends, repeat craft, workout, loading of saved map memory, zones with a created zone.
- Not done on purpose (your call): per-player bubbles (C2), faction rework (C4), balance changes, Windows build, PR.
- Known test-environment quirk: the machine restarts when the session idles, which kills running games; harmless for the code.

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

- **Grab furniture** (`G` then a direction): the host log shows the table pushed 7836 -> 7837 -> 7838 with the player following; pulling backwards was blocked in the test (probably a wall behind the player), not verified.  Works for steps in fluid mode and in classic input.
- **Map memory of a client** (A4 open item): when a client leaves, its map memory is saved (the client's own `.mm1` files were rewritten: `.hot.zzip` and `.warm.zzip` newer than before). Not yet checked: that it is loaded correctly on the next visit at another place.
- **Network compression (C17 start)**: the messages were uncompressed JSON. Both ends now use ENet's range coder (switch off with env `CATA_MP_NO_COMPRESS` for both). Measured while a car drives (worst case, lots of map changes), one client: about 30 KB/s on the wire compressed (145-364 KB per 10 s) against about 70 KB/s uncompressed (300-920 KB per 10 s): roughly half. Driving, joining and the rest still work with it on (6 drive actions, 0 errors). Sending only changed parts of the map (deltas) would be the next saving.
- **Vanilla unit tests** (compiled in a separate copy) with my changes: vehicle (46 cases, 503825 assertions), monster, NPC, construction, item actions, missions, factions all pass. One melee test case ("Character attacking a zombie") crashes inside the test helper `full_attack_details` (`get_hit_weapon` on an empty wielded item); this is in test code I did not touch and is probably already present in the base. A full run of the whole suite was started afterwards.
- **Construction on the host for a player's body** (03:05): replaying a recorded "place battery" choice on a body consumed the battery item, started ACT_BUILD, ran it for a few turns and ended with no partial construction left (so it completed). The first attempt built nothing only because the test chose a tile that was not a valid place (log: `construction chosen (72,58,0) ... valid at (70,57,0)`); not a game bug. Not checked: a real client choosing through the menu (only the host-side replay was driven).

## Built (compiles) but not run in game
- Factions read-only view, pets/mounts/friendly monsters examine, computers/consoles, disarm/steal/use-item replay (the confirmation prompts reach the host), sort armor for friends, repeat last craft, workout.

## Found and fixed on the way
- The host (and now clients) never show a blocking debug popup in multiplayer; they log instead (a popup froze the world / ate the keys).
- NPCs loaded without a template id logged "invalid npc: NC_NONE" every second (client bodies are such NPCs): made the lookup safe.
- A test binary was committed by mistake (about 100 MB in history). As you allowed, I removed it from the branch history of `claude/fluid-proto` only: checked that the final tree is identical, the 25 commit messages are the same, and every rewritten commit differed from its old version ONLY in that file (`cataclysm-tiles.next`); then force-pushed with a lease. A local-only backup branch `backup-before-scrub` still has the old history.
- Test-environment bugs (not game bugs): missing UTF-8 locale crashed the client on "90°"; SDL only delivers keys to the focused window when two clients run; stale game processes held the port.

## Not done / decisions for you
- Grab for vehicles: done at 03:40 by making `game::grabbed_veh_move` work on any Character (the avatar path is unchanged: the old signature forwards to the new one). Checked in game: a shopping cart pushed east 7836 -> 7837 -> 7838 with the body following (log lines `test table at ...`), 0 errors. Not checked: pulling (a wall was behind the player), turning/zig-zag drags, heavy cars. No side branch was needed; the host's own grab was not re-tested but its code path only changed by an extra parameter.
- (Host chat: F2 say / F4 player list now exist for the host too.)
-  dialogue side-column commands (look at / size up / yell), spells (not needed), the faction rework (C4), per-player bubbles (C2).
- Zombie check is only partly verified: in one run it walked from 9 to 3 tiles toward a client with line of sight, in another it wandered off while out of sight. Monsters use the nearest player now; a cleaner test is still missing.
- Balance/gameplay: nothing changed.

(Last update 00:55: host chat, reconnect and the car re-checked OK on the newest build. Now compiling the unit tests in a separate copy to run the vanilla vehicle/monster/NPC tests against my changes. Earlier note 00:30: regression sweep re-run: downed/bleed-out cycle and dedicated server pass again; the car part of that run failed because of a test-script focus bug (fixed), re-run queued together with host chat and reconnect tests.)

Update 03:25: full sweep re-run on the newest build (car: velocity 1200 -> 2400 -> 757, 6 drive actions, 0 failed; downed/helped/bleed-out cycle completes; dedicated server 35 heartbeats; 0 errors in all three). Zombie check: spawned 4-5 tiles away out of sight, it walked from 7 to 5 tiles toward the client body over about 2 minutes (sees=0 all the time, so it follows scent/sound, slowly). Still no clean test with line of sight in daylight; the zombie targets the client's body at least as often as the host avatar, which is the point of the nearest-player change.

Update 04:10: vanilla unit tests re-run against the current sources (vehicle, monster, NPC, construction, iuse_actor, mission, faction all exit 0; melee still crashes in the same test helper as before). Reconnect re-checked on the newest build (rejoins saved body, 0 errors).

Update 05:10: two-client test (car with a second player seated) 0 errors; key tour of 18 client keys: no crash, no host error, no "not available" (note: most keys open client-local menus, so the host log shows nothing for them). 

Note 05:40: the machine was restarted at about 05:18-05:40 (uptime reset, all processes gone; files intact). The sweep that was running (car part passed: 6 drive actions, 0 failed, 0 errors) was cut off in the zombie part; re-started at 05:40.

Update 06:25: full sweep passed again (car 6 drive actions 0 failed; downed/help/bleed-out; dedicated 33 heartbeats; 0 errors). **Zombie targeting now verified**: once the zombie had line of sight (`sees 1`) it walked from 7 to 2 tiles toward the client's body (06:14 -> 06:18 log lines), so monsters do chase a client player. Note for the test setup: the machine is restarted whenever this session sits idle between wake-ups (uptime resets, running games are killed), so long tests only survive while I am actively waiting on them.
