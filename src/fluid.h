#pragma once
#ifndef CATA_SRC_FLUID_H
#define CATA_SRC_FLUID_H

#include <cstdint>
#include <string>
#include <vector>

#include "coordinates.h"
#include "point.h"

class Character;
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
    // camera leads toward the cursor by this many tiles (0 = off)
    double camera_lead_tiles = 0.7;
    // ---- free (grid-free) movement, see doc/MULTIPLAYER_SMOOTH_MOVEMENT_PLAN.md
    // half the width of a player's body, in tiles (0.3 = a body 0.6 tiles wide)
    double body_half = 0.3;
    // walking straight past a corner that overlaps the body by less than this (tiles) nudges
    // the body around it instead of stopping it (door frames, gaps between cars)
    double corner_nudge = 0.4;
    // longest piece of movement checked for collisions at once (tiles)
    double max_substep = 0.2;
    // a frame never moves the body for longer than this (ms), e.g. after a hitch
    int max_move_frame_ms = 60;
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
    // the actor may start something new at this time
    ms_t ready_at = 0;
    // sprint key state of the last frame (hold to sprint)
    bool prev_sprint = false;
    bool prev_cancel = false;
    // the last action was a step and movement is still held: the next step chains on
    bool stepping = false;

    // ---- free movement: where the body really is (absolute tile coordinates, 0.5 = centre
    // of the tile).  The game sees the character on body_tile, the tile under the centre.
    bool body_valid = false;
    double body_x = 0.0;
    double body_y = 0.0;
    tripoint_abs_ms body_tile;
    ms_t body_last = 0;
    bool body_moving = false;
    // a visual correction that fades out over corr_ms (moves the game made itself: a bump into
    // a squeeze, stairs next door ...), in tiles relative to the body
    double corr_x = 0.0;
    double corr_y = 0.0;
    ms_t corr_t0 = 0;
    int corr_ms = 0;

    // ---- melee
    phase melee = phase::idle;
    ms_t phase_end = 0;
    ms_t melee_started = 0;
    int melee_total_ms = 0;
    int melee_windup_ms = 0;
    // direction of the blow (unit steps, 8 directions)
    int strike_dx = 0;
    int strike_dy = 1;
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
    aim         // aim state of a remote player (steadiness for its crosshair)
};

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

/** Runs one frame for a character. New events are appended to @p out (may be null). */
void drive( Character &who, actor_state &st, const input_frame &in, ms_t now,
            std::vector<event> *out );

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

/** Client: fluid mode is on for this session (the host plays fluid and so do we). */
void client_set_mode( bool on, int my_character_id );
bool client_active();
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
