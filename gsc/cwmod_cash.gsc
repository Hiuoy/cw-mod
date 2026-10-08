// -----------------------------------------------------------------------------
// cw-mod — cash drip (Phase 2 demo).
//
// Gives every player 1000 points every 3 seconds.
//
//   acts gscc -g cw -o cwmod_cash <dir containing this file>
//   copy the .gscc into <game>/cw-mod/scripts/ (any file name); it is injected into every ZM match.
// -----------------------------------------------------------------------------

#using scripts\core_common\system_shared;

// VERIFY THIS against ate47/bocw-source scripts/zm_common/zm_score.gsc — the path
// and the add function name are the two things most likely to differ by build.
#using scripts\zm_common\zm_score;

#namespace cwmod_cash;

function private autoexec __init__system__()
{
    // Execution beacon (VM opcode 0x13 lazylink) — proves our bytecode linked/ran
    // even before any cash lands. Same trick as cwmod_hello.
    beacon = &system::register;
    beacon = beacon;

    level thread cash_loop();
}

function private cash_loop()
{
    level endon( #"end_game" );

    // Idempotent: only one loop, no matter how many times we get called.
    if ( isdefined( level.cwmod_cash_running ) )
        return;
    level.cwmod_cash_running = true;

    for ( ;; )
    {
        wait 3;

        players = GetPlayers();
        foreach ( player in players )
        {
            // ---- THE ONE LINE TO VERIFY -------------------------------------
            // Preferred: go through the game's own score API so the HUD, banking,
            // and any listeners all stay consistent.
            player zm_score::add_to_player_score( 1000 );

            // Fallback if the namespaced call won't resolve on your build — set the
            // field directly (uncomment, and comment the call above):
            // player.score += 1000;
            // -----------------------------------------------------------------
        }
    }
}
