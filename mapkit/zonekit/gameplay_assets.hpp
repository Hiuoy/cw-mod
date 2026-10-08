#pragma once
#include "xstream.hpp"

#include <cstdint>

// Loaders for gameplay assets a level zone's other assets point into. Registered in asset_loaders.cpp.
namespace MapKit::Zone {
	bool LoadVehicleAsset(XStream& s, std::uint64_t header); // 0x4C
}
