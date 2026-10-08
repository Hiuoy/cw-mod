#include "patch.hpp"
#include "hash.hpp"

#include <miniz.h>

#include <cstring>

namespace MapKit::Zone {
	namespace {
		constexpr std::uint8_t kVcdiffMagic[4] = { 0xD6, 0xC3, 0xC4, 0x00 };

		enum class Op : std::uint8_t { None, Add, Run, Copy };

		struct Instruction {
			Op op = Op::None;
			std::uint8_t size = 0;
			std::uint8_t mode = 0;
		};

		using CodeTable = std::array<std::array<Instruction, 2>, 256>;

		// The RFC 3284 default code table (section 5.6).
		CodeTable BuildDefaultCodeTable() {
			CodeTable table{};
			std::size_t i = 0;
			table[i++][0] = { Op::Run, 0, 0 };
			table[i++][0] = { Op::Add, 0, 0 };
			for (std::uint8_t size = 1; size <= 17; ++size) {
				table[i++][0] = { Op::Add, size, 0 };
			}
			for (std::uint8_t mode = 0; mode <= 8; ++mode) {
				table[i++][0] = { Op::Copy, 0, mode };
				for (std::uint8_t size = 4; size <= 18; ++size) {
					table[i++][0] = { Op::Copy, size, mode };
				}
			}
			for (std::uint8_t mode = 0; mode <= 5; ++mode) {
				for (std::uint8_t add = 1; add <= 4; ++add) {
					for (std::uint8_t copy = 4; copy <= 6; ++copy) {
						table[i++] = { Instruction{ Op::Add, add, 0 }, Instruction{ Op::Copy, copy, mode } };
					}
				}
			}
			for (std::uint8_t mode = 6; mode <= 8; ++mode) {
				for (std::uint8_t add = 1; add <= 4; ++add) {
					table[i++] = { Instruction{ Op::Add, add, 0 }, Instruction{ Op::Copy, 4, mode } };
				}
			}
			for (std::uint8_t mode = 0; mode <= 8; ++mode) {
				table[i++] = { Instruction{ Op::Copy, 4, mode }, Instruction{ Op::Add, 1, 0 } };
			}
			return table;
		}

		const CodeTable& DefaultCodeTable() {
			static const CodeTable table = BuildDefaultCodeTable();
			return table;
		}

		class Reader {
		public:
			explicit Reader(std::span<const std::uint8_t> data) : m_Data(data) {}

			bool Byte(std::uint8_t& value) {
				if (m_Pos >= m_Data.size()) {
					return false;
				}
				value = m_Data[m_Pos++];
				return true;
			}

			// VCDIFF integers: big-endian base-128, high bit = more bytes follow.
			bool Varint(std::uint64_t& value) {
				value = 0;
				for (int i = 0; i < 10; ++i) {
					std::uint8_t byte;
					if (!Byte(byte)) {
						return false;
					}
					value = (value << 7) | (byte & 0x7F);
					if (!(byte & 0x80)) {
						return true;
					}
				}
				return false;
			}

			bool Take(std::size_t size, std::span<const std::uint8_t>& out) {
				if (m_Data.size() - m_Pos < size) {
					return false;
				}
				out = m_Data.subspan(m_Pos, size);
				m_Pos += size;
				return true;
			}

			bool AtEnd() const { return m_Pos >= m_Data.size(); }

		private:
			std::span<const std::uint8_t> m_Data;
			std::size_t m_Pos = 0;
		};

