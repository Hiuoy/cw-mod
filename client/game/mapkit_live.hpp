#pragma once
// mapkit live-state dump (plan P0, docs/mapkit-plan.md): the bytes the engine holds for an asset AFTER it has
// loaded, linked and drawn, as opposed to the bytes mapkit wrote into the zone. Two uses:
//   - ours vs retail, in memory: a field the engine fills in for a retail asset but not for ours is the lead
//     (first case: mapkit's models never drew while a retail one did; the cause was outside the model, no
//     bgcache listed them, so the dump also reports each model's bgcache entry and every loaded bgcache);
//   - file vs live, offline: every part is also written to a .bin, so ffinfo can compare it with the zone's
//     own bytes and list the fields the engine fills at runtime.
//
// One dump = <game>/cw-mod/mapkit/live/<HHMMSS>_<tag>/ holding one <label>.bin per part plus manifest.txt
// ("label address size", one line each), and a "live diff" block in client.log for every part both models
// have. Parts of a model: the xmodel, its 96-B block, each LOD with its mesh info, surfaces, 96-B block and
// the first 256 bytes of its buffer, each LOD's material table and every distinct material (techset, image
// table, first image). Plus the world roots: the gfx_map the renderer draws, and every clip map pool item.
// Reads only, every read guarded (SafeRead / SafeCopy); any thread.

#include <cstdint>
#include <string>
#include <utility>

namespace Client::Game::MapKit::Live {
	// Called by MapKit::Init once the module base is known.
	void Init(std::uintptr_t moduleBase);

	// Dumps the live state now. tag names the folder (letters, digits, '_').
	void Dump(const std::string& tag);

	// R_InitWorld: dumps at world start and schedules a second dump 20 s later, when the match has drawn.
	// Only for mapkit's own world (ownWorld), where the models are ours.
	void OnWorldStart(bool ownWorld);
	// Game thread, every frame: runs the scheduled dump.
	void Tick();

	// The xmodels a dump compares, comma-separated, by name ("mapkit_zm_test") or name hash ("0x3ED36494E93F3CBD"):
	// ours[i] against retail[i], or against the last retail one. Defaults: the Godot test map's draw test row
	// (cwlink build --draw-test: the retail door written by mapkit, and two cubes) and its brush model, all against
	// the retail door.
	void SetModels(std::string ours, std::string retail);
	std::pair<std::string, std::string> Models();
	// The folder of the last dump, "" before the first.
	std::string LastDump();
}
