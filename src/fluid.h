#pragma once
#ifndef CATA_SRC_FLUID_H
#define CATA_SRC_FLUID_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "coordinates.h"
#include "point.h"

class Character;
class monster;
class Creature;
class JsonObject;
class JsonOut;

/**
 * Experimental fluid movement and real time combat. See doc/MULTIPLAYER_FLUID.md.
 *
 * The game logic stays on the tile grid.  What changes is *when* things happen:
 *  - A step is started when the actor is ready, not when a key event arrives.  The logical
 *    position changes at once (so every collision / reservation / combat rule keeps working),
 *    the sprite then slides from the old to the new tile while the actor is busy.  The busy
 *    time is the native move cost converted to milliseconds, so terrain, encumbrance, speed,
 *    move modes, diagonals ... keep their meaning.
 *  - Melee is a small state machine (wind-up -> strike -> recovery) whose total length is the
 *    native attack cost.  The hit is resolved by the native melee code when the wind-up ends.
 *  - Ranged: holding the aim button lets Character::recoil fall over real time with the native
 *    aim_per_move(), so skill and sights decide how fast the aim settles.  The fire button fires
 *    at the tile under the cursor with whatever steadiness there is.
 *  - World turns (monsters, weather, effects ...) run on a fixed real time clock.
 *
 * The same driver (fluid::drive) moves the host's avatar and the NPCs that stand for remote
 * players, so there is a single code path for everybody.
 */
class npc;

