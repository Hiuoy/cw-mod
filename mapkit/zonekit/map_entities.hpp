#pragma once
#include "xstream.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A level's entities: the entitylist asset (0x8E: worldspawn, script structs, script models, spawners,
// path nodes) and the triggerlist asset (0x80: trigger volumes, plus three arrays not decoded yet). Both
// hold the same entity array. Reversed 2026-09-25 from Load_EntitylistAsset 0x7FF71E7D50F0,
// Load_TriggerlistAsset 0x7FF71E7EF010, Load_TriggerList 0x7FF71E7EEEE0 and Load_MapEntityArray
// 0x7FF71E7DE900. Checked on zm_silver: 2707 entities decode, and the entity list's data ends on the byte
// before the next asset's root struct.
//
//   EntityList  (24 B)  { u64 name; u64 count @8; MapEntity* entities @16 }   name = maps/<p>/<map>.d3dbsp
//   TriggerList (72 B)  { u64 name; u32 modelCount @8; TriggerModel* @16; u32 hullCount @24; TriggerHull* @32;
//                         u32 slabCount @40; TriggerSlab* @48; u64 count @56; MapEntity* entities @64 }
//   TriggerModel (8 B)  { u32 contents; u16 hullCount @4; u16 firstHull @6 }
//   TriggerHull (32 B)  { vec3 midPoint; vec3 halfSize @12; u32 contents @24; u16 slabCount @28; u16 firstSlab @30 }
//   TriggerSlab (20 B)  { vec3 dir; f32 midPoint @12; f32 halfSize @16 }   (the extra planes of a turned shape)
// Shapes are relative to their entity's origin: zm_silver's 357 hulls all have midPoint near 0 (checked
// 2026-09-27). A turned box keeps angles 0 on its entity: its hull is the box's axis-aligned bounds, and one
// slab per turned axis cuts it back to the box (|dot(p, dir) - midPoint| <= halfSize).
// Contents seen on zm_silver: info_volume 0x80000001 (0x80000004 on navmesh region separators),
// trigger_multiple/_use/_damage/_hurt 0x82000000.
// Trigger-list entity i has trigger model i (G_SpawnTriggers 0x7FF723F2C3B0 sets ent+656 = i while
// i < modelCount); CM_TriggerModelTouch 0x7FF72927E210 tests a model's hulls against a box, moved by how far the
// entity has moved from where it was placed. Same layout as Black Ops 3's MapTriggers.
//   MapEntity   (48 B)  { u32 keyCount; KeyValue* keys @8; i32 @16 (-1 in the entity list, 0 in the trigger
//                         list); u32 id @20; vec3 origin @24; vec3 angles @36 }
//   KeyValue    (32 B)  { XString key; u8 value[16] @8; u16 type @24; u16 @26 (1 on most keys) }
// The origin and angles also appear as "origin"/"angles" keys; an edit changes both.
namespace MapKit::Zone {
	enum class EntityValueType : std::uint16_t {
		String = 2, // an XString at +8
		Vector = 3, // 3 floats
		Hash = 4,   // u64 name hash (model and sound names are stored this way)
		Float = 5,
		Int = 6,    // i64
	};

	struct EntityKey {
		std::string key;
		EntityValueType type{};
		std::uint16_t flags = 0;
		std::string text;                 // String
		std::array<float, 3> vector{};    // Vector
		std::uint64_t hash = 0;           // Hash
		float number = 0;                 // Float
		std::int64_t integer = 0;         // Int
		std::size_t offset = 0;           // stream offset of the 32-byte KeyValue
		std::array<std::uint8_t, 32> raw{}; // the KeyValue as stored (pointers included)

		std::string ValueText() const;
	};

	struct MapEntity {
		bool trigger = false;             // from the trigger list
		std::int32_t field16 = -1;        // -1 in the entity list, 0 in the trigger list
		std::uint32_t id = 0;
		std::array<float, 3> origin{};
		std::array<float, 3> angles{};
		std::size_t offset = 0;           // stream offset of the 48-byte MapEntity
		std::array<std::uint8_t, 48> raw{}; // the MapEntity as stored
		std::vector<EntityKey> keys;

