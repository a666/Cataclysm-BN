#include "avatar_action.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "action.h"
#include "activity_actor_definitions.h"
#include "avatar.h"
#include "bodypart.h"
#include "calendar.h"
#include "character.h"
#include "creature.h"
#include "cursesdef.h"
#include "debug.h"
#include "enums.h"
#include "game.h"
#include "game_constants.h"
#include "game_inventory.h"
#include "gun_mode.h"
#include "int_id.h"
#include "inventory.h"
#include "item.h"
#include "item_contents.h"
#include "item_location.h"
#include "itype.h"
#include "line.h"
#include "map.h"
#include "map_iterator.h"
#include "mapdata.h"
#include "math_defines.h"
#include "messages.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "options.h"
#include "output.h"
#include "player_activity.h"
#include "projectile.h"
#include "ranged.h"
#include "ret_val.h"
#include "rng.h"
#include "string_formatter.h"
#include "translations.h"
#include "type_id.h"
#include "value_ptr.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"

class player;

static const efftype_id effect_amigara( "amigara" );
static const efftype_id effect_glowing( "glowing" );
static const efftype_id effect_harnessed( "harnessed" );
static const efftype_id effect_onfire( "onfire" );
static const efftype_id effect_pet( "pet" );
static const efftype_id effect_relax_gas( "relax_gas" );
static const efftype_id effect_ridden( "ridden" );
static const efftype_id effect_stunned( "stunned" );

static const itype_id itype_grass( "grass" );
static const itype_id itype_swim_fins( "swim_fins" );
static const itype_id itype_underbrush( "underbrush" );

static const skill_id skill_swimming( "swimming" );

static const trait_id trait_BURROW( "BURROW" );
static const trait_id trait_GRAZER( "GRAZER" );
static const trait_id trait_RUMINANT( "RUMINANT" );
static const trait_id trait_SHELL2( "SHELL2" );

static const std::string flag_ALLOWS_REMOTE_USE( "ALLOWS_REMOTE_USE" );
static const std::string flag_DIG_TOOL( "DIG_TOOL" );
static const std::string flag_NO_UNWIELD( "NO_UNWIELD" );
static const std::string flag_RAMP_END( "RAMP_END" );
static const std::string flag_SWIMMABLE( "SWIMMABLE" );

#define dbg(x) DebugLog((x), DC::SDL)

bool can_fire_turret( avatar &you, const map &m, const turret_data &turret );

