#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/mapkit_loader.hpp"
#include "game/mapkit_trace.hpp"
#include "game/mapkit_usage.hpp"

// The six custom-map detours (game/mapkit_loader.hpp). Each calls the original directly: none of the
// six targets has a caller check (prologues read 2026-09-25), SV_StartMap and DB_ExpandZoneVariants have
// 5 arguments (too many for an ArxanCall thunk), and the others run on every zone load, file open or
// lobby frame.

// Runs once per zone load, after the zone is linked. Ours are not signed by Treyarch.
template <>
void Client::Hook::Hooks::HK_DB_Signature_VerifyZone::hkCallback() {
	if (Game::MapKit::SkipSignatureCheck()) return;
	m_Original();
}

// A custom zone never has a .fd: without this, a zone that replaces a retail one would pick up the
// retail .fd through the search paths and drop on its base-header check.
template <>
void* Client::Hook::Hooks::HK_FS_OpenFileRead::hkCallback(const char* path, int mode, int device) {
	if (Game::MapKit::HidesPatchFile(path)) return nullptr;
	return m_Original(path, mode, device);
}

// The optional redirect from the overlay's Maps tab ("when the lobby starts zm_silver, start zm_mapkit").
// Three parts: the lobby preloads the target zone instead of the retail one, every other level-zone load
// of the retail name loads the target too, and the map start names it.
template <>
std::uint64_t Client::Hook::Hooks::HK_MapPreload_StartZoneRead::hkCallback(const char* mapName, std::uint64_t a2,
	std::uint64_t a3, std::uint64_t a4) {
	return m_Original(Game::MapKit::RedirectPreload(mapName), a2, a3, a4);
}

template <>
void Client::Hook::Hooks::HK_DB_LoadXAssets::hkCallback(Game::Functions::XZoneInfo* zones, std::uint32_t count,
	int freeFlags) {
	const std::uintptr_t caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
	m_Original(Game::MapKit::RedirectZones(zones, count, freeFlags, caller), count, freeFlags);
}

// A map of its own loads its asset library's zones first: the library goes into the list before the original expands
// it, and the map's own variants (it ships only <id>.ff) come out after. The custom map picked in CUSTOM MAPS, an
// overlay, adds its override zones right after its base. The engine lists a map's own zone last, so it links after its
// variants. An override zone must link after the map instead, or its by-name references into the map find nothing
// (the D2 lighting drop).
template <>
int Client::Hook::Hooks::HK_DB_ExpandZoneVariants::hkCallback(Game::Functions::XZoneInfo* zones, int count,
	char* names, Game::Functions::XZoneInfo* out, int max) {
	int inCount = count;
	Game::Functions::XZoneInfo* const in = Game::MapKit::WithLibraries(zones, inCount);
	int expanded = Game::MapKit::TrimOwnMapVariants(out, m_Original(in, inCount, names, out, max));
	expanded = Game::MapKit::AddActiveMap(out, expanded, max);
	Game::MapKit::OrderOverrideZones(out, expanded);
	return expanded;
}

// The host's lobby map: a map of its own picked in CUSTOM MAPS takes the place of its asset library's name, which the
// playlist sets. Two args, plain prologue: the original is called directly.
template <>
char Client::Hook::Hooks::HK_Session_SetMapName::hkCallback(std::uint32_t slot, const char* mapName) {
	const std::uintptr_t caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
	return m_Original(slot, Game::MapKit::HostLobbyMap(slot, mapName, caller));
}

// The maptable knows a map of its own by its asset library's entry (getmapfields, the loading screen). Leaf functions.
template <>
void* Client::Hook::Hooks::HK_MapTable_FindEntryByHash::hkCallback(std::uint64_t mapHash) {
	return m_Original(Game::MapKit::MapTableHash(mapHash));
}

template <>
std::uint32_t Client::Hook::Hooks::HK_MapTable_GetMapFlags::hkCallback(const char* mapName) {
	return m_Original(Game::MapKit::MapTableName(mapName));
}

// The asset-usage census (game/mapkit_usage.hpp): how scripts and map entities reach the assets a bgcache lists.
// A leaf with a plain prologue: the original is called directly.
template <>
std::int64_t Client::Hook::Hooks::HK_BG_Cache_FindIndex::hkCallback(std::uint8_t table, std::uint64_t name) {
	Game::MapKitUsage::OnCacheFind(table, name, _ReturnAddress());
	return m_Original(table, name);
}

// Logs the gfx_map the renderer starts the level with, then runs as normal.
template <>
void Client::Hook::Hooks::HK_R_InitWorld::hkCallback() {
	Game::MapKit::LogWorldInit();
	m_Original();
}

template <>
std::uint64_t Client::Hook::Hooks::HK_SV_StartMap::hkCallback(std::uint32_t a1, const char* mapName,
	std::uint32_t kind, std::uint8_t a4, std::uint32_t a5) {
	return m_Original(a1, Game::MapKit::RedirectMap(mapName, kind), kind, a4, a5);
}

// The zone trace (game/mapkit_trace.hpp): pure pass-throughs that record, for a zone named in cw-mod.json
// "mapkit_trace", where each asset starts. DB_LoadXFile_Internal has 9 arguments, and the two readers run
// for every stored load of every zone, so all four call the original directly (no caller checks).
template <>
std::int64_t Client::Hook::Hooks::HK_DB_LoadXFile_Internal::hkCallback(std::int64_t a1, std::int64_t a2,
	const char* zoneName, int a4, void* assetList, void* blocks, std::uint64_t a7, int a8, int flags) {
	Game::MapKitTrace::OnZoneBegin(zoneName, assetList, blocks, flags);
	const std::int64_t result = m_Original(a1, a2, zoneName, a4, assetList, blocks, a7, a8, flags);
	Game::MapKitTrace::OnZoneEnd();
	return result;
}

template <>
std::int64_t Client::Hook::Hooks::HK_Load_XAsset::hkCallback(char atStreamStart, std::uint8_t* asset) {
	Game::MapKitTrace::OnAssetBegin(asset);
	const std::int64_t result = m_Original(atStreamStart, asset);
	Game::MapKitTrace::OnAssetEnd();
	return result;
}

// The same, for the twin loader family a preloaded zone (flags & 0x6A0) goes through.
template <>
std::int64_t Client::Hook::Hooks::HK_Load_XAsset_Preload::hkCallback(char atStreamStart, std::uint8_t* asset) {
	Game::MapKitTrace::OnAssetBegin(asset);
	const std::int64_t result = m_Original(atStreamStart, asset);
	Game::MapKitTrace::OnAssetEnd();
	return result;
}

template <>
void Client::Hook::Hooks::HK_DB_ReadXFile::hkCallback(void* dst, int size) {
	m_Original(dst, size);
	Game::MapKitTrace::OnStreamBytes(size);
}

template <>
void Client::Hook::Hooks::HK_DB_ReadXFileString::hkCallback(std::uint8_t* dst, std::uint32_t* count) {
	m_Original(dst, count);
	if (count) Game::MapKitTrace::OnStreamBytes(*count);
}
