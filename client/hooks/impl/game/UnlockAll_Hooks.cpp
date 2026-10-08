#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/arxan_call.hpp"
#include "game/dump_anchors.hpp"
#include "game/unlock_all.hpp"

#include <intrin.h>

// The detours behind "unlock_all". The reasoning is in game/unlock_all.hpp; what each engine function
// decides and who asks it is in game/dump_anchors.hpp.
//
// Each one passes through unless the feature is active on this thread, gets the engine's own answer
// inside a RealScope (so the engine's nested questions are not answered by another detour here), and
// replaces only a "locked" / "not owned" answer. Both outcomes are counted, so the log tells a lock
// nobody asked about from one the engine already had open.

namespace {
	namespace Unlock = Client::Game::UnlockAll;
	using Kind = Unlock::Kind;
	using H = Client::Hook::Hooks;

	template <typename Fn>
	Fn* Thunked(Fn* trampoline, const char* name) {
		auto* const thunk = Client::Game::ArxanCall::MakeThunk<Fn>(reinterpret_cast<void*>(trampoline));
		if (!thunk) {
			LOG("UnlockAll", ERROR, "{}: no Arxan thunk for the trampoline; calling it directly, which a "
				"caller-guarded function would silently skip.", name);
		}
		return thunk ? thunk : trampoline;
	}

	// Built once, at install time, on the installing thread (see ZmProgression_Hooks.cpp). The two
	// five-argument detours are not here: a thunk forwards four register arguments, so those call
	// their trampoline directly.
	decltype(H::HK_Progression_IsItemLocked::m_Original) g_IsItemLocked{};
	decltype(H::HK_Progression_IsAttachmentSlotLocked::m_Original) g_IsAttachmentSlotLocked{};
	decltype(H::HK_Com_SessionMode_IsProgressionExemptContext::m_Original) g_IsExemptContext{};
	decltype(H::HK_Inventory_GetItemQuantity::m_Original) g_InventoryQuantity{};
	decltype(H::HK_Loot_GetItemQuantity::m_Original) g_LootQuantity{};
	decltype(H::HK_Entitlement_IsOwned::m_Original) g_EntitlementOwned{};
	decltype(H::HK_DwFetch_IsInventoryReady::m_Original) g_InventoryReady{};
	decltype(H::HK_Loot_GetBattlePassOwned::m_Original) g_BattlePassOwned{};
	decltype(H::HK_Loot_GetBattlePassRank::m_Original) g_BattlePassRank{};

	std::uintptr_t Caller(void* returnAddress) { return reinterpret_cast<std::uintptr_t>(returnAddress); }
}

void Client::Game::UnlockAll::BindHookThunks() {
	g_IsItemLocked = Thunked(H::HK_Progression_IsItemLocked::m_Original, "Progression_IsItemLocked");
	g_IsAttachmentSlotLocked = Thunked(H::HK_Progression_IsAttachmentSlotLocked::m_Original,
		"Progression_IsAttachmentSlotLocked");
	g_IsExemptContext = Thunked(H::HK_Com_SessionMode_IsProgressionExemptContext::m_Original,
		"Com_SessionMode_IsProgressionExemptContext");
	g_InventoryQuantity = Thunked(H::HK_Inventory_GetItemQuantity::m_Original, "Inventory_GetItemQuantity");
	g_LootQuantity = Thunked(H::HK_Loot_GetItemQuantity::m_Original, "Loot_GetItemQuantity");
	g_EntitlementOwned = Thunked(H::HK_Entitlement_IsOwned::m_Original, "Entitlement_IsOwned");
	g_InventoryReady = Thunked(H::HK_DwFetch_IsInventoryReady::m_Original, "DwFetch_IsInventoryReady");
	g_BattlePassOwned = Thunked(H::HK_Loot_GetBattlePassOwned::m_Original, "Loot_GetBattlePassOwned");
	g_BattlePassRank = Thunked(H::HK_Loot_GetBattlePassRank::m_Original, "Loot_GetBattlePassRank");
}