bool avatar_action::move( avatar &you, map &m, const tripoint &d )
{
    if( ( !g->check_safe_mode_allowed() ) || you.has_active_mutation( trait_SHELL2 ) ) {
        if( you.has_active_mutation( trait_SHELL2 ) ) {
            add_msg( m_warning, _( "You can't move while in your shell.  Deactivate it to go mobile." ) );
        }
        return false;
    }
    const bool is_riding = you.is_mounted();
    tripoint dest_loc;
    if( d.z == 0 && you.has_effect( effect_stunned ) ) {
        dest_loc.x = rng( you.posx() - 1, you.posx() + 1 );
        dest_loc.y = rng( you.posy() - 1, you.posy() + 1 );
        dest_loc.z = you.posz();
    } else {
        dest_loc.x = you.posx() + d.x;
        dest_loc.y = you.posy() + d.y;
        dest_loc.z = you.posz() + d.z;
    }

    if( dest_loc == you.pos() ) {
        // Well that sure was easy
        return true;
    }
    bool via_ramp = false;
    if( m.has_flag( TFLAG_RAMP_UP, dest_loc ) ) {
        dest_loc.z += 1;
        via_ramp = true;
    } else if( m.has_flag( TFLAG_RAMP_DOWN, dest_loc ) ) {
        dest_loc.z -= 1;
        via_ramp = true;
    }

    if( m.has_flag( TFLAG_MINEABLE, dest_loc ) && g->mostseen == 0 &&
        get_option<bool>( "AUTO_FEATURES" ) && get_option<bool>( "AUTO_MINING" ) &&
        !m.veh_at( dest_loc ) && !you.is_underwater() && !you.has_effect( effect_stunned ) &&
        !is_riding ) {
        if( you.weapon.has_flag( flag_DIG_TOOL ) ) {
            if( you.weapon.type->can_use( "JACKHAMMER" ) && you.weapon.ammo_sufficient() ) {
                you.invoke_item( &you.weapon, "JACKHAMMER", dest_loc );
                // don't move into the tile until done mining
                you.defer_move( dest_loc );
                return true;
            } else if( you.weapon.type->can_use( "PICKAXE" ) ) {
                you.invoke_item( &you.weapon, "PICKAXE", dest_loc );
                // don't move into the tile until done mining
                you.defer_move( dest_loc );
                return true;
            }
        }
        if( you.has_trait( trait_BURROW ) ) {
            item burrowing_item( itype_id( "fake_burrowing" ) );
            you.invoke_item( &burrowing_item, "BURROW", dest_loc );
            // don't move into the tile until done mining
            you.defer_move( dest_loc );
            return true;
        }
    }

    // If the player is *attempting to* move on the X axis, update facing direction of their sprite to match.
    point new_d( dest_loc.xy() + point( -you.posx(), -you.posy() ) );

    if( !tile_iso ) {
        if( new_d.x > 0 ) {
            you.facing = FD_RIGHT;
            if( is_riding ) {
                you.mounted_creature->facing = FD_RIGHT;
            }
        } else if( new_d.x < 0 ) {
            you.facing = FD_LEFT;
            if( is_riding ) {
                you.mounted_creature->facing = FD_LEFT;
            }
        }
    } else {
        //
        // iso:
        //
        // right key            =>  +x -y       FD_RIGHT
        // left key             =>  -x +y       FD_LEFT
        // up key               =>  +x +y       ______
        // down key             =>  -x -y       ______
        // y: left-up key       =>  __ +y       FD_LEFT
        // u: right-up key      =>  +x __       FD_RIGHT
        // b: left-down key     =>  -x __       FD_LEFT
        // n: right-down key    =>  __ -y       FD_RIGHT
        //
        // right key            =>  +x -y       FD_RIGHT
        // u: right-up key      =>  +x __       FD_RIGHT
        // n: right-down key    =>  __ -y       FD_RIGHT
        // up key               =>  +x +y       ______
        // down key             =>  -x -y       ______
        // left key             =>  -x +y       FD_LEFT
        // y: left-up key       =>  __ +y       FD_LEFT
        // b: left-down key     =>  -x __       FD_LEFT
        //
        // right key            =>  +x +y       FD_RIGHT
        // u: right-up key      =>  +x __       FD_RIGHT
        // n: right-down key    =>  __ +y       FD_RIGHT
        // up key               =>  +x -y       ______
        // left key             =>  -x -y       FD_LEFT
        // b: left-down key     =>  -x __       FD_LEFT
        // y: left-up key       =>  __ -y       FD_LEFT
        // down key             =>  -x +y       ______
        //
        if( new_d.x >= 0 && new_d.y >= 0 ) {
            you.facing = FD_RIGHT;
            if( is_riding ) {
                auto mons = you.mounted_creature.get();
                mons->facing = FD_RIGHT;
            }
        }
        if( new_d.y <= 0 && new_d.x <= 0 ) {
            you.facing = FD_LEFT;
            if( is_riding ) {
                auto mons = you.mounted_creature.get();
                mons->facing = FD_LEFT;
            }
        }
    }

    if( you.has_effect( effect_amigara ) ) {
        int curdist = INT_MAX;
        int newdist = INT_MAX;
        const tripoint minp = tripoint( 0, 0, you.posz() );
        const tripoint maxp = tripoint( MAPSIZE_X, MAPSIZE_Y, you.posz() );
        for( const tripoint &pt : m.points_in_rectangle( minp, maxp ) ) {
            if( m.ter( pt ) == t_fault ) {
                int dist = rl_dist( pt, you.pos() );
                if( dist < curdist ) {
                    curdist = dist;
                }
                dist = rl_dist( pt, dest_loc );
                if( dist < newdist ) {
                    newdist = dist;
                }
            }
        }
        if( newdist > curdist ) {
            add_msg( m_info, _( "You cannot pull yourself away from the faultline…" ) );
            return false;
        }
    }

    dbg( DL::Debug ) << "game:plmove: From " << you.pos() << " to " << dest_loc;

    if( g->disable_robot( dest_loc ) ) {
        return false;
    }

    // Check if our movement is actually an attack on a monster or npc
    // Are we displacing a monster?

    bool attacking = false;
    if( g->critter_at( dest_loc ) ) {
        attacking = true;
    }

    if( !you.move_effects( attacking ) ) {
        you.moves -= 100;
        return false;
    }

    if( monster *const mon_ptr = g->critter_at<monster>( dest_loc, true ) ) {
        monster &critter = *mon_ptr;
        if( critter.friendly == 0 &&
            !critter.has_effect( effect_pet ) ) {
            if( you.is_auto_moving() ) {
                add_msg( m_warning, _( "Monster in the way.  Auto-move canceled." ) );
                add_msg( m_info, _( "Move into the monster to attack." ) );
                you.clear_destination();
                return false;
            } else {
            }
            if( you.has_effect( effect_relax_gas ) ) {
                if( one_in( 8 ) ) {
                    add_msg( m_good, _( "Your willpower asserts itself, and so do you!" ) );
                } else {
                    you.moves -= rng( 2, 8 ) * 10;
                    add_msg( m_bad, _( "You're too pacified to strike anything…" ) );
                    return false;
                }
            }
            you.melee_attack( critter, true );
            if( critter.is_hallucination() ) {
                critter.die( &you );
            }
            g->draw_hit_mon( dest_loc, critter, critter.is_dead() );
            return false;
        } else if( critter.has_flag( MF_IMMOBILE ) || critter.has_effect( effect_harnessed ) ||
                   critter.has_effect( effect_ridden ) ) {
            add_msg( m_info, _( "You can't displace your %s." ), critter.name() );
            return false;
        }
        // Successful displacing is handled (much) later
    }
    // If not a monster, maybe there's an NPC there
    if( npc *const np_ = g->critter_at<npc>( dest_loc ) ) {
        npc &np = *np_;
        if( you.is_auto_moving() ) {
            add_msg( _( "NPC in the way, Auto-move canceled." ) );
            add_msg( m_info, _( "Move into the NPC to interact or attack." ) );
            you.clear_destination();
            return false;
        }

        if( !np.is_enemy() ) {
            g->npc_menu( np );
            return false;
        }

        you.melee_attack( np, true );
        np.make_angry();
        return false;
    }

    // GRAB: pre-action checking.
    int dpart = -1;
    const optional_vpart_position vp0 = m.veh_at( you.pos() );
    vehicle *const veh0 = veh_pointer_or_null( vp0 );
    const optional_vpart_position vp1 = m.veh_at( dest_loc );
    vehicle *const veh1 = veh_pointer_or_null( vp1 );

    bool veh_closed_door = false;
    bool outside_vehicle = ( veh0 == nullptr || veh0 != veh1 );
    if( veh1 != nullptr ) {
        dpart = veh1->next_part_to_open( vp1->part_index(), outside_vehicle );
        veh_closed_door = dpart >= 0 && !veh1->part( dpart ).open;
    }

    if( veh0 != nullptr && std::abs( veh0->velocity ) > 100 ) {
        if( veh1 == nullptr ) {
            if( query_yn( _( "Dive from moving vehicle?" ) ) ) {
                g->moving_vehicle_dismount( dest_loc );
            }
            return false;
        } else if( veh1 != veh0 ) {
            add_msg( m_info, _( "There is another vehicle in the way." ) );
            return false;
        } else if( !vp1.part_with_feature( "BOARDABLE", true ) ) {
            add_msg( m_info, _( "That part of the vehicle is currently unsafe." ) );
            return false;
        }
    }
    bool toSwimmable = m.has_flag( flag_SWIMMABLE, dest_loc );
    bool toDeepWater = m.has_flag( TFLAG_DEEP_WATER, dest_loc );
    bool fromSwimmable = m.has_flag( flag_SWIMMABLE, you.pos() );
    bool fromDeepWater = m.has_flag( TFLAG_DEEP_WATER, you.pos() );
    bool fromBoat = veh0 != nullptr && veh0->is_in_water();
    bool toBoat = veh1 != nullptr && veh1->is_in_water();
    if( is_riding ) {
        if( !you.check_mount_will_move( dest_loc ) ) {
            if( you.is_auto_moving() ) {
                you.clear_destination();
            }
            you.moves -= 20;
            return false;
        }
    }
    // Dive into water!
    if( toSwimmable && toDeepWater && !toBoat ) {
        // Requires confirmation if we were on dry land previously
        if( is_riding ) {
            auto mon = you.mounted_creature.get();
            if( !mon->swims() || mon->get_size() < you.get_size() + 2 ) {
                add_msg( m_warning, _( "The %s cannot swim while it is carrying you!" ), mon->get_name() );
                return false;
            }
        }
        if( ( fromSwimmable && fromDeepWater && !fromBoat ) || query_yn( _( "Dive into the water?" ) ) ) {
            if( ( !fromDeepWater || fromBoat ) && you.swim_speed() < 500 ) {
                add_msg( _( "You start swimming." ) );
                add_msg( m_info, _( "%s to dive underwater." ),
                         press_x( ACTION_MOVE_DOWN ) );
            }
            avatar_action::swim( get_map(), get_avatar(), dest_loc );
        }

        g->on_move_effects();
        return true;
    }

    //Wooden Fence Gate (or equivalently walkable doors):
    // open it if we are walking
    // vault over it if we are running
    if( m.passable_ter_furn( dest_loc )
        && you.movement_mode_is( CMM_WALK )
        && m.open_door( dest_loc, !m.is_outside( you.pos() ) ) ) {
        you.moves -= 100;
        // if auto-move is on, continue moving next turn
        if( you.is_auto_moving() ) {
            you.defer_move( dest_loc );
        }
        return true;
    }
    if( g->walk_move( dest_loc, via_ramp ) ) {
        return true;
    }
    if( veh_closed_door ) {
        if( !veh1->handle_potential_theft( dynamic_cast<player &>( you ) ) ) {
            return true;
        } else {
            if( outside_vehicle ) {
                veh1->open_all_at( dpart );
            } else {
                veh1->open( dpart );
                add_msg( _( "You open the %1$s's %2$s." ), veh1->name,
                         veh1->part_info( dpart ).name() );
            }
        }
        you.moves -= 100;
        // if auto-move is on, continue moving next turn
        if( you.is_auto_moving() ) {
            you.defer_move( dest_loc );
        }
        return true;
    }

    if( m.furn( dest_loc ) != f_safe_c && m.open_door( dest_loc, !m.is_outside( you.pos() ) ) ) {
        you.moves -= 100;
        // if auto-move is on, continue moving next turn
        if( you.is_auto_moving() ) {
            you.defer_move( dest_loc );
        }
        return true;
    }

    // Invalid move
    const bool waste_moves = you.is_blind() || you.has_effect( effect_stunned );
    if( waste_moves || dest_loc.z != you.posz() ) {
        add_msg( _( "You bump into the %s!" ), m.obstacle_name( dest_loc ) );
        // Only lose movement if we're blind
        if( waste_moves ) {
            you.moves -= 100;
        }
    } else if( m.ter( dest_loc ) == t_door_locked || m.ter( dest_loc ) == t_door_locked_peep ||
               m.ter( dest_loc ) == t_door_locked_alarm || m.ter( dest_loc ) == t_door_locked_interior ) {
        // Don't drain move points for learning something you could learn just by looking
        add_msg( _( "That door is locked!" ) );
    } else if( m.ter( dest_loc ) == t_door_bar_locked ) {
        add_msg( _( "You rattle the bars but the door is locked!" ) );
    }
    return false;
}