		// near (4) + same (3 x 256) address cache, RFC 3284 section 5.1.
		class AddressCache {
		public:
			bool Decode(Reader& addresses, std::uint64_t here, std::uint8_t mode, std::uint64_t& address) {
				if (mode == 0) {
					if (!addresses.Varint(address)) {
						return false;
					}
				}
				else if (mode == 1) {
					std::uint64_t back;
					if (!addresses.Varint(back) || back > here) {
						return false;
					}
					address = here - back;
				}
				else if (mode < 2 + kNear) {
					std::uint64_t offset;
					if (!addresses.Varint(offset)) {
						return false;
					}
					address = m_Near[mode - 2] + offset;
				}
				else {
					std::uint8_t slot;
					if (!addresses.Byte(slot)) {
						return false;
					}
					address = m_Same[(mode - 2 - kNear) * 256 + slot];
				}

				m_Near[m_NextSlot] = address;
				m_NextSlot = (m_NextSlot + 1) % kNear;
				m_Same[address % m_Same.size()] = address;
				return true;
			}

		private:
			static constexpr std::size_t kNear = 4;
			std::array<std::uint64_t, kNear> m_Near{};
			std::array<std::uint64_t, 3 * 256> m_Same{};
			std::size_t m_NextSlot = 0;
		};

		bool Inflate(std::span<const std::uint8_t> input, std::vector<std::uint8_t>& output, std::string& error) {
			mz_stream z{};
			if (mz_inflateInit(&z) != MZ_OK) {
				error = "zlib init failed";
				return false;
			}
			output.clear();
			std::uint8_t chunk[1 << 16];
			z.next_in = input.data();
			z.avail_in = static_cast<unsigned int>(input.size());
			int status;
			do {
				z.next_out = chunk;
				z.avail_out = sizeof(chunk);
				status = mz_inflate(&z, MZ_NO_FLUSH);
				if (status != MZ_OK && status != MZ_STREAM_END) {
					mz_inflateEnd(&z);
					error = "the patch's zlib stream is damaged";
					return false;
				}
				output.insert(output.end(), chunk, chunk + (sizeof(chunk) - z.avail_out));
			} while (status != MZ_STREAM_END);
			mz_inflateEnd(&z);
			return true;
		}
	}

	bool ReadPatchFile(std::span<const std::uint8_t> file, PatchFile& out, std::string& error) {
		if (file.size() < kPatchHeaderSize) {
			error = "patch file is smaller than its header";
			return false;
		}
		std::memcpy(out.patchHeader.data(), file.data(), kPatchHeaderSize);

		std::size_t offset = kPatchHeaderSize;
		if (!FastFileHeader::Parse(file, offset, out.result, error) || !FastFileHeader::Parse(file, offset, out.base, error)) {
			error = "patch header: " + error;
			return false;
		}

		const std::uint8_t codec = out.result.Flag(FlagIndex::Compression);
		if (codec != 6) {
			error = "patch codec " + std::to_string(codec) + " is not zlib";
			return false;
		}
		return Inflate(file.subspan(offset), out.delta, error);
	}