// Engine.IsItemLocked and every native reader of an item's lock: weapons, equipment, perks, wildcards.
template <>
bool Client::Hook::Hooks::HK_Progression_IsItemLocked::hkCallback(int mode, unsigned int controller, int itemIndex) {
	auto* const original = g_IsItemLocked ? g_IsItemLocked : m_Original;
	if (!Unlock::Active()) return original(mode, controller, itemIndex);
	bool locked = false;
	{
		Unlock::RealScope real;
		locked = original(mode, controller, itemIndex);
	}
	Unlock::Note(Kind::Item, locked, static_cast<std::uint64_t>(mode), static_cast<std::uint64_t>(itemIndex),
		Caller(_ReturnAddress()));
	return false;
}

// Engine.IsItemAttachmentLocked and the server-side twin. A slot the weapon does not have stays locked.
template <>
char Client::Hook::Hooks::HK_Progression_IsAttachmentLockedInBlock::hkCallback(
	int mode, void* statsBlock, unsigned int itemIndex, int slot, char a5) {

	if (!Unlock::Active()) return m_Original(mode, statsBlock, itemIndex, slot, a5);
	char locked = 0;
	{
		Unlock::RealScope real;
		locked = m_Original(mode, statsBlock, itemIndex, slot, a5);
	}
	const bool open = locked && Unlock::AttachmentSlotExists(mode, itemIndex, slot);
	Unlock::Note(Kind::Attachment, open, itemIndex, static_cast<std::uint64_t>(slot), Caller(_ReturnAddress()));
	return open ? 0 : locked;
}

// Engine.IsAttachmentSlotLocked: the slot's gun level.
template <>
bool Client::Hook::Hooks::HK_Progression_IsAttachmentSlotLocked::hkCallback(
	unsigned int mode, unsigned int controller, unsigned int itemIndex, int slot) {

	auto* const original = g_IsAttachmentSlotLocked ? g_IsAttachmentSlotLocked : m_Original;
	if (!Unlock::Active()) return original(mode, controller, itemIndex, slot);
	bool locked = false;
	{
		Unlock::RealScope real;
		locked = original(mode, controller, itemIndex, slot);
	}
	Unlock::Note(Kind::AttachmentSlot, locked, itemIndex, static_cast<std::uint64_t>(slot), Caller(_ReturnAddress()));
	return false;
}

// Camos and reticles: the option's challenge against its target. The original asks itself about a
// prerequisite option; inside the RealScope that nested call passes through.
template <>
char Client::Hook::Hooks::HK_Progression_IsItemOptionLockedCore::hkCallback(
	unsigned int mode, unsigned int controller, unsigned int itemIndex, unsigned int optionIndex, char skipPrerequisite) {

	if (!Unlock::Active()) return m_Original(mode, controller, itemIndex, optionIndex, skipPrerequisite);
	char locked = 0;
	{
		Unlock::RealScope real;
		locked = m_Original(mode, controller, itemIndex, optionIndex, skipPrerequisite);
	}
	Unlock::Note(Kind::WeaponOption, locked != 0, itemIndex, optionIndex, Caller(_ReturnAddress()));
	return 0;
}

// The engine's own "progression rules are off here" test. Answered yes only to the unlockables module,
// where it switches off the level rules, the purchase checks and the "new" markers together.
template <>
bool Client::Hook::Hooks::HK_Com_SessionMode_IsProgressionExemptContext::hkCallback() {
	auto* const original = g_IsExemptContext ? g_IsExemptContext : m_Original;
	const bool exempt = original();
	if (exempt || !Unlock::Active()) return exempt;
	const std::uintptr_t caller = Caller(_ReturnAddress());
	if (!Unlock::CallerInUnlockables(caller)) return exempt;
	Unlock::Note(Kind::LevelRule, true, 0, 0, caller);
	return true;
}

// Every ownership read ends here: blueprints, bundles, operators, battle pass rewards, entitlements.
template <>
std::uint64_t Client::Hook::Hooks::HK_Inventory_GetItemQuantity::hkCallback(int controller, unsigned int itemId) {
	auto* const original = g_InventoryQuantity ? g_InventoryQuantity : m_Original;
	const std::uint64_t quantity = original(controller, itemId);
	if (!Unlock::Active() || !Unlock::IsOwnableItem(controller, itemId)) return quantity;
	const bool owned = static_cast<std::uint32_t>(quantity) != 0;
	Unlock::Note(Kind::Inventory, !owned, static_cast<std::uint64_t>(controller), itemId, Caller(_ReturnAddress()));
	return owned ? quantity : 1;
}

