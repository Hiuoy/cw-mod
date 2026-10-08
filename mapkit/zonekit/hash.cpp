#include "hash.hpp"

#include <cstring>

namespace MapKit::Zone {
	namespace {
		constexpr std::uint32_t kPrime1 = 2654435761u;
		constexpr std::uint32_t kPrime2 = 2246822519u;
		constexpr std::uint32_t kPrime3 = 3266489917u;
		constexpr std::uint32_t kPrime4 = 668265263u;
		constexpr std::uint32_t kPrime5 = 374761393u;

		std::uint32_t Rotl(std::uint32_t value, int bits) {
			return (value << bits) | (value >> (32 - bits));
		}

		std::uint32_t Read32(const std::uint8_t* p) {
			std::uint32_t value;
			std::memcpy(&value, p, sizeof(value));
			return value;
		}
	}

	std::uint32_t Xxh32(std::span<const std::uint8_t> data, std::uint32_t seed) {
		const std::uint8_t* p = data.data();
		const std::uint8_t* end = p + data.size();
		std::uint32_t hash;

		if (data.size() >= 16) {
			std::uint32_t v1 = seed + kPrime1 + kPrime2;
			std::uint32_t v2 = seed + kPrime2;
			std::uint32_t v3 = seed;
			std::uint32_t v4 = seed - kPrime1;
			while (p <= end - 16) {
				v1 = Rotl(v1 + Read32(p) * kPrime2, 13) * kPrime1;
				v2 = Rotl(v2 + Read32(p + 4) * kPrime2, 13) * kPrime1;
				v3 = Rotl(v3 + Read32(p + 8) * kPrime2, 13) * kPrime1;
				v4 = Rotl(v4 + Read32(p + 12) * kPrime2, 13) * kPrime1;
				p += 16;
			}
			hash = Rotl(v1, 1) + Rotl(v2, 7) + Rotl(v3, 12) + Rotl(v4, 18);
		}
		else {
			hash = seed + kPrime5;
		}

		hash += static_cast<std::uint32_t>(data.size());
		for (; p + 4 <= end; p += 4) {
			hash = Rotl(hash + Read32(p) * kPrime3, 17) * kPrime4;
		}
		for (; p < end; ++p) {
			hash = Rotl(hash + *p * kPrime5, 11) * kPrime1;
		}

		hash ^= hash >> 15;
		hash *= kPrime2;
		hash ^= hash >> 13;
		hash *= kPrime3;
		hash ^= hash >> 16;
		return hash;
	}
}