bool avatar_action::ramp_move( avatar &you, map &m, const tripoint &dest_loc )
{
    if( dest_loc.z != you.posz() ) {
        // No recursive ramp_moves
        return false;
    }

    // We're moving onto a tile with no support, check if it has a ramp below
    if( !m.has_floor_or_support( dest_loc ) ) {
        tripoint below( dest_loc.xy(), dest_loc.z - 1 );
        if( m.has_flag( TFLAG_RAMP, below ) ) {
            // But we're moving onto one from above
            const tripoint dp = dest_loc - you.pos();
            move( you, m, tripoint( dp.xy(), -1 ) );
            // No penalty for misaligned stairs here
            // Also cheaper than climbing up
            return true;
        }

        return false;
    }

    if( !m.has_flag( TFLAG_RAMP, you.pos() ) ||
        m.passable( dest_loc ) ) {
        return false;
    }

    // Try to find an aligned end of the ramp that will make our climb faster
    // Basically, finish walking on the stairs instead of pulling self up by hand
    bool aligned_ramps = false;
    for( const tripoint &pt : m.points_in_radius( you.pos(), 1 ) ) {
        if( rl_dist( pt, dest_loc ) < 2 && m.has_flag( flag_RAMP_END, pt ) ) {
            aligned_ramps = true;
            break;
        }
    }

    const tripoint above_u( you.posx(), you.posy(), you.posz() + 1 );
    if( m.has_floor_or_support( above_u ) ) {
        add_msg( m_warning, _( "You can't climb here - there's a ceiling above." ) );
        return false;
    }

    const tripoint dp = dest_loc - you.pos();
    const tripoint old_pos = you.pos();
    move( you, m, tripoint( dp.xy(), 1 ) );
    // We can't just take the result of the above function here
    if( you.pos() != old_pos ) {
        you.moves -= 50 + ( aligned_ramps ? 0 : 50 );
    }

    return true;
}

