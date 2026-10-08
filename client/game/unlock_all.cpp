// "unlock_all". See unlock_all.hpp for what it does; dump_anchors.hpp for where each lock is decided.
#include "common.hpp"
#include "game/unlock_all.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/arxan_call.hpp"
#include "game/dump_anchors.hpp"
#include "game/settings.hpp"
#include "game/function_types.hpp"

#include <atomic>
#include <format>
#include <mutex>
#include <string>
#include <unordered_set>

namespace Client::Game::UnlockAll {
	namespace {
		namespace F = Functions;

		std::atomic_bool g_Enabled{ false };
		std::atomic_bool g_UiLoaded{ false };
		std::atomic_bool g_ModelRefresh{ false };
		thread_local int t_RealDepth = 0;

		std::uintptr_t g_ModuleBase = 0;

		// Resolved once in Init, read from any thread afterwards.
		struct Api {
			F::Dvar_GetBoolT* DvarGetBool{};
			F::Loot_UpdateBattlePassModelsT* UpdateBattlePassModels{};
			F::LiveStorage_AreStatsReadableT* StatsReadable{};
			const std::uint8_t* unlockables{};
		} g_Api;

		constexpr std::size_t kKinds = static_cast<std::size_t>(Kind::Count);
		struct KindInfo {
			const char* name;      // in the summary line
			const char* a;         // what Note's `a` is
			const char* b;
			const char* changed;   // the engine's answer and ours
		};
		constexpr KindInfo kKindInfo[kKinds] = {
			{ "items",             "mode",       "item",      "the engine says LOCKED, answered unlocked" },
			{ "attachments",       "item",       "slot",      "the engine says LOCKED, answered unlocked" },
			{ "attachment slots",  "item",       "slot",      "the engine says LOCKED, answered unlocked" },
			{ "camos and reticles", "item",      "option",    "the engine says LOCKED, answered unlocked" },
			{ "level rules",       "",           "",          "the rules apply here, answered 'rules off' (the engine's own bypass)" },
			{ "inventory",         "controller", "item id",   "quantity 0, answered 1" },
			{ "loot",              "controller", "item id",   "quantity 0, answered 1" },
			{ "entitlements",      "controller", "name hash", "not owned, answered owned" },
			{ "inventory ready",   "controller", "",          "never loaded on this backend, answered ready" },
			{ "battle pass owned", "controller", "season",    "not owned, answered owned" },
			{ "battle pass tier",  "controller", "season",    "below the top tier, answered the top tier" },
		};
		struct Counter {
			std::atomic<std::uint64_t> asked{ 0 };
			std::atomic<std::uint64_t> changed{ 0 };
			std::atomic<int> detailLines{ 0 };
		};
		Counter g_Counters[kKinds];
		constexpr int kDetailLinesPerKind = 3;

		// Distinct inventory items reported as owned, for the log.
		std::mutex g_ItemLock;
		std::unordered_set<std::uint32_t> g_ItemsSeen;
		constexpr std::size_t kMaxItemsSeen = 16384;
		constexpr std::size_t kItemLines = 12;

		std::uintptr_t Rva(std::uintptr_t address) {
			return address >= g_ModuleBase ? address - g_ModuleBase : address;
		}

		// One line for each of the first few distinct items; the summary line carries the count.
		void NoteItem(const char* reader, std::uint32_t itemId, std::uintptr_t returnAddress) {
			thread_local std::uint32_t t_Last = 0;
			if (t_Last == itemId) return;
			t_Last = itemId;
			std::size_t n = 0;
			{
				std::lock_guard lock(g_ItemLock);
				if (g_ItemsSeen.size() >= kMaxItemsSeen || !g_ItemsSeen.insert(itemId).second) return;
				n = g_ItemsSeen.size();
			}
			if (n > kItemLines) return;
			LOG("UnlockAll", INFO, "{}: item {} has quantity 0 (the inventory never loads on this backend), answered 1. "
				"Asked from {}+0x{:X}; distinct item #{}{}.", reader, itemId, g_GameModuleName, Rva(returnAddress), n,
				n == kItemLines ? ", further ones are only counted" : "");
		}

		// The pointer at LiveUser + 40024 (the AE block and the loot state), 0 while not signed in.
		std::uintptr_t LootBlock(unsigned int controller) {
			if (controller > 1 || !g_Pointers || !g_Pointers->m_g_liveUserObjects) return 0;
			std::uintptr_t user = 0, block = 0;
			if (!SafeRead(g_Pointers->m_g_liveUserObjects + controller, user) || !user) return 0;
			SafeRead(reinterpret_cast<const void*>(user + Pointers::kLiveUser_StatsContainer), block);
			return block;
		}