namespace fluid
{

using ms_t = int64_t;

// ---------------------------------------------------------------------------------------
// configuration

/** True if the experimental mode is on (option FLUID_MODE, tiles build only). */
bool enabled();
/** True if this process simulates the world (single player or multiplayer host). */
bool authority();
/** Monotonic clock in milliseconds. */
ms_t now_ms();
/** Length of one world turn in milliseconds (option FLUID_TURN_MS). */
int turn_ms();
/** Smooth camera on? */
bool smooth_camera();
/** Players move freely inside the tiles instead of in tile steps (option FLUID_FREE_MOVEMENT). */
bool free_movement();
/** True while a soft-wall push (see tuning::soft_wall_ms) runs the native move: the player
 * already confirmed entering the dangerous tile by pushing, so game::walk_move asks nothing. */
bool soft_wall_confirmed();

struct tuning {
    // length of one frame of the input wait loops
    int frame_ms = 16;
    // while movement is held, a step that is up to this late (ms) still starts when the
    // previous one ended (a slow world turn must not add a pause to walking)
    int chain_ms = 150;
    // ---- melee
    // share of the attack time spent winding up before the blow lands
    float windup_share = 0.40f;
    // bounds of the wind-up (ms); the blow of a very fast attack must still be readable
    int windup_min_ms = 80;
    int windup_max_ms = 700;
    // an attack never takes less than this (ms)
    int attack_min_ms = 200;
    // recovery is multiplied by this if the blow hit nothing (cheaper to miss a moved target)
    float whiff_recovery = 0.6f;
    // cancelling in the wind-up costs this share of the wind-up that was still left
    float cancel_cost = 0.5f;
    // a click this long before the actor is ready is remembered and fired when it is
    int buffer_ms = 300;
    // target assist: half angle of the cone around the cursor direction (cosine)
    float assist_cos = 0.5f;
    // sprite lunge, in tiles
    float lunge_back = 0.10f;
    float lunge_forward = 0.38f;
    // ---- ranged
    // share of the steadiness that is lost by a step taken while aiming
    float aim_step_loss = 0.35f;
    // moving this fast (running/sprinting) throws the aim away completely
    float aim_run_loss = 1.0f;
    // minimum time between two shots (ms), on top of the native gun time
    int fire_min_ms = 120;
    // time (ms) in which an unheld aim falls back to hip fire completely
    int aim_relax_ms = 1400;
    // share of the steadiness lost per radian the aim is swept across the screen
    float aim_sweep_loss = 0.6f;
    // a shot needs the aim button held at least this long when the weapon is not "ALWAYS_AIMED"
    int fire_hip_ms = 0;
    // upper bound of aim steps simulated in one frame (protects against a stalled game)
    int max_aim_moves_per_frame = 400;
    // ---- visuals
    // slides never take longer than this (ms)
    int slide_max_ms = 1500;
    // camera follows the visual position of the avatar
    bool camera_follows_slide = true;
    // ---- movement feel (WASD), see the movement rework spec in doc/MULTIPLAYER_ROADMAP.md
    // from standstill, wait this long for a second key so that a two-key diagonal forms (0 = off)
    int chord_ms = 40;
    // after one key of a held diagonal is released, the diagonal stays latched this long
    int release_grace_ms = 60;
    // a tap is sent for at least this long, so that a short press is never lost
    int tap_hold_ms = 100;
    // a blocked diagonal slides along the free cardinal component
    bool corner_slide = true;
    // a step the host did not allow eases back over this long instead of snapping
    int ease_back_ms = 100;
    // the melee click hits the tile of the 45 degree sector the cursor is in
    bool melee_sectors = true;
    // camera leads toward the cursor by this many tiles (0 = off).  Off (Noah 2026-10-16): the
    // view drifting with the mouse made the character look off-centre and jittery
    double camera_lead_tiles = 0.0;
    // ---- free (grid-free) movement, see doc/MULTIPLAYER_SMOOTH_MOVEMENT_PLAN.md
    // half the width of a player's body, in tiles (0.3 = a body 0.6 tiles wide)
    double body_half = 0.3;
    // walking straight past a corner that overlaps the body by less than this (tiles) nudges
    // the body around it instead of stopping it (door frames, gaps between cars)
    double corner_nudge = 0.4;
    // longest piece of movement checked for collisions at once (tiles)
    double max_substep = 0.2;
    // a frame never moves the body for longer than this (ms), e.g. after a hitch
    int max_move_frame_ms = 120;
    // dangerous tiles (fire, known traps, ledges) are soft walls: the body stops at their edge
    // and goes in after pushing on for this long (ms), instead of a "Really step into ...?" query
    int soft_wall_ms = 400;
};
const tuning &tune();
// the own character started a step (the input resolver then drops a latched tap)
void note_own_step_started();

// ---------------------------------------------------------------------------------------
// input

/** What the player (or a remote player) is doing right now. Absolute, device independent. */
struct input_frame {
    // movement intent, each -1..1
    int move_x = 0;
    int move_y = 0;
    // primary button (melee / fire) and secondary button (aim / steady), held state
    bool primary = false;
    bool secondary = false;
    // running counters of presses; the driver derives "pressed since last frame" from them,
    // so a click shorter than a packet interval is not lost
    int primary_presses = 0;
    int secondary_presses = 0;
    // presses of the throw key (throws the wielded item at the cursor)
    int throw_presses = 0;
    // presses of the shove key / middle mouse button
    int shove_presses = 0;
    // dodges asked for: a movement key tapped twice, a mouse side button or the dodge key
    int dodge_presses = 0;
    // client: this frame starts a dodge (set from dodge_presses, sent with the movement frame)
    bool dodge = false;
    // cancels a wind-up / drops the aim
    bool cancel = false;
    // held: sprint (B4)
    bool sprint = false;
    // cursor in world tiles; the integer part is the absolute tile, 0.5 its centre
    bool cursor_valid = false;
    double cursor_x = 0.0;
    double cursor_y = 0.0;
    int cursor_z = 0;
};

/** Gathers the keyboard / mouse state into a frame. False if there is no such device
 * (curses build, or nothing to poll). */
bool poll_device( input_frame &out );

/** True if this classic (turn based) action must be ignored because fluid mode owns the
 * input: movement keys, mouse buttons ... Used in game::handle_mouseview. */
bool swallows( const std::string &action );

// ---------------------------------------------------------------------------------------
// actors

enum class phase : int {
    idle = 0,
    windup,
    recovery
};

/** Per character state of the driver. */
struct actor_state {
    // the actor may start a new attack / fire / shove at this time
    ms_t ready_at = 0;
    // movement may start again at this time (tile steps, doors, pushes).  Melee and shooting do
    // not set it: the body keeps moving while it winds up, recovers or waits for the next shot.
    ms_t move_ready_at = 0;
    // movement is not allowed before this time (shove and throw commit the body).  Melee and
    // shooting leave it alone.
    ms_t root_until = 0;
    // movement frames of a player (fluid::move_frame): time the body still stands after a push
    // that opened a door, counted in the frames' durations (the client predicts the same frames)
    double frame_wait_ms = 0;
    // dodge (realtime combat): dash time left (frame time), blows miss until, next dodge from
    double dash_left_ms = 0;
    ms_t invuln_until = 0;
    ms_t dodge_ready_at = 0;
    // a dodge asked for just before it was ready: it goes as soon as it is (input buffer)
    ms_t dodge_buffered_until = 0;
    // sprint key state of the last frame (hold to sprint)
    bool prev_sprint = false;
    bool prev_cancel = false;
    // the last action was a step and movement is still held: the next step chains on
    bool stepping = false;
    // the autoattack key was pressed: swing at the best adjacent enemy once the actor is ready
    ms_t autoattack_until = 0;