void avatar_action::swim( map &m, avatar &you, const tripoint &p )
{
    if( !m.has_flag( flag_SWIMMABLE, p ) ) {
        debugmsg( "Tried to swim in %s!", m.tername( p ) );
        return;
    }
    if( you.has_effect( effect_onfire ) ) {
        add_msg( _( "The water puts out the flames!" ) );
        you.remove_effect( effect_onfire );
        if( you.is_mounted() ) {
            monster *mon = you.mounted_creature.get();
            if( mon->has_effect( effect_onfire ) ) {
                mon->remove_effect( effect_onfire );
            }
        }
    }
    if( you.has_effect( effect_glowing ) ) {
        add_msg( _( "The water washes off the glowing goo!" ) );
        you.remove_effect( effect_glowing );
    }
    int movecost = you.swim_speed();
    you.practice( skill_swimming, you.is_underwater() ? 2 : 1 );
    if( movecost >= 500 ) {
        if( !you.is_underwater() &&
            !( you.shoe_type_count( itype_swim_fins ) == 2 ||
               ( you.shoe_type_count( itype_swim_fins ) == 1 && one_in( 2 ) ) ) ) {
            add_msg( m_bad, _( "You sink like a rock!" ) );
            you.set_underwater( true );
            ///\EFFECT_STR increases breath-holding capacity while sinking
            you.oxygen = 30 + 2 * you.str_cur;
        }
    }
    if( you.oxygen <= 5 && you.is_underwater() ) {
        if( movecost < 500 ) {
            popup( _( "You need to breathe!  (%s to surface.)" ), press_x( ACTION_MOVE_UP ) );
        } else {
            popup( _( "You need to breathe but you can't swim!  Get to dry land, quick!" ) );
        }
    }
    bool diagonal = ( p.x != you.posx() && p.y != you.posy() );
    if( you.in_vehicle ) {
        m.unboard_vehicle( you.pos() );
    }
    if( you.is_mounted() && m.veh_at( you.pos() ).part_with_feature( VPFLAG_BOARDABLE, true ) ) {
        add_msg( m_warning, _( "You cannot board a vehicle while mounted." ) );
        return;
    }
    if( const auto vp = m.veh_at( p ).part_with_feature( VPFLAG_BOARDABLE, true ) ) {
        if( !vp->vehicle().handle_potential_theft( dynamic_cast<player &>( you ) ) ) {
            return;
        }
    }
    you.setpos( p );
    g->update_map( you );

    cata_event_dispatch::avatar_moves( you, m, p );

    if( m.veh_at( you.pos() ).part_with_feature( VPFLAG_BOARDABLE, true ) ) {
        m.board_vehicle( you.pos(), &you );
    }
    you.moves -= ( movecost > 200 ? 200 : movecost ) * ( trigdist && diagonal ? M_SQRT2 : 1 );
    you.inv.rust_iron_items();

    if( !you.is_mounted() ) {
        you.burn_move_stamina( movecost );
    }

    body_part_set drenchFlags{ {
            bp_leg_l, bp_leg_r, bp_torso, bp_arm_l,
            bp_arm_r, bp_foot_l, bp_foot_r, bp_hand_l, bp_hand_r
        }
    };

    if( you.is_underwater() ) {
        drenchFlags |= { { bp_head, bp_eyes, bp_mouth, bp_hand_l, bp_hand_r } };
    }
    you.drench( 100, drenchFlags, true );
}