	bool ApplyVcdiff(std::span<const std::uint8_t> delta, std::span<const std::uint8_t> source,
		std::vector<std::uint8_t>& target, std::string& error) {
		Reader file(delta);
		std::span<const std::uint8_t> magic;
		std::uint8_t headerIndicator;
		if (!file.Take(4, magic) || std::memcmp(magic.data(), kVcdiffMagic, 4) != 0 || !file.Byte(headerIndicator)) {
			error = "not a VCDIFF stream";
			return false;
		}
		if (headerIndicator & ~0x0Cu) {
			error = "unsupported VCDIFF header indicator";
			return false;
		}

		const CodeTable& table = DefaultCodeTable();
		target.clear();
		std::size_t window = 0;
		while (!file.AtEnd()) {
			std::uint8_t indicator;
			if (!file.Byte(indicator)) {
				break;
			}

			std::span<const std::uint8_t> sourceWindow;
			if ((indicator & 3) == 1) {
				std::uint64_t length, position, fetch;
				if (!file.Varint(length) || !file.Varint(position) || ((indicator & 4) && !file.Varint(fetch))
					|| position > source.size() || length > source.size() - position) {
					error = "window " + std::to_string(window) + " has a bad source segment";
					return false;
				}
				sourceWindow = source.subspan(static_cast<std::size_t>(position), static_cast<std::size_t>(length));
			}
			else if (indicator & 3) {
				error = "target-window copies are not supported (the game rejects them too)";
				return false;
			}

			std::uint64_t encodingLength, targetLength, dataLength, instLength, addrLength;
			std::uint8_t deltaIndicator;
			if (!file.Varint(encodingLength) || !file.Varint(targetLength) || !file.Byte(deltaIndicator)
				|| !file.Varint(dataLength) || !file.Varint(instLength) || !file.Varint(addrLength)) {
				error = "window " + std::to_string(window) + " header is truncated";
				return false;
			}
			if (deltaIndicator != 0) {
				error = "secondary-compressed VCDIFF sections are not supported";
				return false;
			}

			std::span<const std::uint8_t> dataSection, instSection, addrSection;
			if (!file.Take(static_cast<std::size_t>(dataLength), dataSection) || !file.Take(static_cast<std::size_t>(instLength), instSection)
				|| !file.Take(static_cast<std::size_t>(addrLength), addrSection)) {
				error = "window " + std::to_string(window) + " sections are truncated";
				return false;
			}

			Reader data(dataSection);
			Reader inst(instSection);
			Reader addr(addrSection);
			AddressCache cache;
			std::vector<std::uint8_t> out;
			out.reserve(static_cast<std::size_t>(targetLength));

			while (!inst.AtEnd()) {
				std::uint8_t code;
				inst.Byte(code);
				for (const Instruction& instruction : table[code]) {
					if (instruction.op == Op::None) {
						continue;
					}
					std::uint64_t size = instruction.size;
					if (size == 0 && !inst.Varint(size)) {
						error = "truncated instruction size";
						return false;
					}
					if (out.size() + size > targetLength) {
						error = "window " + std::to_string(window) + " writes past its target length";
						return false;
					}

					if (instruction.op == Op::Add) {
						std::span<const std::uint8_t> bytes;
						if (!data.Take(static_cast<std::size_t>(size), bytes)) {
							error = "ADD runs past the data section";
							return false;
						}
						out.insert(out.end(), bytes.begin(), bytes.end());
					}
					else if (instruction.op == Op::Run) {
						std::uint8_t byte;
						if (!data.Byte(byte)) {
							error = "RUN runs past the data section";
							return false;
						}
						out.insert(out.end(), static_cast<std::size_t>(size), byte);
					}
					else {
						// Addresses below the source length are in the source window, the rest in this window's output.
						const std::uint64_t here = sourceWindow.size() + out.size();
						std::uint64_t address;
						if (!cache.Decode(addr, here, instruction.mode, address) || address >= here) {
							error = "COPY has a bad address";
							return false;
						}
						if (address + size <= sourceWindow.size()) {
							const auto from = sourceWindow.subspan(static_cast<std::size_t>(address), static_cast<std::size_t>(size));
							out.insert(out.end(), from.begin(), from.end());
						}
						else {
							// Target copies may overlap what they write (run-length style), so go byte by byte.
							for (std::uint64_t k = 0; k < size; ++k) {
								const std::uint64_t at = address + k;
								out.push_back(at < sourceWindow.size() ? sourceWindow[static_cast<std::size_t>(at)]
									: out[static_cast<std::size_t>(at - sourceWindow.size())]);
							}
						}
					}
				}
			}

			if (out.size() != targetLength) {
				error = "window " + std::to_string(window) + " produced " + std::to_string(out.size()) + " of "
					+ std::to_string(targetLength) + " bytes";
				return false;
			}

			if (indicator & 8) {
				std::span<const std::uint8_t> checksumBytes;
				if (!file.Take(4, checksumBytes)) {
					error = "window checksum is truncated";
					return false;
				}
				std::uint32_t expected;
				std::memcpy(&expected, checksumBytes.data(), 4);
				if (Xxh32(out) != expected) {
					error = "window " + std::to_string(window) + " XXH32 mismatch";
					return false;
				}
			}

			target.insert(target.end(), out.begin(), out.end());
			++window;
		}
		return true;
	}
}
