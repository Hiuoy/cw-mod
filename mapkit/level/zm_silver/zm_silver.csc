// -----------------------------------------------------------------------------
// mapkit level script (client): scripts/zm/<id>.csc for a map of its own, in place of scripts/zm/zm_silver.csc for
// an overlay. See zm_silver.gsc.
//
// It registers exactly the clientfields zm_silver.gsc registers (player_lives), and includes the
// systems zm_silver.gsc includes that have a client half (steiner, flashlight, fast travel, intel, dogs).
//
// Both VMs must end up with the same clientfield list, or the client is kicked with "Clientfield
// Mismatch" once the map has loaded. The list is everything linked on each side, not only what these
// two files include, so a server include can pull in fields whose client half nothing links. The GSC
// loader logs the difference at every ClientField_Shutdown ("Clientfield diff" in client.log). Boot
// 2026-09-26 20:58 found two such cases, both handled below:
//   - zm_fasttravel.gsc includes zm_dac_challenges.gsc (the zone challenges: 16 fields), while
//     zm_fasttravel.csc does not include its client half; DM's zm_silver.csc included it directly.
//   - one actor field (hash_c5d06ae18fde4c0) comes from a hashed server script that some linked AI script
//     includes. Its client half (script_581877678e31274c.csc) would also bring the jammer gadget's
//     fields, which the server may not have, so the field is registered here instead.
//
// Build: cwlink build compiles it (ACTS: acts gscc -g cw --name-client scripts/zm/zm_silver.csc).
// -----------------------------------------------------------------------------

#using scripts\core_common\ai\zombie;
#using scripts\core_common\ai_shared;
#using scripts\core_common\audio_shared;
#using scripts\core_common\callbacks_shared;
#using scripts\core_common\clientfield_shared;
#using scripts\core_common\load_shared;
#using scripts\core_common\util_shared;
#using scripts\zm\ai\zm_ai_steiner;
#using scripts\zm_common\util\ai_dog_util;
#using scripts\zm_common\zm_dac_challenges;
#using scripts\zm_common\zm_fasttravel;
#using scripts\zm_common\zm_flashlight;
#using scripts\zm_common\zm_intel;

#namespace mapkit_level;

function autoexec opt_in()
{
    level.aat_in_use = 1;
    level.var_5470be1c = 1;
}

function event_handler[level_init] main( eventstruct )
{
    clientfield::register_clientuimodel( "player_lives", #"zm_hud", #"player_lives", 1, 2, "int", undefined, 0, 0 );
    clientfield::register( "actor", "" + #"hash_c5d06ae18fde4c0", 1, 1, "int", undefined, 0, 0 );   // see the header
    setsoundcontext( "dark_aether", "inactive" );
    level.var_a396a670 = 1;                                   // remotemissile, as DM
    setdvar( #"player_shallowwaterwadescale", 1 );
    setdvar( #"player_waistwaterwadescale", 1 );
    setdvar( #"player_deepwaterwadescale", 1 );
    load::main();
    util::function_89a98f85();
}