static float rate_critter( const Creature &c )
{
    const npc *np = dynamic_cast<const npc *>( &c );
    if( np != nullptr ) {
        return np->weapon_value( np->weapon );
    }

    const monster *m = dynamic_cast<const monster *>( &c );
    return m->type->difficulty;
}

void avatar_action::autoattack( avatar &you, map &m )
{
    int reach = you.weapon.reach_range( you );
    std::vector<Creature *> critters = ranged::targetable_creatures( you, reach );
    critters.erase( std::remove_if( critters.begin(), critters.end(), []( const Creature * c ) {
        if( !c->is_npc() ) {
            return false;
        }
        return !dynamic_cast<const npc *>( c )->is_enemy();
    } ), critters.end() );
    if( critters.empty() ) {
        add_msg( m_info, _( "No hostile creature in reach.  Waiting a turn." ) );
        if( g->check_safe_mode_allowed() ) {
            you.pause();
        }
        return;
    }

    Creature &best = **std::max_element( critters.begin(), critters.end(),
    []( const Creature * l, const Creature * r ) {
        return rate_critter( *l ) > rate_critter( *r );
    } );

    const tripoint diff = best.pos() - you.pos();
    if( std::abs( diff.x ) <= 1 && std::abs( diff.y ) <= 1 && diff.z == 0 ) {
        move( you, m, tripoint( diff.xy(), 0 ) );
        return;
    }

    you.reach_attack( best.pos() );
}