    // ---- free movement: where the body really is (absolute tile coordinates, 0.5 = centre
    // of the tile).  The game sees the character on body_tile, the tile under the centre.
    bool body_valid = false;
    double body_x = 0.0;
    double body_y = 0.0;
    tripoint_abs_ms body_tile;
    ms_t body_last = 0;
    bool body_moving = false;
    // when movement was last asked for (aim gets worse while moving)
    ms_t moved_at = -100000;
    // a visual correction that fades out over corr_ms (moves the game made itself: a bump into
    // a squeeze, stairs next door ...), in tiles relative to the body
    double corr_x = 0.0;
    double corr_y = 0.0;
    ms_t corr_t0 = 0;
    int corr_ms = 0;
    // pushing into a soft wall (dangerous tile): since when, and in which direction
    ms_t soft_since = 0;
    ms_t soft_last = 0;
    int soft_dx = 0;
    int soft_dy = 0;

    // ---- melee
    phase melee = phase::idle;
    ms_t phase_end = 0;
    ms_t melee_started = 0;
    int melee_total_ms = 0;
    int melee_windup_ms = 0;
    // direction of the blow (unit steps, 8 directions)
    int strike_dx = 0;
    int strike_dy = 1;
    // the swing came from walking into somebody: it aims where the actor walked, not at the cursor
    bool bump_swing = false;
    // a click that came too early, remembered until this time
    ms_t buffered_until = 0;
    bool buffered_is_attack = false;
    bool strike_pending = false;
    // the blow already landed / whiffed in this swing
    bool struck = false;
    bool hit_something = false;

    // ---- facing, for sprites and for attacks without a cursor
    int face_dx = 0;
    int face_dy = 1;

    // ---- aim
    bool aiming = false;
    // a gun is wielded: the spread circle is shown even without the aim button
    bool holds_gun = false;
    ms_t aim_last = 0;
    // moves that fractionally were not simulated yet (aim runs in whole moves)
    double aim_move_carry = 0.0;
    // direction of the cursor at the last frame, to measure how far the aim was swept
    bool aim_angle_valid = false;
    double aim_angle = 0.0;
    // when the overlay was last sent to the owner of this state (host side)
    ms_t aim_sent = 0;
    // where the shot goes
    tripoint_abs_ms aim_tile;
    double aim_x = 0.0;
    double aim_y = 0.0;
    // steadiness 0 (hip fire) .. 1 (as steady as this character gets), for the overlay
    double steadiness = 0.0;
    double spread_tiles = 0.0;
    bool can_fire = false;

