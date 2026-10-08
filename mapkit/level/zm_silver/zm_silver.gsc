// -----------------------------------------------------------------------------
// mapkit level script (server). A map of its own (cwlink build, the default since 2026-09-29) starts under its
// own name, and cwlink compiles this file as scripts/zm/<id>.gsc, the level script the engine runs for that name;
// an empty stand-in goes under scripts/zm/zm_silver.gsc. An overlay (cwlink build --overlay) serves this file in
// place of scripts/zm/zm_silver.gsc, and the rest of this header describes that older form.
//
// A mapkit map loads under Die Maschine's name, because that is what loads DM's asset library (AI,
// weapons, FX, sounds, materials). The engine then links the level scripts listed by zm_silver's
// script_using assets (24 B {name, gsc, csc}; zm_silver lists zm_silver.gsc/.csc, zm_silver_ffotd.gsc/.csc,
// zm_silver_zones.gsc, zm_silver_fixup.gsc/.csc and the AI/bgb/vehicle scripts). cwlink ships this file
// and the stand-ins next to it in cw-mod/maps/zm_silver/scripts, and the GSC loader serves them by name.
//
// Kept from DM's main: the asset-library setup (clearance ceiling, FX, the zombie crawl entry, the announcer;
// DM's weapon table comes from settings() when DM's zone loads) and the ZM boot (load::main, the minimap, the
// zone manager). Since P6 step 2b-2 only DM's techset zone loads under the map (docs/mapkit-plan.md).
// Dropped: the intro cinematic (how it works and how to bring one back: docs/mapkit-plan.md "The intro
// cinematic"), the main, Pack-a-Punch and wonder-weapon quests, exfil, intel, VO, music, dog rounds, the
// side quest and every DM clientfield. Every include below is also in zm_silver.csc where DM had it on both
// sides: both VMs must register the same clientfields (see zm_silver.csc for the two cases that are not
// visible from the includes: zm_fasttravel.gsc still links the zone challenges, and one AI field).
//
// P1 step 2: the level's entity and trigger lists are the map's own (cwlink ComposeLevelEntities), and its zones
// are the map's MkZones: volume zones, in zm_silver_zones.gsc, which cwlink build generates from the map source
// (namespace mapkit_zones: start_zones() and init()).
//
// Build: cwlink build compiles it (ACTS: acts gscc -g cw --name scripts/zm/zm_silver.gsc).
// -----------------------------------------------------------------------------

#using scripts\core_common\array_shared;
#using scripts\core_common\callbacks_shared;
#using scripts\core_common\clientfield_shared;
#using scripts\core_common\compass;
#using scripts\core_common\exploder_shared;
#using scripts\core_common\flag_shared;
#using scripts\core_common\load_shared;
#using scripts\core_common\scene_shared;
#using scripts\core_common\scriptmodels_shared;
#using scripts\core_common\spawner_shared;
#using scripts\core_common\struct;
#using scripts\core_common\util_shared;
#using scripts\zm\ai\zm_ai_steiner;
#using scripts\zm\zm_silver_zones;
#using scripts\zm_common\callbacks;
#using scripts\zm_common\gametypes\zm_gametype;
#using scripts\zm_common\util\ai_dog_util;
#using scripts\zm_common\zm;
#using scripts\zm_common\zm_audio;
#using scripts\zm_common\zm_contracts;
#using scripts\zm_common\zm_fasttravel;
#using scripts\zm_common\zm_flashlight;
#using scripts\zm_common\zm_hazard;
#using scripts\zm_common\zm_intel;
#using scripts\zm_common\zm_loadout;
#using scripts\zm_common\zm_magicbox;
#using scripts\zm_common\zm_perks;
#using scripts\zm_common\zm_round_spawning;
#using scripts\zm_common\zm_spawner;
#using scripts\zm_common\zm_utility;
#using scripts\zm_common\zm_zonemgr;

#namespace mapkit_level;

function autoexec opt_in()
{
    level.aat_in_use = 1;
    level.random_pandora_box_start = 1;
    level.var_5470be1c = 1;                                   // bgb newtonian negation, as DM (both VMs)
    level.var_e2f95698 = #"hash_20902988a95a6003";            // announcer (globallogic_audio, zm_vo)
    level.var_462ca9bb = #"blops_taacom";
    setdvar( #"player_shallowwaterwadescale", 1 );
    setdvar( #"player_waistwaterwadescale", 1 );
    setdvar( #"player_deepwaterwadescale", 1 );
}

function event_handler[level_init] main( eventstruct )
{
    setclearanceceiling( 29 );
    zm::init_fx();
    level.default_start_location = mapkit_zones::start_zones()[ 0 ];
    level.default_game_mode = "zclassic";
    level._allow_melee_weapon_switching = 1;
    level.custom_spawner_entry[ #"crawl" ] = &zm_spawner::function_45bb11e4;
    // DM's weapon spec table (level.var_d0ab70a2, zm_weapons) is in DM's zone: settings() sets it when that zone loads.
    // Every perk machine starts powered (zm_perks::get_perk_machine_start_state), unless the map has a power switch:
    // then settings() sets this back to 0 and the machines wait for it.
    level.vending_machines_powered_on_at_start = 1;
    mapkit_zones::settings();                                 // the exfil, power, crafting items, props
    clientfield::register_clientuimodel( "player_lives", 1, 2, "int" );
    load::main();
    compass::setupminimap( "" );
    level.zones = [];
    level.zone_manager_init_func = &mapkit_zones::init;
    level thread zm_zonemgr::manage_zones( mapkit_zones::start_zones() );
    level.var_bb6bf2e0 = 1;                                   // zm_blockers, as DM
    level thread zm_perks::spare_change();
    setdvar( #"hkai_pathfinditerationlimit", 1050 );
    level thread announce();
}

// Proof in client.log (the script print mirror) that this script, not DM's, runs the level.
function private announce()
{
    level flag::wait_till( "initial_blackscreen_passed" );
    iprintlnbold( "mapkit: level script running, start zone " + mapkit_zones::start_zones()[ 0 ] + ", " + level.zones.size + " zones" );
}
