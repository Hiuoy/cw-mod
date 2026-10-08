// mapkit stand-in for scripts/zm/zm_silver_zones.gsc. DM's is linked on its own (a script_using entry), and
// its autoexec sets up DM's zone lists, a spawn callback and the zone challenges. cwlink build generates this file
// from the map source instead (its MkZones and doors; the result is kept in <map folder>/generated/); this copy
// only keeps the level folder compiling on its own: one start zone, "start_zone".
// Build: acts gscc -g cw --name scripts/zm/zm_silver_zones.gsc

#using scripts\core_common\flag_shared;
#using scripts\zm_common\zm_zonemgr;

#namespace mapkit_zones;

function start_zones()
{
    return array( "start_zone" );
}

function init()
{
    level flag::init( "always_on" );
    level flag::set( "always_on" );
    zm_zonemgr::zone_init( "start_zone" );
}

// The level's settings from the map's objects (an exfil, crafting items turned off): none here.
function settings()
{
}