bool avatar_action::can_fire_weapon( avatar &you, const map &m, const item &weapon )
{
    if( !weapon.is_gun() ) {
        debugmsg( "Expected item to be a gun" );
        return false;
    }

    if( you.has_effect( effect_relax_gas ) ) {
        if( one_in( 5 ) ) {
            add_msg( m_good, _( "Your eyes steel, and you raise your weapon!" ) );
        } else {
            you.moves -= rng( 2, 5 ) * 10;
            add_msg( m_bad, _( "You can't fire your weapon, it's too heavy…" ) );
            return false;
        }
    }

    std::vector<std::string> messages;

    const gun_mode &mode = weapon.gun_current_mode();
    bool check_common = ranged::gunmode_checks_common( you, m, messages, mode );
    bool check_weapon = ranged::gunmode_checks_weapon( you, m, messages, mode );
    bool can_use_mode = check_common && check_weapon;
    if( can_use_mode ) {
        return true;
    }

    for( const std::string &message : messages ) {
        add_msg( m_info, message );
    }
    return false;
}

/**
 * Checks if the turret is valid and if the player meets certain conditions for manually firing it.
 * @param turret Turret to check.
 * @return True if all conditions are true, otherwise false.
 */
bool can_fire_turret( avatar &you, const map &m, const turret_data &turret )
{
    const item &weapon = *turret.base();
    if( !weapon.is_gun() ) {
        debugmsg( "Expected turret base to be a gun." );
        return false;
    }

    switch( turret.query() ) {
        case turret_data::status::no_ammo:
            add_msg( m_bad, _( "The %s is out of ammo." ), turret.name() );
            return false;

        case turret_data::status::no_power:
            add_msg( m_bad, _( "The %s is not powered." ), turret.name() );
            return false;

        case turret_data::status::ready:
            break;

        default:
            debugmsg( "Unknown turret status" );
            return false;
    }

    if( you.has_effect( effect_relax_gas ) ) {
        if( one_in( 5 ) ) {
            add_msg( m_good, _( "Your eyes steel, and you aim your weapon!" ) );
        } else {
            you.moves -= rng( 2, 5 ) * 10;
            add_msg( m_bad, _( "You are too pacified to aim the turret…" ) );
            return false;
        }
    }

    std::vector<std::string> messages;

    for( const std::pair<const gun_mode_id, gun_mode> &mode_map : weapon.gun_all_modes() ) {
        bool can_use_mode = ranged::gunmode_checks_common( you, m, messages, mode_map.second );
        if( can_use_mode ) {
            return true;
        }
    }

    for( const std::string &message : messages ) {
        add_msg( m_info, message );
    }
    return false;
}

void avatar_action::fire_wielded_weapon( avatar &you )
{
    item &weapon = you.weapon;
    if( weapon.is_gunmod() ) {
        add_msg( m_info,
                 _( "The %s must be attached to a gun, it can not be fired separately." ),
                 weapon.tname() );
        return;
    } else if( !weapon.is_gun() ) {
        return;
    } else if( weapon.ammo_data() && weapon.type->gun &&
               !weapon.type->gun->ammo.count( weapon.ammo_data()->ammo->type ) ) {
        std::string ammoname = weapon.ammo_current()->nname( 1 );
        add_msg( m_info, _( "The %s can't be fired while loaded with incompatible ammunition %s" ),
                 weapon.tname(), ammoname );
        return;
    }

    you.assign_activity( aim_activity_actor::use_wielded(), false );
}

void avatar_action::fire_ranged_mutation( avatar &you, const item &fake_gun )
{
    you.assign_activity( aim_activity_actor::use_mutation( fake_gun ), false );
}

void avatar_action::fire_ranged_bionic( avatar &you, const item &fake_gun,
                                        const units::energy &cost_per_shot )
{
    you.assign_activity( aim_activity_actor::use_bionic( fake_gun, cost_per_shot ), false );
}

void avatar_action::fire_turret_manual( avatar &you, map &m, turret_data &turret )
{
    if( !can_fire_turret( you, m, turret ) ) {
        return;
    }

    g->temp_exit_fullscreen();
    target_handler::trajectory trajectory = target_handler::mode_turret_manual( you, turret );

    if( !trajectory.empty() ) {
        turret.fire( you, trajectory.back() );
    }
    g->reenter_fullscreen();
}

void avatar_action::mend( avatar &you, item_location loc )
{
    if( !loc ) {
        if( you.is_armed() ) {
            loc = item_location( you, &you.weapon );
        } else {
            add_msg( m_info, _( "You're not wielding anything." ) );
            return;
        }
    }

    if( you.has_item( *loc ) ) {
        you.mend_item( item_location( loc ) );
    }
}

