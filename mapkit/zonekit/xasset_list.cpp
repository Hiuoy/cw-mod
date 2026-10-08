#include "xasset_list.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace MapKit::Zone {
	namespace {
		// g_xassetTypeNames (0x7FF72A388D90), in type order.
		constexpr std::array<std::string_view, kXAssetTypeCount> kTypeNames = {
			"zone", "assetlist", "physpreset", "physconstraints", "destructibledef", "xanim", "xmodel", "xcollision",
			"xskeleton", "xmodelmesh", "material", "csdef", "computeshaderset", "rtsdef", "raytraceshaderset", "techset",
			"image", "sound", "sound_bank", "sound_asset", "sound_duck", "sound_alias_modifier", "sound_acoustics", "col_map",
			"clip_map", "com_map", "game_map", "gfx_map", "fonticon", "localizeentry", "gesture", "gesturetable",
			"cinematicmotion", "weapon", "weaponfull", "weaponfrontend", "weaponblueprint", "weaponstylesettings", "weaponsecondarymovement", "weapontunables",
			"cgmediatable", "playersoundstable", "playerfxtable", "sharedweaponsounds", "attachment", "attachmentunique", "weaponcamo", "weaponcamobinding",
			"customizationtable", "customizationtablefrontend", "snddriverglobals", "fx", "tagfx", "klf", "impactsfxtable", "impactsoundstable",
			"aitype", "character", "xmodelalias", "rawfile", "rawfilepreproc", "rawtextfile", "animtree", "stringtable",
			"structuredtable", "leaderboarddef", "ddl", "glasses", "scriptparsetree", "scriptparsetreedbg", "script_using", "script_using_cp",
			"script_using_mp", "script_using_wz", "script_using_zm", "keyvaluepairs", "vehicle", "tracer", "surfacefxtable", "surfacesounddef",
			"footsteptable", "entityfximpacts", "entitysoundimpacts", "zbarrier", "vehiclefxdef", "vehiclesounddef", "typeinfo", "scriptbundle",
			"scriptbundlelist", "rumble", "bulletpenetration", "locdmgtable", "aimtable", "shoottable", "playerglobaltunables", "overheadcameratunables",
			"animselectortable", "animmappingtable", "animstatemachine", "behaviortree", "behaviorstatemachine", "ttf", "sanim", "shellshock",
			"statuseffect", "cinematic_camera", "cinematic_sequence", "spectate_camera", "xcam", "bgcache", "flametable", "bitfield",
			"maptable", "maptableentry", "maptablelist", "objective", "objectivelist", "navmesh", "navvolume", "laser",
			"beam", "streamerhint", "flowgraph", "postfxbundle", "luafile", "luafiledebug", "renderoverridebundle", "staticlevelfxlist",
			"triggerlist", "playerroletemplate", "playerroletemplatefrontend", "playerrolecategorytable", "playerrolecategory", "characterbodytype", "characterbodytypefrontend", "playeroutfit",
			"gametypetable", "gametypetableentry", "feature", "featuretable", "unlockableitem", "unlockableitemtable", "entitylist", "playlists",
			"playlistglobalsettings", "playlistschedule", "motionmatchinginput", "blackboard", "tacticalquery", "playermovementtunables", "hierarchicaltasknetwork", "ragdoll",
			"storagefile", "storagefilelist", "charmixer", "storeproduct", "storecategory", "storecategorylist", "rank", "ranktable",
			"prestige", "prestigetable", "firstpartyentitlement", "firstpartyentitlementlist", "entitlement", "entitlementlist", "sku", "labelstore",
			"labelstorelist", "cpu_occlusion_data", "lighting", "districts", "streamerworld", "talent", "playertalenttemplate", "playeranimation",
			"<unused>", "terraingfx", "highlightreelinfodefines", "highlightreelprofileweighting", "highlightreelstarlevels", "dlogevent", "rawstring", "ballisticdesc",
			"streamkey", "rendertargets", "drawnodes", "grouplodmodel", "fxlibraryvolume", "arenaseasons", "sprayorgestureitem", "sprayorgesturelist",
			"hwplatform", "attachmenttable", "navinput", "uimodeldatastruct", "crafticon", "crafticonlist", "craftweaponsticker", "craftweaponstickerlist",
			"craftbackground", "craftbackgroundlist", "craftmaterial", "craftmateriallist", "craftcategory", "craftcategorylist", "craftweaponicontransform", "craftweaponicontransformlist",
			"xanimcurve", "dynmodel", "vectorfield", "winddef", "vehicleassembly", "milestone", "milestonetable", "triggereffectdesc",
			"triggeractions", "playersettings", "compasstunables", "execution", "scenario",
		};

		template <typename T>
		bool ReadAt(std::span<const std::uint8_t> data, std::size_t offset, T& value) {
			if (offset > data.size() || data.size() - offset < sizeof(T)) {
				return false;
			}
			std::memcpy(&value, data.data() + offset, sizeof(T));
			return true;
		}
	}

	std::string_view XAssetTypeName(std::uint64_t type) {
		return type < kTypeNames.size() ? kTypeNames[static_cast<std::size_t>(type)] : std::string_view("?");
	}

	std::optional<std::uint64_t> XAssetTypeFromName(std::string_view name) {
		for (std::size_t i = 0; i < kTypeNames.size(); ++i) {
			if (kTypeNames[i] == name) {
				return i;
			}
		}
		return std::nullopt;
	}

	std::size_t XAssetNameOffset(std::uint64_t type) {
		// Every GetName of the table, decoded from the exe 2026-09-30 (the rest are `mov rax,[rcx]`; 0x00, weaponfull
		// 0x22 and 0xB0 return 0, assetlist 0x01 a constant).
		switch (type) {
		case 0x05: return 112;  // xanim (the 288-byte root; +0 is not the name)
		case 0x11: return 8;    // sound
		case 0x1D: return 8;    // localizeentry
		case 0x27: return 48;   // weapontunables
		case 0x35: return 16;   // klf (+0 is a string)
		case 0x66: return 8;    // sanim
		case 0x6E: return 432;  // flametable
		case 0x71: return 8;    // maptableentry
		case 0x89: return 32;   // gametypetableentry
		case 0x8C: return 8;    // unlockableitem
		case 0x8F: return 16;   // playlists
		case 0x90: return 8;    // playlistglobalsettings
		case 0x91: return 8;    // playlistschedule
		case 0x97: return 2976; // ragdoll
		case 0x98: return 8;    // storagefile
		case 0x9C: return 8;    // storecategory
		case 0xB5: return 8;    // dlogevent
		case 0xBD: return 8;    // arenaseasons
		case 0xC4: return 16;   // crafticon
		case 0xC6: return 16;   // craftweaponsticker
		case 0xC8: return 8;    // craftbackground
		case 0xCA: return 16;   // craftmaterial
		case 0xCC: return 8;    // craftcategory
		case 0xD1: return 224;  // dynmodel
		default: return 0;
		}
	}

	bool ParseXAssetList(std::span<const std::uint8_t> stream, XAssetList& out, std::string& error) {
		std::uint32_t stringCount = 0;
		std::uint64_t stringsPtr = 0, assetCount = 0, assetsPtr = 0;
		if (!ReadAt(stream, 0, stringCount) || !ReadAt(stream, 8, stringsPtr) || !ReadAt(stream, 24, assetCount)
			|| !ReadAt(stream, 32, assetsPtr)) {
			error = "stream is smaller than the asset list header";
			return false;
		}

		std::size_t cursor = kXAssetListSize;
		out.strings.clear();
		if (stringsPtr == kPtrInline) {
			std::vector<std::uint64_t> pointers(stringCount);
			for (std::uint64_t& pointer : pointers) {
				if (!ReadAt(stream, cursor, pointer)) {
					error = "string pointer array is truncated";
					return false;
				}
				cursor += 8;
			}
			for (std::uint64_t pointer : pointers) {
				if (pointer != kPtrInline) {
					out.strings.emplace_back(std::nullopt);
					continue;
				}
				const auto* begin = reinterpret_cast<const char*>(stream.data() + cursor);
				const std::size_t length = strnlen(begin, stream.size() - cursor);
				if (cursor + length >= stream.size()) {
					error = "script string runs past the end of the stream";
					return false;
				}
				out.strings.emplace_back(std::string(begin, length));
				cursor += length + 1;
			}
		}
		else if (stringsPtr != kPtrNull) {
			error = "script string array is not inline";
			return false;
		}

		out.assets.clear();
		out.assetsOffset = cursor;
		if (assetsPtr == kPtrInline) {
			if ((stream.size() - cursor) / 16 < assetCount) {
				error = "asset array is truncated";
				return false;
			}
			out.assets.resize(static_cast<std::size_t>(assetCount));
			for (XAssetEntry& entry : out.assets) {
				ReadAt(stream, cursor, entry.type);
				ReadAt(stream, cursor + 8, entry.header);
				cursor += 16;
			}
		}
		else if (assetsPtr != kPtrNull) {
			error = "asset array is not inline";
			return false;
		}
		out.bodyOffset = cursor;
		return true;
	}

	std::vector<std::size_t> FindAssetLinks(std::span<const std::uint8_t> stream, std::size_t begin, std::size_t end,
		std::uint64_t assetArrayPos, std::size_t assetCount) {
		constexpr std::uint64_t kBlockVirtual = 4; // xstream.hpp XBlockVirtual
		std::vector<std::size_t> out;
		end = std::min(end, stream.size());
		for (std::size_t at = begin; at + 8 <= end; ++at) {
			std::uint64_t stored = 0;
			std::memcpy(&stored, stream.data() + at, 8);
			if (stored == kPtrNull || stored == kPtrInline || stored == kPtrInsert) {
				continue;
			}
			const std::uint64_t value = stored - 1;
			const std::uint64_t offset = value & 0x0FFFFFFFFFFFFFFFull;
			if ((value >> 60) == kBlockVirtual && offset >= assetArrayPos && offset < assetArrayPos + 16ull * assetCount
				&& (offset - assetArrayPos) % 16 == 8) {
				out.push_back(at);
			}
		}
		return out;
	}
}
