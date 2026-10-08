// Gameplay assets a level zone's entities point into, each loader transcribed from the game's
// (addresses in the comments; IDB bocw_fixed_renamed.i64, build 1.34.0.15931218). Nested assets are
// loaded through LoadAsset: a type mapkit has no loader for passes while it is a reference.
#include "gameplay_assets.hpp"
#include "asset_loaders.hpp"
#include "model_assets.hpp"
#include "world_assets.hpp"

#include <vector>

namespace MapKit::Zone {
	namespace {
		constexpr std::uint64_t kFx = 0x33;
		constexpr std::uint64_t kImage = 0x10;
		constexpr std::uint64_t kScriptBundle = 0x57;
		constexpr std::uint64_t kRumble = 0x59;
		constexpr std::uint64_t kSurfaceFxTable = 0x4E;
		constexpr std::uint64_t kVehicleFxDef = 0x54;
		constexpr std::uint64_t kVehicleSoundDef = 0x55;

		// A {pointer, i32 count} field the generated code only tests for non-null.
		void LoadCountedI32(XStream& s, std::span<const std::uint8_t> root, std::size_t pointer, std::size_t count,
			std::size_t stride, std::uint64_t alignment) {
			const auto n = Get<std::int32_t>(root, count);
			LoadArray(s, Get<std::uint64_t>(root, pointer), n > 0 ? static_cast<std::size_t>(n) : 0, stride, alignment);
		}

		// sub_7FF71E7D0D90 (24 B): +0 a collision tree (Load_XCollisionTree 0x7FF71E7F9A90), +8 its 32-B index.
		void LoadVehicleCollision(XStream& s, std::uint64_t pointer) {
			if (!pointer) {
				return;
			}
			s.Alloc(8);
			std::vector<std::uint8_t> head(24);
			if (!s.Load(head.data(), head.size())) {
				return;
			}
			if (Get<std::uint64_t>(head, 0)) {
				s.Alloc(8);
				std::vector<std::uint8_t> tree(184);
				if (s.Load(tree.data(), tree.size())) {
					ReadCollisionTree(s, tree, nullptr);
				}
			}
			if (Get<std::uint64_t>(head, 8)) {
				s.Alloc(8);
				std::vector<std::uint8_t> index(32);
				if (s.Load(index.data(), index.size())) {
					LoadCollisionTreeIndexBody(s, index);
				}
			}
		}