bool avatar_action::eat_here( avatar &you )
{
    map &here = get_map();
    if( ( you.has_active_mutation( trait_RUMINANT ) || you.has_active_mutation( trait_GRAZER ) ) &&
        ( here.ter( you.pos() ) == t_underbrush || here.ter( you.pos() ) == t_shrub ) ) {
        item food( itype_underbrush, calendar::turn, 1 );
        if( you.get_stored_kcal() > you.max_stored_kcal() -
            food.get_comestible()->default_nutrition.kcal ) {
            add_msg( _( "You're too full to eat the leaves from the %s." ), here.ter( you.pos() )->name() );
            return true;
        } else {
            you.moves -= 400;
            here.ter_set( you.pos(), t_grass );
            add_msg( _( "You eat the underbrush." ) );
            you.eat( food );
            return true;
        }
    }
    if( you.has_active_mutation( trait_GRAZER ) && ( here.ter( you.pos() ) == t_grass ||
            here.ter( you.pos() ) == t_grass_long || here.ter( you.pos() ) == t_grass_tall ) ) {
        item food( item( itype_grass, calendar::turn, 1 ) );
        if( you.get_stored_kcal() > you.max_stored_kcal() -
            food.get_comestible()->default_nutrition.kcal ) {
            add_msg( _( "You're too full to graze." ) );
            return true;
        } else {
            you.moves -= 400;
            add_msg( _( "You eat the grass." ) );
            you.eat( food );
            if( here.ter( you.pos() ) == t_grass_tall ) {
                here.ter_set( you.pos(), t_grass_long );
            } else if( here.ter( you.pos() ) == t_grass_long ) {
                here.ter_set( you.pos(), t_grass );
            } else {
                here.ter_set( you.pos(), t_dirt );
            }
            return true;
        }
    }
    if( you.has_active_mutation( trait_GRAZER ) ) {
        if( here.ter( you.pos() ) == t_grass_golf ) {
            add_msg( _( "This grass is too short to graze." ) );
            return true;
        } else if( here.ter( you.pos() ) == t_grass_dead ) {
            add_msg( _( "This grass is dead and too mangled for you to graze." ) );
            return true;
        } else if( here.ter( you.pos() ) == t_grass_white ) {
            add_msg( _( "This grass is tainted with paint and thus inedible." ) );
            return true;
        }
    }
    return false;
}

void avatar_action::eat( avatar &you )
{
    item_location loc = game_menus::inv::consume( you );
    avatar_action::eat( you, loc );
}

void avatar_action::eat( avatar &you, item_location loc )
{
    if( !loc ) {
        you.cancel_activity();
        add_msg( _( "Never mind." ) );
        return;
    }
    item *it = loc.get_item();
    if( loc.where() == item_location::type::character ) {
        you.consume( loc );

    } else if( you.consume_item( *it ) ) {
        if( it->is_food_container() || !you.can_consume_as_is( *it ) ) {
            it->remove_item( it->contents.front() );
            add_msg( _( "You leave the empty %s." ), it->tname() );
        } else {
            loc.remove_item();
        }
    }
    if( g->u.get_value( "THIEF_MODE_KEEP" ) != "YES" ) {
        g->u.set_value( "THIEF_MODE", "THIEF_ASK" );
    }
}

