#include "mp_lives.h"

#include <algorithm>
#include <list>
#include <sstream>
#include <string>

#include "avatar.h"
#include "calendar.h"
#include "character.h"
#include "debug.h"
#include "effect.h"
#include "fluid.h"
#include "item.h"
#include "json.h"
#include "json_loader.h"
#include "magic.h"
#include "map.h"
#include "messages.h"
#include "net_session.h"
#include "options.h"
#include "overmapbuffer.h"
#include "string_formatter.h"
#include "translations.h"
#include "type_id.h"

static const efftype_id effect_cureall( "cureall" );
static const efftype_id effect_downed( "downed" );
static const efftype_id effect_incorporeal( "incorporeal" );
static const efftype_id effect_mp_respawn_grace( "mp_respawn_grace" );
static const efftype_id effect_mp_respawn_incorporeal( "mp_respawn_incorporeal" );
static const efftype_id effect_mp_respawn_speed( "mp_respawn_speed" );
static const efftype_id effect_mp_respawn_veil( "mp_respawn_veil" );
static const efftype_id effect_panacea( "panacea" );
static const spell_id spell_mp_respawn_pushback( "mp_respawn_pushback" );

namespace mp_lives
{

namespace
{

// The difficulties of Extra Lives (eoc_difficulty_selection.json of the mod).
struct rules {
    bool unlimited = false;
    int start = 2;
    int max = 2;
    int cap = 4;
    // a life comes back after this many days alive (0 = never)
    int regain_days = 14;
    // the items stay with the player (else they stay where the player died)
    bool keep_items = true;
};

rules current_rules()
{
    const std::string d = get_option<std::string>( "MP_LIVES_DIFFICULTY" );
    rules r;
    if( d == "tourist" ) {
        r.unlimited = true;
        r.regain_days = 0;
    } else if( d == "easy" ) {
        r = { false, 3, 3, 5, 0, true };
    } else if( d == "hard" ) {
        r = { false, 2, 2, 3, 21, false };
    } else if( d == "very_hard" ) {
        r = { false, 1, 1, 2, 35, false };
    } else {
        // normal
        r = { false, 2, 2, 4, 14, true };
    }
    return r;
}

// the message goes to whoever plays @p who (the host or a client)
void say( const Character &who, bool good, const std::string &text )
{
    net::tell_player( who, text, good );
}

int get_int( const Character &who, const std::string &key, int def )
{
    const diag_value *v = who.maybe_get_value( key );
    return v != nullptr && v->is_dbl() ? static_cast<int>( v->dbl() ) : def;
}

void set_int( Character &who, const std::string &key, int value )
{
    who.set_value( key, static_cast<double>( value ) );
}

// "EOC_heal_and_reset_conditions" of the mod: healthy, rested, nothing bad on the character.
void heal_and_reset( Character &who )
{
    who.set_all_parts_hp_to_max();
    who.set_stored_kcal( std::max( who.get_stored_kcal(), who.get_healthy_kcal() ) );
    who.set_thirst( std::min( who.get_thirst(), 0 ) );
    for( const char *vit : {
             "redcells", "bad_food", "blood", "instability"
         } ) {
        const vitamin_id id( vit );
        if( id.is_valid() ) {
            who.vitamin_set( id, 0 );
        }
    }
    who.set_pain( 0 );
    who.set_rad( 0 );
    who.set_stamina( who.get_stamina_max() );
    who.set_sleepiness( 0 );
    who.set_sleep_deprivation( 0 );
    who.set_painkiller( 0 );
    who.set_stim( 0 );
    who.set_focus( 100 );
    static const char *const bad_effects[] = {
        "corroding", "onfire", "dazed", "stunned", "venom_blind", "nausea", "stim", "stim_overdose",
        "formication", "shakes", "blisters", "frostbite", "frostbite_recovery", "wet", "slimed",
        "migo_atmosphere", "high", "fetid_goop", "weary_1", "lack_sleep", "sap", "bleed", "deaf",
        "blind", "zapped", "staggered_character", "pkill", "pkill2", "pkill3", "pkill_l", "winded",
        "grabbed", "downed", "hot", "hot_speed", "sad", "tetanus", "flu", "pre_flu", "tapeworm",
        "fungus", "dermatik", "rat_bite_fever", "common_cold", "brainworms", "paincysts",
        "bloodworms", "VITRIFYING", "teargas", "smoke_eyes", "smoke_lungs", "poison", "badpoison",
        "relax_gas", "glowing_gas", "glowing_gas_cover", "tpollen", "hallucinogenic_acid_light",
        "hallucinogenic_acid_heavy", "foodpoison", "paralyzepoison", "venom_dmg", "venom_weaken",
        "venom_pain", "venom_player1", "venom_player2", "infected", "bite", "anemia",
        "redcells_anemia", "hypovolemia", "anticoagulant", "anticoagulant_draculin",
        "conjunctivitis", "hay_fever", "sneezing", "pre_common_cold", "asthma", "scurvy",
        "hypervitaminosis", "bile_irritant", "bile_stink", "taint", "triffid_fiber_indigestion",
        "motor_seizure", "dermatik_visible", "sleep_deprived", "boomered", "glowing", "skunk_spray",
        "sludged", "glare", "snow_glare", "datura", "visuals", "hallucination_attacks", "beartrap",
        "lightsnare", "immobilization", "webbed", "crushed", "staggered", "fearparalyze",
        "laserlocked", "was_laserlocked", "electrocuted", "sensor_stun", "winded_arm_l",
        "winded_arm_r", "winded_leg_l", "winded_leg_r", "slowed_time", "strengthened_gravity",
        "strengthened_inertia", "cold", "meth", "spores", "chafing"
    };
    for( const char *id : bad_effects ) {
        const efftype_id eff( id );
        if( eff.is_valid() && who.has_effect( eff ) ) {
            who.remove_effect( eff );
        }
    }
}

// The mod's durations are meant as real seconds of play.  In realtime play a turn is shorter than
// a second (option FLUID_TURN_MS), so they are converted.
time_duration real_seconds( int seconds )
{
    if( !fluid::enabled() ) {
        return time_duration::from_seconds( seconds );
    }
    return time_duration::from_turns( std::max( 1, seconds * 1000 / std::max( 1, fluid::turn_ms() ) ) );
}

// The items stay where the player died (hard and very hard).
void drop_everything( Character &who )
{
    map &here = get_map();
    const tripoint_bub_ms at = who.pos_bub( here );
    std::list<item> things = who.remove_items_with( []( const item & ) {
        return true;
    } );
    if( things.empty() ) {
        return;
    }
    for( item &it : things ) {
        here.add_item_or_charges( at, it );
    }
    overmap_buffer.add_note( coords::project_to<coords::omt>( who.pos_abs() ),
                             string_format( _( "%s's things (died here)" ), who.get_name() ) );
    say( who, false, _( "Everything you carried stayed where you died." ) );
}

} // namespace

void init( Character &who )
{
    if( get_int( who, "mp_lives_init", 0 ) != 0 ) {
        return;
    }
    const rules r = current_rules();
    set_int( who, "mp_lives_init", 1 );
    set_int( who, "mp_lives", r.unlimited ? -1 : r.start );
    set_int( who, "mp_lives_max", r.max );
    set_int( who, "mp_lives_cap", r.cap );
    set_int( who, "mp_deaths", 0 );
    set_int( who, "mp_lives_regain_turn", to_turn<int>( calendar::turn ) );
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: lives of " << who.get_name() << " set to " <<
                               get_int( who, "mp_lives", 0 );
}

int lives_left( const Character &who )
{
    return get_int( who, "mp_lives", 0 );
}

bool spend_life( Character &who )
{
    init( who );
    set_int( who, "mp_deaths", get_int( who, "mp_deaths", 0 ) + 1 );
    const int lives = lives_left( who );
    if( lives < 0 ) {
        return true;
    }
    if( lives == 0 ) {
        return false;
    }
    set_int( who, "mp_lives", lives - 1 );
    return true;
}

std::string lives_text( const Character &who )
{
    const int lives = lives_left( who );
    if( lives < 0 ) {
        return _( "unlimited lives" );
    }
    return string_format( n_gettext( "%d extra life left", "%d extra lives left", lives ), lives );
}

void respawn_here( Character &who )
{
    const rules r = current_rules();
    if( !r.keep_items ) {
        drop_everything( who );
    }
    heal_and_reset( who );
    who.remove_effect( effect_downed );
    // "EOC_respawn_here" of the mod
    who.add_effect( effect_incorporeal, real_seconds( 30 ) );
    who.add_effect( effect_mp_respawn_incorporeal, real_seconds( 30 ) );
    who.add_effect( effect_mp_respawn_veil, real_seconds( 90 ) );
    who.add_effect( effect_mp_respawn_speed, real_seconds( 90 ) );
    who.add_effect( effect_mp_respawn_grace, real_seconds( 30 ) );
    who.add_effect( effect_cureall, real_seconds( 1 ), false, 1 );
    who.add_effect( effect_panacea, real_seconds( 30 ), false, 1 );
    // five waves of force, one per second (the first now)
    set_int( who, "mp_pushback_left", 5 );
    set_int( who, "mp_pushback_next", 0 );
    say( who, true, bed_of( who ) ?
         _( "In the split-second before your life is snuffed out, you are remade where you last slept!" ) :
         _( "In the split-second before your life is snuffed out, you are remade!" ) );
    say( who, false, string_format( _( "** You have %s. **" ), lives_text( who ) ) );
    DebugLog( D_INFO, D_MAIN ) << "Multiplayer: " << who.get_name() << " respawned here, " <<
                               lives_text( who );
}

std::optional<tripoint_abs_ms> bed_of( const Character &who )
{
    if( get_int( who, "mp_bed_set", 0 ) == 0 ) {
        return std::nullopt;
    }
    return tripoint_abs_ms( get_int( who, "mp_bed_x", 0 ), get_int( who, "mp_bed_y", 0 ),
                            get_int( who, "mp_bed_z", 0 ) );
}

void tick( Character &who )
{
    if( who.is_dead_state() ) {
        return;
    }
    init( who );
    // the respawn point: where the player last slept (Noah, 2026-10-14)
    if( who.in_sleep_state() ) {
        const tripoint_abs_ms at = who.pos_abs();
        const std::optional<tripoint_abs_ms> bed = bed_of( who );
        if( !bed || *bed != at ) {
            set_int( who, "mp_bed_set", 1 );
            set_int( who, "mp_bed_x", at.x() );
            set_int( who, "mp_bed_y", at.y() );
            set_int( who, "mp_bed_z", at.z() );
            say( who, true, _( "This is your respawn point now (where you sleep)." ) );
        }
    }
    // lives that come back over time ("EOC_regain_life_time_passed_*")
    const rules r = current_rules();
    const int now_turn = to_turn<int>( calendar::turn );
    if( r.regain_days > 0 && lives_left( who ) >= 0 ) {
        const int since = get_int( who, "mp_lives_regain_turn", now_turn );
        if( now_turn - since >= to_turns<int>( 1_days * r.regain_days ) ) {
            set_int( who, "mp_lives_regain_turn", now_turn );
            if( lives_left( who ) < get_int( who, "mp_lives_max", r.max ) ) {
                set_int( who, "mp_lives", lives_left( who ) + 1 );
                say( who, true, string_format(
                         _( "Your continued survival for %d days has earned you an extra life!  (%s)" ),
                         r.regain_days, lives_text( who ) ) );
            }
        }
    }
    // the respawn grace: healed again while it lasts
    if( who.has_effect( effect_mp_respawn_grace ) ) {
        heal_and_reset( who );
    }
    // the waves of force
    const int waves = get_int( who, "mp_pushback_left", 0 );
    if( waves > 0 ) {
        const int now = static_cast<int>( fluid::now_ms() % 1000000000 );
        const int next = get_int( who, "mp_pushback_next", 0 );
        if( next == 0 || now >= next || now + 500000000 < next ) {
            set_int( who, "mp_pushback_left", waves - 1 );
            set_int( who, "mp_pushback_next", now + 1000 );
            if( spell_mp_respawn_pushback.is_valid() ) {
                const spell sp( spell_mp_respawn_pushback );
                sp.cast_all_effects( who, who.pos_bub() );
            }
            say( who, true, waves > 1 ?
                 _( "A wave of force emanates from you, knocking back those around you!" ) :
                 _( "The waves of force emanating from you cease." ) );
        }
    }
}

void make_new_character( avatar &u )
{
    avatar fresh;
    while( !fresh.create( character_type::CUSTOM ) ) {
        fresh = avatar();
    }
    std::ostringstream os;
    JsonOut jsout( os );
    fresh.serialize( jsout );
    const character_id own_id = u.getID();
    map &here = get_map();
    const tripoint_bub_ms pos = u.pos_bub( here );
    u.setID( character_id(), true );
    try {
        JsonValue jv = json_loader::from_string( os.str() );
        JsonObject jo = jv.get_object();
        jo.allow_omitted_members();
        u.deserialize( jo );
    } catch( const std::exception &err ) {
        DebugLog( D_WARNING, D_GAME ) << "Multiplayer: new character: bad data: " << err.what();
    }
    u.setID( own_id, true );
    u.setpos( here, pos );
    u.add_profession_items();
    u.set_all_parts_hp_to_max();
    // a new character gets the lives of the difficulty
    u.remove_value( "mp_lives_init" );
    init( u );
}

} // namespace mp_lives