		std::string SummaryText() {
			std::string out;
			for (std::size_t i = 0; i < kKinds; ++i) {
				const std::uint64_t asked = g_Counters[i].asked.load(std::memory_order_relaxed);
				if (!asked) continue;
				out += std::format("{}{} {} of {}", out.empty() ? "" : ", ", kKindInfo[i].name,
					g_Counters[i].changed.load(std::memory_order_relaxed), asked);
			}
			std::size_t items = 0;
			{
				std::lock_guard lock(g_ItemLock);
				items = g_ItemsSeen.size();
			}
			return std::format("{} ({} distinct inventory item{})", out.empty() ? "nothing asked yet" : out, items,
				items == 1 ? "" : "s");
		}
	}

	void Init(std::uintptr_t moduleBase, std::size_t imageSize) {
		g_ModuleBase = moduleBase;
		if (!Settings::Get().unlockAll) {
			LOG("UnlockAll", INFO, "\"unlock_all\" is off in cw-mod.json: locks and ownership are the game's own.");
			return;
		}
		if (!ArxanCall::Ready()) {
			LOG("UnlockAll", ERROR, "no Arxan return gadget: the engine calls this needs would be silent no-ops, "
				"so unlock_all stays OFF.");
			return;
		}

		std::string missing;
		auto addr = [&](std::uintptr_t dumpAbs) -> void* {
			const std::uintptr_t a = moduleBase + (dumpAbs - kDumpImagebase);
			return (a >= moduleBase && a < moduleBase + imageSize) ? reinterpret_cast<void*>(a) : nullptr;
		};
		auto thunk = [&]<typename Fn>(Fn*& out, std::uintptr_t dumpAbs, const char* name) {
			void* a = addr(dumpAbs);
			out = a ? ArxanCall::MakeThunk<Fn>(a) : nullptr;
			if (!out) missing += std::format(" {}", name);
		};
		thunk(g_Api.DvarGetBool, kDump_Dvar_GetBool, "Dvar_GetBool");
		thunk(g_Api.UpdateBattlePassModels, kDump_Loot_UpdateBattlePassModels, "Loot_UpdateBattlePassModels");
		thunk(g_Api.StatsReadable, kDump_LiveStorage_AreStatsReadable, "LiveStorage_AreStatsReadable");
		g_Api.unlockables = static_cast<const std::uint8_t*>(addr(kDump_g_unlockableItemsByMode));
		if (!g_Api.unlockables) missing += " g_unlockableItemsByMode";
		if (!g_Pointers || !g_Pointers->m_g_liveUserObjects) missing += " g_liveUserObjects";

		if (!missing.empty()) {
			LOG("UnlockAll", ERROR, "unlock_all OFF: unresolved{}.", missing);
			return;
		}

		g_Enabled = true;
		LOG("UnlockAll", WARN, "\"unlock_all\" is ON: level and gun-level locks, camo and reticle challenges, and "
			"everything behind the store inventory (blueprints, bundles, operators, battle pass rewards) are "
			"answered unlocked or owned, and the battle pass reads owned at tier {}. Nothing is written to the "
			"save; set \"unlock_all\": false in cw-mod.json to get the real locks back.", kBattlePass_TopTier);
	}

	bool Enabled() { return g_Enabled.load(std::memory_order_relaxed); }

	RealScope::RealScope() { ++t_RealDepth; }
	RealScope::~RealScope() { --t_RealDepth; }

	bool Active() { return Enabled() && t_RealDepth == 0; }

	void Note(Kind kind, bool changed, std::uint64_t a, std::uint64_t b, std::uintptr_t returnAddress) {
		const std::size_t i = static_cast<std::size_t>(kind);
		Counter& c = g_Counters[i];
		c.asked.fetch_add(1, std::memory_order_relaxed);
		if (!changed) return;
		c.changed.fetch_add(1, std::memory_order_relaxed);

		if (kind == Kind::Inventory || kind == Kind::Loot) {
			NoteItem(kKindInfo[i].name, static_cast<std::uint32_t>(b), returnAddress);
			return;
		}
		if (c.detailLines.load(std::memory_order_relaxed) >= kDetailLinesPerKind
			|| c.detailLines.fetch_add(1, std::memory_order_relaxed) >= kDetailLinesPerKind) {
			return;
		}
		const KindInfo& info = kKindInfo[i];
		std::string what;
		if (*info.a) what += std::format(" {} {}", info.a, a);
		if (*info.b) {
			what += kind == Kind::Entitlement ? std::format(" {} 0x{:X}", info.b, b) : std::format(" {} {}", info.b, b);
		}
		LOG("UnlockAll", INFO, "{}:{} - {}. Asked from {}+0x{:X}. The first {} of a kind are listed, the rest counted.",
			info.name, what, info.changed, g_GameModuleName, Rva(returnAddress), kDetailLinesPerKind);
	}