		// --- vehicle (0x4C): Load_VehicleAsset 0x7FF71E7F1080 -> 0x7FF71E7F0170 (7192 B) --------------------
		void LoadVehicleBody(XStream& s, std::span<const std::uint8_t> v) {
			s.Push(XBlockVirtual);
			for (const std::size_t field : { 352, 368, 384, 400, 416, 448 }) {
				LoadCountedI32(s, v, field, field + 8, 8, 4);
			}
			for (std::size_t k = 0; k < 16 && !s.Failed(); ++k) {
				LoadAsset(s, kFx, Get<std::uint64_t>(v, 1744 + 40 * k), "vehicle fx");
			}
			for (std::size_t k = 0; k < 16 && !s.Failed(); ++k) {
				LoadAsset(s, kFx, Get<std::uint64_t>(v, 2408 + 64 * k), "vehicle fx");
				LoadAsset(s, kSurfaceFxTable, Get<std::uint64_t>(v, 2416 + 64 * k), "vehicle surface fx");
			}
			for (std::size_t k = 0; k < 5 && !s.Failed(); ++k) {
				LoadAsset(s, kScriptBundle, Get<std::uint64_t>(v, 4024 + 88 * k), "vehicle scriptbundle");
			}
			LoadAsset(s, 0x20, Get<std::uint64_t>(v, 4432), "vehicle cinematicmotion");
			LoadAsset(s, kVehicleSoundDef, Get<std::uint64_t>(v, 4608), "vehicle sounds");
			LoadAsset(s, kVehicleSoundDef, Get<std::uint64_t>(v, 4616), "vehicle sounds");
			LoadXString(s, Get<std::uint64_t>(v, 4672), "vehicle +4672");
			LoadCountedI32(s, v, 4744, 4752, 12, 4);
			for (const std::size_t field : { 4920, 4928, 4936, 4944 }) {
				LoadXModelAsset(s, Get<std::uint64_t>(v, field));
			}
			LoadVehicleCollision(s, Get<std::uint64_t>(v, 4952));
			LoadVehicleCollision(s, Get<std::uint64_t>(v, 4960));
			LoadAsset(s, kFx, Get<std::uint64_t>(v, 4976), "vehicle fx");
			LoadAsset(s, kFx, Get<std::uint64_t>(v, 4992), "vehicle fx");
			LoadAsset(s, kRumble, Get<std::uint64_t>(v, 5008), "vehicle rumble");
			LoadAsset(s, kFx, Get<std::uint64_t>(v, 5016), "vehicle fx");
			for (std::size_t field = 5032; field <= 5080; field += 8) {
				LoadAsset(s, kVehicleFxDef, Get<std::uint64_t>(v, field), "vehicle fx def");
			}
			for (const std::size_t field : { 5096, 5120, 5152 }) {
				LoadAsset(s, kFx, Get<std::uint64_t>(v, field), "vehicle fx");
			}
			for (const std::size_t base : { 5176, 5224, 5272, 5320 }) {
				for (std::size_t k = 0; k < 4 && !s.Failed(); ++k) {
					LoadAsset(s, kFx, Get<std::uint64_t>(v, base + 8 * k), "vehicle fx");
				}
			}
			LoadXString(s, Get<std::uint64_t>(v, 5384), "vehicle +5384");
			LoadAsset(s, kRumble, Get<std::uint64_t>(v, 5408), "vehicle rumble");
			LoadXString(s, Get<std::uint64_t>(v, 5416), "vehicle +5416");
			LoadAsset(s, kImage, Get<std::uint64_t>(v, 5496), "vehicle image");
			for (std::size_t field = 5528; field <= 5816; field += 16) {
				LoadXString(s, Get<std::uint64_t>(v, field), "vehicle string");
			}
			LoadXString(s, Get<std::uint64_t>(v, 5840), "vehicle +5840");
			for (const std::size_t field : { 5904, 5920, 5928, 5936, 5944 }) {
				LoadAsset(s, kRumble, Get<std::uint64_t>(v, field), "vehicle rumble");
			}
			LoadCountedI32(s, v, 6024, 6032, 8, 4);
			for (std::size_t field = 6808; field <= 6848; field += 8) {
				LoadAsset(s, kScriptBundle, Get<std::uint64_t>(v, field), "vehicle scriptbundle");
			}
			LoadAsset(s, 0x62, Get<std::uint64_t>(v, 6856), "vehicle animstatemachine");
			LoadAsset(s, 0x60, Get<std::uint64_t>(v, 6864), "vehicle animselectortable");
			LoadAsset(s, 0x61, Get<std::uint64_t>(v, 6872), "vehicle animmappingtable");
			LoadAsset(s, 0x4F, Get<std::uint64_t>(v, 6888), "vehicle surface sounds");
			LoadAsset(s, kSurfaceFxTable, Get<std::uint64_t>(v, 6896), "vehicle surface fx");
			LoadAsset(s, 0x04, Get<std::uint64_t>(v, 6904), "vehicle destructibledef");
			LoadAsset(s, kImage, Get<std::uint64_t>(v, 6912), "vehicle image");
			LoadAsset(s, kImage, Get<std::uint64_t>(v, 6920), "vehicle image");
			// sub_7FF71E7DCDE0 (16 B, embedded): +0 8 B x i32 @8.
			for (const std::size_t field : { 6952, 6968, 6984, 7000, 7016, 7040, 7056, 7072, 7088, 7104, 7120, 7136 }) {
				LoadCountedI32(s, v, field, field + 8, 8, 4);
			}
			LoadSettingsTree(s, v.subspan(7152, 32));
			LoadAsset(s, 0xD4, Get<std::uint64_t>(v, 7184), "vehicle assembly");
			s.Pop();
		}
	}

	bool LoadVehicleAsset(XStream& s, std::uint64_t header) {
		s.SetRootType(0x4C);
		return LoadAssetHeader(s, header, 7192, 8, [&](std::span<std::uint8_t> v) { LoadVehicleBody(s, v); });
	}
}