    // ---- edge detection of the last input frame
    int seen_primary_presses = 0;
    int seen_secondary_presses = 0;
    int seen_throw_presses = 0;
    int seen_shove_presses = 0;
    bool prev_primary = false;
    bool prev_secondary = false;
    bool initialised = false;
};

enum class fx_type : int {
    step = 0,   // an actor started a step
    swing,      // wind-up started
    strike,     // the blow landed (hit == true) or hit nothing
    cancel,     // wind-up cancelled
    shot,       // a shot was fired
    aim,        // aim state of a remote player (steadiness for its crosshair)
    tracer,     // the path of a shot, from -> to
    aimline,    // a monster takes aim: from -> to for dur_ms
    boom,       // an explosion at from, radius in dx
    flourish    // a combat effect (windup_ms = its kind, see flourish_kind) at from
};

/** Small combat effects drawn on the characters (realtime combat, Noah 2026-10-16): no HUD,
 * the feedback is where the eyes already are (as in Hades / Enter the Gungeon). */
enum class flourish_kind : int {
    dash = 0,   // a dodge: a streak in direction dx, dy from the start point
    evade,      // a blow missed the dodging player: a white ring
    stagger,    // a monster lost its blow: a yellow burst
    winded      // a dodge without the stamina for it: a grey puff
};
struct flourish_view {
    flourish_kind kind = flourish_kind::dash;
    // start point in absolute tiles (0.5 = tile centre), direction for the dash
    double x = 0.0;
    double y = 0.0;
    int z = 0;
    double dx = 0.0;
    double dy = 0.0;
    // 0 at the start .. 1 when gone
    double progress = 0.0;
    // the creature it belongs to stands on this tile now (rings and bursts follow it)
    tripoint_abs_ms on;
};
std::vector<flourish_view> flourishes();

/** Something that happened and that the other players must see. */
struct event {
    fx_type type = fx_type::step;
    // character id of the actor (as on the host)
    int actor = 0;
    tripoint_abs_ms from;
    tripoint_abs_ms to;
    // step: slide duration; swing: total attack time
    int dur_ms = 0;
    // swing: wind-up time
    int windup_ms = 0;
    // direction of a swing / shot
    int dx = 0;
    int dy = 0;
    bool hit = false;
    // aim: steadiness and spread in 1/1000
    int steadiness = 0;
    int spread = 0;
};

/** One frame of a player's movement as the client predicted it.  The host moves the body by
 * exactly these frames (direction and duration), so its position after frame @c seq is the one
 * the client predicted for it; only real differences (a refused door, a creature in the way)
 * are corrected on the client (deterministic input frames). */
struct move_frame {
    int seq = 0;
    int dt = 0;
    int move_x = 0;
    int move_y = 0;
    bool sprint = false;
    // the dodge (a dash in which monster blows miss) starts with this frame
    bool dodge = false;
};

/** Runs one frame for a character. New events are appended to @p out (may be null).
 * @p moves: a remote player's movement frames to apply this frame (free movement); null for
 * the host's own avatar and NPCs, which move by the frame time. */
void drive( Character &who, actor_state &st, const input_frame &in, ms_t now,
            std::vector<event> *out, const std::vector<move_frame> *moves = nullptr );

/** The autoattack key (Tab): the actor swings at the weakest hostile creature next to it as soon
 * as it is ready (fluid melee: wind-up and blow). */
void request_autoattack( actor_state &st );

/** State of the host's own avatar / of a remote player's NPC. */
actor_state &avatar_state();
actor_state &state_of( int character_id );
void forget_state( int character_id );
/** The body of a multiplayer player takes hold of the furniture in direction @p dir (or lets go
 * for the zero direction); dragging happens in the steps.  False if nothing was grabbed. */
bool body_grab( Character &who, const tripoint_rel_ms &dir );
/** One step of a player's body (classic input): takes held furniture along. */
bool body_step( npc &guy, const tripoint_bub_ms &dest );

// ---------------------------------------------------------------------------------------
// the loop

/** Host / single player: one frame of fluid mode. Call this on every iteration of the loops
 * that wait for input. Polls the device, drives the avatar and the remote players and
 * advances the animations. */
void host_frame();

/** Called at the start of every world turn: opens the window of the new turn. */
void begin_turn();
/** Called after the avatar's part of the turn: runs frames until the window is over. */
void finish_turn_window();
/** True once the current turn is over and the world should advance. */
bool turn_due();
/** Reset the clock (new game, load, session start). */
void reset_clock();
/** Forgets the last game's effects, prediction, camera and clock (after leaving a game). */
void reset_session();

/** Around monmove(): records where monsters and NPCs were, then slides them to where they
 * went. The host also sends those slides to the clients through net. */
void snapshot_before_monsters();
void snapshot_after_monsters();

// ---------------------------------------------------------------------------------------
// visuals (read by cata_tiles)

struct offset {
    double x = 0.0;
    double y = 0.0;
};

/** Registers a slide of whatever stands at @p to: it is drawn at @p from_x/@p from_y (absolute
 * tile coordinates, 0.5 = centre) at @p t0 and reaches @p to at @p t0 + @p dur. */
void begin_slide( const tripoint_abs_ms &to, double from_x, double from_y, ms_t t0, int dur );
/** Same, from a tile centre. */
void begin_slide( const tripoint_abs_ms &from, const tripoint_abs_ms &to, ms_t t0, int dur );
/** Registers a melee lunge of whatever stands at @p at. */
void begin_lunge( const tripoint_abs_ms &at, int dx, int dy, ms_t t0, int windup_ms, int total_ms );
/** Cancels a lunge. */
void end_lunge( const tripoint_abs_ms &at );
/** A blow connected: the attacker's lunge holds for a moment (hit-stop), the target flashes,
 * shakes and is drawn knocked back a little. */
void register_hit( const tripoint_abs_ms &attacker, const tripoint_abs_ms &target, int dx, int dy,
                   ms_t now );
/** A monster winding up a blow: the tile it will hit and how far the wind-up is (0..1). */
struct incoming_blow {
    tripoint_abs_ms from;
    tripoint_abs_ms tile;
    double progress = 0.0;
};
std::vector<incoming_blow> incoming_blows();
/** A projectile flew from @p from to @p to: drawn as a fading tracer (and sent to clients). */
void note_projectile( const tripoint_abs_ms &from, const tripoint_abs_ms &to );
struct tracer_view {
    tripoint_abs_ms from;
    tripoint_abs_ms to;
    // 1 when fired .. 0 when gone
    double strength = 0.0;
};
std::vector<tracer_view> tracers();
/** A monster takes aim at @p to (B9): a red aim line for @p dur_ms, also on the clients. */
void monster_aims( const tripoint_abs_ms &from, const tripoint_abs_ms &to, int dur_ms );
struct aim_line_view {
    tripoint_abs_ms from;
    tripoint_abs_ms to;
    // 0 when the aim starts .. 1 when it fires
    double progress = 0.0;
};
std::vector<aim_line_view> monster_aim_lines();
/** Ranged monster attacks on players (B9 [C]): the first call starts a visible aim of
 * ranged_windup_ms and returns false; once it has run out the attack may go (true).  Attacks on
 * NPCs and monsters are not delayed. */
bool ranged_windup_ready( monster &z, const std::string &attack );
/** An explosion (C22): drawn as an expanding flash without stopping the game (also on the
 * clients); replaces the blocking explosion animation in fluid mode. */
void note_explosion( const tripoint_abs_ms &at, int radius );
struct boom_view {
    tripoint_abs_ms at;
    int radius = 0;
    // 0 at the blast .. 1 when gone
    double progress = 0.0;
};
std::vector<boom_view> booms();
/** Creatures that were just hit, with the strength of their flash (1..0). */
std::vector<std::pair<tripoint_abs_ms, double>> hit_flashes();

/** Where the sprite of whatever is on @p p is drawn, relative to the tile centre, in tiles. */
offset sprite_offset( const tripoint_abs_ms &p );
/** Where the camera is relative to the avatar's tile, in tiles. */
offset camera_offset();
/** True if anything is moving (the screen must be redrawn every frame). */
bool animating();

struct aim_view {
    bool active = false;
    // crosshair, absolute tile coordinates (0.5 = centre)
    double x = 0.0;
    double y = 0.0;
    tripoint_abs_ms origin;
    // radius of the circle the shot lands in at the cursor distance, in tiles
    double spread_tiles = 0.0;
    // 0..1
    double steadiness = 0.0;
    bool can_fire = false;
    bool in_range = true;
};
const aim_view &local_aim_view();

struct melee_view {
    bool active = false;
    // tile that is about to be hit
    tripoint_abs_ms tile;
    int phase = 0;
    double progress = 0.0;
};
const melee_view &local_melee_view();

// ---------------------------------------------------------------------------------------
// the network (see fluid_net notes in doc/MULTIPLAYER_FLUID.md)

/** Applies an event to the visuals of this process (clients and host). */
void apply_visual_event( const event &ev, ms_t arrival );
void write_event( JsonOut &jo, const event &ev );
event read_event( const JsonObject &jo );

/** A player's body (free movement) as the host sends it to the clients: absolute tile
 * coordinates, 0.5 = the centre of a tile. */
struct body_sample {
    int actor = 0;
    double x = 0.0;
    double y = 0.0;
    int z = 0;
};
/** Host: where the free-moving bodies (players' bodies and the host's avatar) are now. */
std::vector<body_sample> host_body_samples();
/** Client: body positions from the host.  @p acked_seq is the newest input frame of this
 * client the host had applied (see net::client_send_input); the own predicted body is
 * corrected against what was predicted for that frame. */
void client_bodies( const std::vector<body_sample> &list, int acked_seq, ms_t arrival );
/** Client: the host sends body positions (free movement): other players are placed from
 * them, not from their tile steps. */
bool client_free_movement();

/** Host, phase 6: a monster commits to a melee blow at a player: the blow lands after a short
 * wind-up (shown as a lunge) and only if the player is still in reach then.  True if handled
 * (the caller does not attack now). */
bool monster_melee_windup( monster &z, Character &target );
/** Host, every frame: blows whose wind-up is over land or miss. */
void process_monster_windups( ms_t now );

/** Client: fluid mode is on for this session (the host plays fluid and so do we). */
void client_set_mode( bool on, int my_character_id );
bool client_active();
/** Client: overmap travel was confirmed: the body walks @p omt_path (as the avatar's omt_path,
 * the destination first) leg by leg, pressing the movement keys for the player.  Kept here,
 * because the avatar's own copy is replaced by the host's state. */
void client_start_travel( const std::vector<tripoint_abs_omt> &omt_path );
/** Client: one frame. Polls the device, sends the input, predicts the own steps. */
void client_frame();
/** Client: an event from the host. @p mine is true if the actor is this client's character.
 * Own steps confirm the prediction and keep the slide continuous. */
void client_event( const event &ev, ms_t arrival, bool mine );
/** Client: what the host says about the own aim (the crosshair is drawn at the local cursor). */
void client_aim_status( double steadiness, double spread_tiles, bool can_fire, bool aiming );

// ---------------------------------------------------------------------------------------
// classic input around fluid mode

/** Remembers a classic mouse press that fluid mode swallowed (the driver derives clicks). */
void note_classic_action( int action );
/** Around a classic action: whatever gets pressed while a menu is open does not count as
 * movement afterwards, until it was released. */
void classic_action_begin();
void classic_action_end();
/** True if fluid mode reads @p key as held state (movement, also with Shift for sprinting,
 * or the cancel / attack keys): pressing it is not an unknown command. */
bool device_key( int key );

// ---------------------------------------------------------------------------------------
// the layout switch (see doc)

/** The keyboard layouts the game can switch between. */
enum class key_layout : int {
    classic = 0,
    fluid_wasd
};
/** Reloads the keybindings with / without the fluid overlay. Safe to call at any time. */
void apply_key_layout( key_layout layout );
key_layout current_key_layout();
/** Applies option FLUID_WASD_LAYOUT if it differs from what is loaded. Cheap. */
void sync_key_layout();

} // namespace fluid

#endif // CATA_SRC_FLUID_H