		const EntityKey* Find(std::string_view key) const;
		std::string Text(std::string_view key) const; // a String key's value, else ""
	};

	// The trigger list's shapes, as stored (see the layout above).
	struct TriggerShapes {
		std::vector<std::array<std::uint8_t, 8>> models;
		std::vector<std::array<std::uint8_t, 32>> hulls;
		std::vector<std::array<std::uint8_t, 20>> slabs;
	};

	struct MapEntities {
		TriggerShapes shapes;             // the trigger list's
		std::size_t triggerList = 0;      // stream offsets of the two root structs
		std::size_t entityList = 0;
		std::size_t entityListEnd = 0;    // where the entity list's data ends
		std::vector<MapEntity> entities;  // the trigger list's, then the entity list's
		std::size_t unresolved = 0;       // string references into other assets, left as "<ref ...>"
	};

	// Strings seen while decoding: inline ones by block position, and references to resolve later.
	struct EntityStrings;

	// The loaders. `out` and `strings` may be null: the full-stream walk only consumes.
	bool LoadEntityListBody(XStream& s, std::span<const std::uint8_t> root, std::vector<MapEntity>* out,
		EntityStrings* strings);
	bool LoadTriggerListBody(XStream& s, std::span<const std::uint8_t> root, std::vector<MapEntity>* out,
		EntityStrings* strings, TriggerShapes* shapes = nullptr);

	// maps/<p>/<map>.d3dbsp, <p> being the text before the first '_' (Com_GetMapBspName).
	std::string MapBspName(std::string_view mapName);

	// Finds and decodes both assets in a level zone's stream without walking everything before them:
	// each root struct starts with the hash of MapBspName(map). A candidate counts when its data decodes
	// inside the stream, and (entity list) its first entity is worldspawn. Until the full walk reaches
	// these assets, references to strings in other assets are matched by position (see the .cpp).
	bool FindMapEntities(std::span<const std::uint8_t> stream, std::string_view mapName, MapEntities& out,
		std::string& error);

	// Moves an entity in place, in its MapEntity and its "origin"/"angles" keys. Nothing changes size.
	void MoveEntity(std::span<std::uint8_t> stream, const MapEntity& entity, const std::array<float, 3>& origin,
		std::optional<float> yaw);
	struct XAssetList;
	struct ZoneTrace;
	class XWriter;

	// FindMapEntities with a zone trace (zone_trace.hpp): both lists decode at their recorded starts, so the
	// block positions are the engine's and every string reference resolves exactly, including the ones into
	// other assets (those assets are replayed with their loaders). Anything left over is counted in
	// `unresolved` and named "<ref ... in asset N (type)>".
	bool ReadMapEntitiesTraced(std::span<const std::uint8_t> stream, const XAssetList& list, const ZoneTrace& trace,
		MapEntities& out, std::string& error);

	// Sets an entity's origin (and yaw) in its fields and its "origin"/"angles" keys.
	void SetEntityOrigin(MapEntity& entity, const std::array<float, 3>& origin, std::optional<float> yaw);

	// Writes an entitylist asset (Load_EntitylistAsset(0, &header), header inline) holding `entities`, every
	// string inline. An entity keeps its stored bytes except the pointers, counts, id, origin and angles; a
	// key keeps its bytes except the pointers and its value (vector, hash, float or int, from its fields).
	// `name` = HashName(MapBspName(map)).
	void EncodeEntityList(XWriter& w, std::uint64_t name, std::span<const MapEntity> entities);

	// Writes a triggerlist asset (Load_TriggerlistAsset, header inline): the shapes, then `entities` as
	// EncodeEntityList writes them. Entity i takes model i, so shapes.models must line up with the entities.
	void EncodeTriggerList(XWriter& w, std::uint64_t name, const TriggerShapes& shapes, std::span<const MapEntity> entities);

	// Adds a box trigger model: a box of halfSize (along its own axes) centered on its entity's origin and turned
	// yaw degrees about Z. Returns the model's index.
	std::size_t AddTriggerBox(TriggerShapes& shapes, std::uint32_t contents, const std::array<float, 3>& halfSize, float yaw);
}