// The same read behind loot_enabled and "inventory loaded".
template <>
std::uint64_t Client::Hook::Hooks::HK_Loot_GetItemQuantity::hkCallback(
	unsigned int controller, std::uint64_t itemId, std::uint64_t a3, std::uint64_t a4) {

	auto* const original = g_LootQuantity ? g_LootQuantity : m_Original;
	if (!Unlock::Active()) return original(controller, itemId, a3, a4);
	std::uint64_t quantity = 0;
	{
		Unlock::RealScope real;
		quantity = original(controller, itemId, a3, a4);
	}
	const std::uint32_t id = static_cast<std::uint32_t>(itemId);
	if (!Unlock::IsOwnableItem(controller, id)) return quantity;
	const bool owned = static_cast<std::uint32_t>(quantity) != 0;
	Unlock::Note(Kind::Loot, !owned, controller, id, Caller(_ReturnAddress()));
	return owned ? quantity : 1;
}

// Engine.HasEntitlement and the entitlement-locked weapon options.
template <>
char Client::Hook::Hooks::HK_Entitlement_IsOwned::hkCallback(int controller, std::uint64_t nameHash) {
	auto* const original = g_EntitlementOwned ? g_EntitlementOwned : m_Original;
	if (!Unlock::Active()) return original(controller, nameHash);
	char owned = 0;
	{
		Unlock::RealScope real;
		owned = original(controller, nameHash);
	}
	const bool change = !owned && controller >= 0 && controller <= 1 && (nameHash & 0x7FFFFFFFFFFFFFFFULL) != 0;
	Unlock::Note(Kind::Entitlement, change, static_cast<std::uint64_t>(controller), nameHash, Caller(_ReturnAddress()));
	return change ? 1 : owned;
}

// "The marketplace inventory has loaded". It never does on this backend, and the loot getters answer
// nothing until it has. The lobby's fetch status keeps the real answer.
template <>
bool Client::Hook::Hooks::HK_DwFetch_IsInventoryReady::hkCallback(unsigned int controller) {
	auto* const original = g_InventoryReady ? g_InventoryReady : m_Original;
	const bool ready = original(controller);
	if (ready || !Unlock::Active() || controller > 1) return ready;
	const std::uintptr_t caller = Caller(_ReturnAddress());
	if (Unlock::CallerIsFetchStatus(caller)) return ready;
	Unlock::Note(Kind::InventoryReady, true, controller, 0, caller);
	return true;
}

template <>
char Client::Hook::Hooks::HK_Loot_GetBattlePassOwned::hkCallback(unsigned int controller, int season) {
	auto* const original = g_BattlePassOwned ? g_BattlePassOwned : m_Original;
	if (!Unlock::Active()) return original(controller, season);
	char owned = 0;
	{
		Unlock::RealScope real;
		owned = original(controller, season);
	}
	const bool change = !owned && Unlock::HasLootBlock(controller);
	Unlock::Note(Kind::BattlePassOwned, change, controller, static_cast<std::uint64_t>(season), Caller(_ReturnAddress()));
	return change ? 1 : owned;
}

template <>
std::uint64_t Client::Hook::Hooks::HK_Loot_GetBattlePassRank::hkCallback(unsigned int controller, int season) {
	auto* const original = g_BattlePassRank ? g_BattlePassRank : m_Original;
	if (!Unlock::Active()) return original(controller, season);
	std::uint64_t tier = 0;
	{
		Unlock::RealScope real;
		tier = original(controller, season);
	}
	const bool change = static_cast<std::uint8_t>(tier) < Client::Game::kBattlePass_TopTier
		&& Unlock::HasLootBlock(controller);
	Unlock::Note(Kind::BattlePassTier, change, controller, static_cast<std::uint64_t>(season), Caller(_ReturnAddress()));
	return change ? static_cast<std::uint64_t>(Client::Game::kBattlePass_TopTier) : tier;
}