	bool CallerInUnlockables(std::uintptr_t returnAddress) {
		const std::uintptr_t rva = returnAddress - g_ModuleBase;
		return rva >= kDump_Unlockables_Begin - kDumpImagebase && rva < kDump_Unlockables_End - kDumpImagebase;
	}

	bool CallerIsFetchStatus(std::uintptr_t returnAddress) {
		const std::uintptr_t rva = returnAddress - g_ModuleBase;
		const std::uintptr_t begin = kDump_DwFetch_GetStatus - kDumpImagebase;
		return rva >= begin && rva < begin + kDwFetch_GetStatus_Size;
	}

	bool IsOwnableItem(std::int64_t controller, std::uint64_t itemId) {
		return controller >= 0 && controller <= 1 && itemId != 0
			&& !(itemId >= kInventory_TrialItemFirst && itemId <= kInventory_TrialItemLast);
	}

	bool AttachmentSlotExists(int mode, unsigned int itemIndex, int slot) {
		if (!g_Api.unlockables || mode < 0 || mode > 3 || itemIndex > 0x3FF || slot < 0) return false;
		const std::uint8_t* const entry = g_Api.unlockables + kUnlockables_ModeStride * static_cast<std::size_t>(mode)
			+ kUnlockables_ItemStride * static_cast<std::size_t>(itemIndex);
		std::uint8_t flags = 0, count = 0;
		if (!SafeRead(entry + kUnlockables_ItemFlags, flags) || !(flags & 4)) return false;
		if (!SafeRead(entry + kUnlockables_ItemRow + kUnlockableRow_AttachmentCount, count)) return false;
		return slot < count;
	}

	bool HasLootBlock(unsigned int controller) { return LootBlock(controller) != 0; }

	void OnUiLoaded() {
		if (!Enabled()) return;
		g_UiLoaded = true;
		g_ModelRefresh = true;
	}

	void Tick() {
		if (!Enabled()) return;
		static std::uint64_t s_NextPoll = 0, s_NextSummary = 0, s_LastChanged = 0;
		static std::uintptr_t s_Block = 0;
		static bool s_Open = false, s_Pending = false;
		static int s_RefreshLines = 0, s_LootEnabled = -1;
		static std::uintptr_t* s_LootEnabledDvar = nullptr;

		const std::uint64_t now = GetTickCount64();
		s_Pending |= g_ModelRefresh.exchange(false);
		if (now < s_NextPoll) return;
		s_NextPoll = now + 1000;

		// Read-only: Loot_GetItemQuantity and the Lua's own loot checks also depend on loot_enabled, a
		// publisher variable. The detours do not need it; the value is logged in case a menu still does.
		if (!s_LootEnabledDvar && g_Pointers) s_LootEnabledDvar = g_Pointers->FindDvar("loot_enabled");
		if (s_LootEnabledDvar) {
			const int value = g_Api.DvarGetBool(s_LootEnabledDvar) ? 1 : 0;
			if (value != s_LootEnabled) {
				LOG("UnlockAll", INFO, "loot_enabled reads {}{}.", value ? "true" : "false", s_LootEnabled < 0 ? ""
					: " (it changed)");
				s_LootEnabled = value;
			}
		}

		// The battle pass model. The engine fills it on inventory and AE events, which never arrive on
		// this backend, so it is filled here: once the getters can answer (signed in, stats readable),
		// again when the sign-in made a new loot block, and after every UI load.
		const std::uintptr_t block = LootBlock(0);
		const bool open = block != 0 && g_UiLoaded.load() && g_Api.StatsReadable(0);
		if (open && (s_Pending || !s_Open || block != s_Block)) {
			const char* const why = !s_Open ? "the getters can answer now" : block != s_Block ? "a sign-in made a new loot "
				"block" : "the UI was loaded again";
			g_Api.UpdateBattlePassModels(0);
			s_Pending = false;
			if (++s_RefreshLines <= 8) {
				LOG("UnlockAll", INFO, "battle pass: UI model filled from the getters ({}; refresh #{}).", why, s_RefreshLines);
			}
		}
		s_Open = open;
		s_Block = block;

		if (now >= s_NextSummary) {
			std::uint64_t changed = 0;
			for (const Counter& c : g_Counters) changed += c.changed.load(std::memory_order_relaxed);
			if (changed != s_LastChanged) {
				s_LastChanged = changed;
				s_NextSummary = now + 60000;
				LOG("UnlockAll", INFO, "so far (changed of asked): {}.", SummaryText());
			}
		}
	}

	Status ReadStatus() {
		Status s;
		s.enabled = Enabled();
		for (const Counter& c : g_Counters) {
			s.asked += c.asked.load(std::memory_order_relaxed);
			s.changed += c.changed.load(std::memory_order_relaxed);
		}
		return s;
	}
}