void avatar_action::plthrow( avatar &you, item_location loc,
                             const cata::optional<tripoint> &blind_throw_from_pos )
{
    if( you.has_active_mutation( trait_SHELL2 ) ) {
        add_msg( m_info, _( "You can't effectively throw while you're in your shell." ) );
        return;
    }
    if( you.is_mounted() ) {
        monster *mons = get_player_character().mounted_creature.get();
        if( mons->has_flag( MF_RIDEABLE_MECH ) ) {
            if( !mons->check_mech_powered() ) {
                add_msg( m_bad, _( "Your %s refuses to move as its batteries have been drained." ),
                         mons->get_name() );
                return;
            }
        }
    }

    if( !loc ) {
        loc = game_menus::inv::titled_menu( you,  _( "Throw item" ),
                                            _( "You don't have any items to throw." ) );
    }

    if( !loc ) {
        add_msg( _( "Never mind." ) );
        return;
    }
    // make a copy and get the original.
    // the copy is thrown and has its and the originals charges set appropiately
    // or deleted from inventory if its charges(1) or not stackable.
    item thrown = *loc.get_item();
    int range = you.throw_range( thrown );
    if( range < 0 ) {
        add_msg( m_info, _( "You don't have that item." ) );
        return;
    } else if( range == 0 ) {
        add_msg( m_info, _( "That is too heavy to throw." ) );
        return;
    }

    if( you.is_wielding( *loc ) && loc->has_flag( flag_NO_UNWIELD ) ) {
        // pos == -1 is the weapon, NO_UNWIELD is used for bio_claws_weapon
        add_msg( m_info, _( "That's part of your body, you can't throw that!" ) );
        return;
    }

    if( you.has_effect( effect_relax_gas ) ) {
        if( one_in( 5 ) ) {
            add_msg( m_good, _( "You concentrate mightily, and your body obeys!" ) );
        } else {
            you.moves -= rng( 2, 5 ) * 10;
            add_msg( m_bad, _( "You can't muster up the effort to throw anything…" ) );
            return;
        }
    }
    // if you're wearing the item you need to be able to take it off
    if( you.is_wearing( loc->typeId() ) ) {
        ret_val<bool> ret = you.can_takeoff( *loc );
        if( !ret.success() ) {
            add_msg( m_info, "%s", ret.c_str() );
            return;
        }
    }
    // you must wield the item to throw it
    // But only if you don't have enough free hands
    int usable_hands = you.get_working_arm_count() -
                       ( you.is_armed() ? 1 : 0 ) -
                       ( you.weapon.is_two_handed( you ) ? 1 : 0 );
    if( !you.is_wielding( *loc ) &&
        ( usable_hands < ( loc->is_two_handed( you ) ? 2 : 1 ) ) ) {
        if( !you.wield( *loc ) ) {
            add_msg( m_info, _( "You do not have enough free hands to throw %s without wielding it." ),
                     loc->tname() );
            return;
        }

        loc = item_location( you, &you.weapon );
    }

    // Shift our position to our "peeking" position, so that the UI
    // for picking a throw point lets us target the location we couldn't
    // otherwise see.
    const tripoint original_player_position = you.pos();
    if( blind_throw_from_pos ) {
        you.setpos( *blind_throw_from_pos );
    }

    target_handler::trajectory trajectory = target_handler::mode_throw( you, *loc,
                                            blind_throw_from_pos.has_value() );

    // If we previously shifted our position, put ourselves back now that we've picked our target.
    if( blind_throw_from_pos ) {
        you.setpos( original_player_position );
    }

    if( trajectory.empty() ) {
        return;
    }

    if( &*loc != &you.weapon ) {
        // This is to represent "implicit offhand wielding"
        int extra_cost = you.item_handling_cost( *loc, true, INVENTORY_HANDLING_PENALTY / 2 );
        you.mod_moves( -extra_cost );
    }

    if( loc->count_by_charges() && loc->charges > 1 ) {
        loc->mod_charges( -1 );
        thrown.charges = 1;
    } else {
        loc.remove_item();
    }
    you.throw_item( trajectory.back(), thrown, blind_throw_from_pos );
    g->reenter_fullscreen();
}

static void make_active( item_location loc )
{
    map &here = get_map();
    switch( loc.where() ) {
        case item_location::type::map:
            here.make_active( loc );
            break;
        case item_location::type::vehicle:
            here.veh_at( loc.position() )->vehicle().make_active( loc );
            break;
        default:
            break;
    }
}

static void update_lum( item_location loc, bool add )
{
    switch( loc.where() ) {
        case item_location::type::map:
            get_map().update_lum( loc, add );
            break;
        default:
            break;
    }
}

void avatar_action::use_item( avatar &you )
{
    item_location loc;
    avatar_action::use_item( you, loc );
}

void avatar_action::use_item( avatar &you, item_location &loc )
{
    // Some items may be used without being picked up first
    bool use_in_place = false;

    if( !loc ) {
        loc = game_menus::inv::use( you );

        if( !loc ) {
            add_msg( _( "Never mind." ) );
            return;
        }

        if( loc->has_flag( flag_ALLOWS_REMOTE_USE ) ) {
            use_in_place = true;
        } else {
            const int obtain_cost = loc.obtain_cost( you );
            loc = loc.obtain( you );
            if( !loc ) {
                debugmsg( "Failed to obtain target item" );
                return;
            }

            // TODO: the following comment is inaccurate and this mechanic needs to be rexamined
            // This method only handles items in the inventory, so refund the obtain cost.
            you.mod_moves( obtain_cost );
        }
    }

    if( use_in_place ) {
        update_lum( loc, false );
        you.use( loc );
        update_lum( loc, true );

        make_active( loc );
    } else {
        you.use( loc );
    }

    you.invalidate_crafting_inventory();
}

// Opens up a menu to Unload a container, gun, or tool
// If it's a gun, some gunmods can also be loaded
void avatar_action::unload( avatar &you )
{
    item_location loc = g->inv_map_splice( [&you]( const item & it ) {
        return you.rate_action_unload( it ) == hint_rating::good;
    }, _( "Unload item" ), 1, _( "You have nothing to unload." ) );

    if( !loc ) {
        add_msg( _( "Never mind." ) );
        return;
    }

    you.unload( loc );
}
