// -----------------------------------------------------------------------------
// cw-mod trainer: a test harness for ZM progression.
//
// In every ZM match, for each player, a few seconds after the first spawn:
//   - adds Aetherium crystals to the player's stats (committed at match end like any stat),
//   - Pack-a-Punches the current weapon up to tier 3,
//   - refills ammo every quarter second.
// Rounds and zombie spawning are left to the map.
//
// Build (ACTS, Cold War):
//   acts gscc -g cw --name scripts/cwmod/trainer.gsc -o cwmod_trainer <folder containing this file>
// then copy cwmod_trainer.gscc into <game>/cw-mod/scripts/. The loader injects it into every ZM
// match. End the match normally (or from the pause menu) so the stats commit.
//
// Every API here is the game's own, from the decompiled T9 scripts:
//   crystals  stats::inc_stat(<crystal array>, rare|epic|legendary, n), the array custom_class.csc
//             reads for the crystal counts (zm_progression.ddl: hash_65febbdf3f1ab4d7[epic,legendary,rare])
//   PAP tier  item_inventory::function_73ae3380(item, tier), what the PAP machine calls (tiers 1-3,
//             prices 5000/15000/30000 in script_6fc2be37feeb317b)
// -----------------------------------------------------------------------------

#using scripts\core_common\callbacks_shared;
#using scripts\core_common\item_inventory;
#using scripts\core_common\player\player_stats;
#using scripts\core_common\system_shared;

#namespace cwmod_trainer;

// Crystals added once per match. Raw = rare, refined = epic, flawless = legendary.
function private crystals_raw() { return 100; }
function private crystals_refined() { return 20; }
function private crystals_flawless() { return 10; }

function private autoexec __init__system__()
{
    system::register( #"cwmod_trainer", &preinit, undefined, undefined, undefined );
}

function private preinit()
{
    callback::on_spawned( &on_player_spawned );
}

function private on_player_spawned()
{
    self endon( #"disconnect" );

    if ( isdefined( self.cwmod_trainer ) )
    {
        return;
    }
    self.cwmod_trainer = 1;

    // Let the loadout and the weapon inventory finish setting up.
    wait 5;
    self iprintlnbold( "cw-mod trainer on" );

    self thread infinite_ammo();
    self thread grant_crystals();
    self thread pap_to_tier( 3 );
}

function private grant_crystals()
{
    self endon( #"disconnect" );

    self stats::inc_stat( #"hash_65febbdf3f1ab4d7", #"rare", crystals_raw() );
    self stats::inc_stat( #"hash_65febbdf3f1ab4d7", #"epic", crystals_refined() );
    self stats::inc_stat( #"hash_65febbdf3f1ab4d7", #"legendary", crystals_flawless() );

    raw = self stats::get_stat( #"hash_65febbdf3f1ab4d7", #"rare" );
    refined = self stats::get_stat( #"hash_65febbdf3f1ab4d7", #"epic" );
    flawless = self stats::get_stat( #"hash_65febbdf3f1ab4d7", #"legendary" );
    wait 2;
    self iprintlnbold( "Crystals (earned total): raw " + raw + ", refined " + refined + ", flawless " + flawless );
}

function private infinite_ammo()
{
    self endon( #"disconnect" );

    while ( true )
    {
        weapon = self getcurrentweapon();
        if ( isdefined( weapon ) && weapon != level.weaponnone )
        {
            self givemaxammo( weapon );
            self setweaponammoclip( weapon, weapon.clipsize );
        }
        wait 0.25;
    }
}

// One tier per call, like repeated trips to the machine. Tier 1 swaps in the upgraded weapon,
// so the item is looked up again every step.
function private pap_to_tier( target )
{
    self endon( #"disconnect", #"death" );

    for ( attempt = 0; attempt < 8; attempt++ )
    {
        weapon = self getcurrentweapon();
        if ( !isdefined( weapon ) || weapon == level.weaponnone )
        {
            wait 0.5;
            continue;
        }

        item = self item_inventory::function_230ceec4( weapon );
        if ( !isdefined( item ) )
        {
            wait 0.5;
            continue;
        }

        tier = 0;
        if ( isdefined( item.paplv ) )
        {
            tier = item.paplv;
        }
        if ( tier >= target )
        {
            self iprintlnbold( "Pack-a-Punch tier " + tier );
            return;
        }

        self item_inventory::function_73ae3380( item, tier + 1 );
        wait 1.5;
    }
    self iprintlnbold( "Pack-a-Punch did not reach tier " + target );
}
