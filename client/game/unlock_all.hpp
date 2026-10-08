#pragma once
// "unlock_all": true in cw-mod.json. Everything the menus and the match treat as locked is answered
// "unlocked" or "owned": weapons, attachments and equipment behind a level, attachments behind a gun
// level, camos and reticles behind their challenges, and everything behind the store inventory
// (blueprints, bundles, operators, battle pass rewards, challenge-reward weapons), plus the battle pass
// itself (owned, top tier).
//
// How: the game asks a handful of predicates (dump_anchors.hpp names each one and what it reads), and
// each has a detour that calls the original and changes only a "locked" / "not owned" answer. Nothing
// is written to a save file or to the engine's stats, so turning the key off brings every lock back as
// the save has it. Progress keeps recording as usual underneath.
//
// Not changed: which items exist, the player level and gun levels shown, the trial state, and seven
// Zombies rewards the menu Lua reads straight from save data.
//
// Off by default. On any boot profile; the predicates are the same in LAN, offline and online.

#include <cstdint>

namespace Client::Game::UnlockAll {
	// Reads the setting and resolves the engine calls. Call once, after ArxanCall is initialised and
	// before the hooks go in. Logs its decision.
	void Init(std::uintptr_t moduleBase, std::size_t imageSize);

	// On for this boot: "unlock_all" is true and everything resolved.
	bool Enabled();

	// Builds the Arxan thunks the detours call their originals through. Call once, right after the
	// detours are installed and before the game can call them. Defined in UnlockAll_Hooks.cpp.
	void BindHookThunks();

	// What a detour answered for. The order is the order of the log's summary line.
	enum class Kind {
		Item,            // Progression_IsItemLocked
		Attachment,      // Progression_IsAttachmentLockedInBlock
		AttachmentSlot,  // Progression_IsAttachmentSlotLocked
		WeaponOption,    // Progression_IsItemOptionLockedCore (camos, reticles)
		LevelRule,       // Com_SessionMode_IsProgressionExemptContext, asked by the unlockables module
		Inventory,       // Inventory_GetItemQuantity
		Loot,            // Loot_GetItemQuantity
		Entitlement,     // Entitlement_IsOwned
		InventoryReady,  // DwFetch_IsInventoryReady
		BattlePassOwned, // Loot_GetBattlePassOwned
		BattlePassTier,  // Loot_GetBattlePassRank
		Count
	};

	// While one is alive on this thread, every detour here passes through: the detour that made it is
	// asking the engine for its own answer, and the engine's nested questions must get real ones too.
	struct RealScope {
		RealScope();
		~RealScope();
		RealScope(const RealScope&) = delete;
		RealScope& operator=(const RealScope&) = delete;
	};
	// Enabled, and no RealScope on this thread. Asked first by every detour.
	bool Active();

	// Counts one question of `kind`; `changed` when the engine's answer was replaced. The first few
	// changed ones of each kind are logged with `a`, `b` (what was asked about) and the caller.
	void Note(Kind kind, bool changed, std::uint64_t a, std::uint64_t b, std::uintptr_t returnAddress);

	// The caller is one of the unlockables module's rule sites / is DwFetch_GetStatus.
	bool CallerInUnlockables(std::uintptr_t returnAddress);
	bool CallerIsFetchStatus(std::uintptr_t returnAddress);
	// An inventory item that may be reported as owned: a real controller, a nonzero id, and not one of
	// the three items the trial state is computed from.
	bool IsOwnableItem(std::int64_t controller, std::uint64_t itemId);
	// The weapon has that attachment slot (the engine reports a slot it does not have as locked).
	bool AttachmentSlotExists(int mode, unsigned int itemIndex, int slot);
	// The controller is signed in far enough to have a loot block; the battle pass getters need it.
	bool HasLootBlock(unsigned int controller);

	// LUI_RunFile detour, after ui/main.lua: the menus were just rebuilt, so the battle pass model is
	// filled again on the next tick.
	void OnUiLoaded();
	// Game thread, every frame. Refreshes the battle pass model when it is due and writes the log's
	// summary line.
	void Tick();

	struct Status {
		bool enabled = false;
		std::uint64_t asked = 0;     // lock / ownership questions seen
		std::uint64_t changed = 0;   // of those, answered "unlocked" / "owned" instead
	};
	Status ReadStatus();             // any thread
}
