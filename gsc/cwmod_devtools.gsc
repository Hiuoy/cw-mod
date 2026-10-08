// -----------------------------------------------------------------------------
// cw-mod dev tools: god mode + noclip, for walking mapkit maps.
//
// For each player, from the first spawn on:
//   - god mode: invulnerable, health refilled every 0.1 s, and the map's out-of-playable-area kill is off
//     (zm_player::player_out_of_playable_area_monitor takes a third of max health per tick outside the old
//     map's player volumes; zclassic's monitor teleports you back after 30-60 s outside),
//   - noclip: hold AIM and press MELEE to toggle. While on, you fly where you look with the movement keys;
//     sprint = faster, jump = straight up. Aim + melee again drops you where you are.
//   - mapkit check (zm_silver only, once): reports the entity that places mapkit's drawn brushes, spawns a
//     second copy of that model at the same spot, and a retail resident model (the Pack-a-Punch plinth)
//     100 units in front of you. Which of the three draw tells where the invisible walls fail.
//
// Build (ACTS, Cold War), from a folder holding only this file:
//   acts gscc -g cw --name scripts/cwmod/devtools.gsc -o cwmod_devtools <folder>
// then copy cwmod_devtools.gscc into <game>/cw-mod/scripts/.
//
// Every API here is the game's own, from the decompiled T9 scripts:
//   god mode  enableinvulnerability / getinvulnerability (values_shared "takedamage" does the same for players)
//   out-of-area  level.player_out_of_playable_area_monitor = 0 (zsurvival does this), the monitor's own
//             stop notify, and zclassic's out-of-area timer (field var_eb31aed8) held at zero
//   noclip    a script_origin the player is linked to, moved each frame (zombie_vortex's fly_ent does the same)
// -----------------------------------------------------------------------------

#using scripts\core_common\callbacks_shared;
#using scripts\core_common\system_shared;

#namespace cwmod_devtools;

function private noclip_speed() { return 25; }        // units per server frame
function private noclip_speed_sprint() { return 75; }

function private autoexec __init__system__()
{
    system::register( #"cwmod_devtools", &preinit, undefined, undefined, undefined );
}

function private preinit()
{
    // Read at every spawn by zm_player (and once by zclassic); zm::init only sets it when undefined.
    level.player_out_of_playable_area_monitor = 0;
    callback::on_spawned( &on_player_spawned );
}

function private on_player_spawned()
{
    self endon( #"disconnect" );

    if ( isdefined( self.cwmod_devtools ) )
    {
        return;
    }
    self.cwmod_devtools = 1;

    self thread god_mode();
    self thread noclip_toggle_watch();

    wait 5;
    self iprintlnbold( "cw-mod: god mode on, AIM + MELEE toggles noclip" );

    if ( level.script === "zm_silver" )
    {
        wait 3;
        self mapkit_check();
    }
}

// cwlink names the drawn-brush model "mapkit_<source map name>"; the Godot test map is zm_test.
function private mapkit_check()
{
    found = 0;
    foreach ( ent in getentarray( "script_model", "classname" ) )
    {
        if ( ent.model === #"mapkit_zm_test" )
        {
            found++;
            at = ent.origin;
            ent show();
        }
    }

    if ( found == 0 )
    {
        self iprintlnbold( "mapkit: no script_model with mapkit_zm_test" );
    }
    else
    {
        self iprintlnbold( "mapkit: " + found + " wall entity at " + at + ", spawned a copy there" );
        copy = spawn( "script_model", at );
        copy setmodel( #"mapkit_zm_test" );
    }

    fwd = anglestoforward( self getplayerangles() );
    plinth = spawn( "script_model", self.origin + ( fwd[ 0 ], fwd[ 1 ], 0 ) * 100 );
    plinth setmodel( #"p8_zm_zod_pap_plinth_sequence_air" );
    wait 2;
    self iprintlnbold( "mapkit: a Pack-a-Punch plinth should stand 100 units in front of you" );
}

function private god_mode()
{
    self endon( #"disconnect" );

    while ( true )
    {
        if ( isalive( self ) )
        {
            // Scenes and last stand reset invulnerability through val::reset, so it is re-applied.
            if ( !self getinvulnerability() )
            {
                self enableinvulnerability();
            }
            if ( isdefined( self.maxhealth ) && self.health < self.maxhealth )
            {
                self.health = self.maxhealth;
            }
        }

        // A monitor started before preinit would still run: stop it (it ends on this notify) and keep
        // zclassic's out-of-area time below its 30-60 s teleport.
        self notify( #"stop_player_out_of_playable_area_monitor" );
        self.var_eb31aed8 = 0;

        wait 0.1;
    }
}

function private noclip_toggle_watch()
{
    self endon( #"disconnect" );

    while ( true )
    {
        if ( self adsbuttonpressed() && self meleebuttonpressed() )
        {
            if ( isdefined( self.cwmod_noclip ) )
            {
                self noclip_off();
            }
            else
            {
                self thread noclip_on();
            }

            // One toggle per press.
            while ( self meleebuttonpressed() )
            {
                waitframe( 1 );
            }
        }
        waitframe( 1 );
    }
}

function private noclip_on()
{
    self endon( #"disconnect", #"cwmod_noclip_off" );

    mover = spawn( "script_origin", self.origin );
    self.cwmod_noclip = mover;
    mover thread delete_with_owner( self );
    self playerlinkto( mover );
    self iprintlnbold( "Noclip on" );

    while ( true )
    {
        speed = noclip_speed();
        if ( self sprintbuttonpressed() )
        {
            speed = noclip_speed_sprint();
        }

        // [0] = forward, [1] = right (bot::move_dir).
        move = self getnormalizedmovement();
        angles = self getplayerangles();
        delta = ( anglestoforward( angles ) * move[ 0 ] + anglestoright( angles ) * move[ 1 ] ) * speed;
        if ( self jumpbuttonpressed() )
        {
            delta += ( 0, 0, speed );
        }

        mover.origin += delta;
        waitframe( 1 );
    }
}

function private noclip_off()
{
    self notify( #"cwmod_noclip_off" );
    self unlink();
    if ( isdefined( self.cwmod_noclip ) )
    {
        self.cwmod_noclip delete();
    }
    self.cwmod_noclip = undefined;
    self iprintlnbold( "Noclip off" );
}

function private delete_with_owner( owner )
{
    self endon( #"death" );

    owner waittill( #"disconnect" );
    self delete();
}
