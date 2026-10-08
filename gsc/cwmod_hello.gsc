// -----------------------------------------------------------------------------
// cw-mod — first custom GSC script (Phase 2, "prove the pipe").
//
// Minimal, self-contained proof that community-authored GSC compiles for T9 and
// runs inside Black Ops Cold War Zombies via the cw-mod loader. Idioms (autoexec
// system::register, level "connected" notify, function refs, iprintlnbold) are
// taken straight from the game's own scripts — see ate47/bocw-source
// scripts/zm_common/callbacks.gsc.
//
// Compile (ACTS, Cold War) and drop the .gscc into <game>/cw-mod/scripts/ under any
// file name. The loader injects it into every ZM match (see client/scripting/scripting.hpp):
//   acts gscc -g cw -o cwmod_hello <dir containing this file>
// -----------------------------------------------------------------------------

#using scripts\core_common\system_shared;

#namespace cwmod;

// autoexec runs the instant this script object is LINKED into the VM. If you never
// see the "Handler calls" counter move after loading, this autoexec never ran — the
// script you replaced is loaded as data but not executed by the engine; pick a
// different target.
function private autoexec __init__system__()
{
    // EXECUTION BEACON: a cross-script function reference (&otherNamespace::func) is
    // a lazylink (VM opcode 0x13). The instant this line runs, the mod's LazyLink
    // handler fires and "Handler calls" climbs — a definitive "our bytecode ran"
    // signal that does NOT depend on players, HUD, or timing. Keep it even after the
    // on-screen path works; it's the cheapest proof of life.
    beacon = &system::register;
    beacon = beacon; // silence "unused" — the reference itself is what emits 0x13

    // Belt-and-suspenders: register through the game's own system pass...
    system::register( #"cwmod", &init, undefined, undefined, undefined );

    // ...AND kick a self-contained watcher immediately, in case the system pass
    // already ran (ordering depends on which script we replaced).
    level thread announce_loop();
}

function private init()
{
    level thread announce_loop();
}

// No dependence on the one-shot "connected" notify: just broadcast on a loop to
// whoever is in the game. First visible line proves the full pipe end to end.
function private announce_loop()
{
    level endon( #"end_game" );

    // Only one loop even though both autoexec and init may call us.
    if ( isdefined( level.cwmod_running ) )
        return;
    level.cwmod_running = true;

    // String literals work: the loader interns them before the engine sees the script.
    counter = 0;
    for ( ;; )
    {
        wait 3;
        counter++;
        players = GetPlayers();
        foreach ( player in players )
        {
            player iprintlnbold( counter );   // renders (proof the pipe is live)
        }
    }
}
