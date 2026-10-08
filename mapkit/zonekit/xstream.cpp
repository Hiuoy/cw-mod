#include "xstream.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace MapKit::Zone {
	XStream::XStream(std::span<const std::uint8_t> stream, std::size_t cursor) : m_Stream(stream), m_Cursor(cursor) {}

	bool XStream::Fail(std::string message) {
		if (m_Error.empty()) {
			m_Error = std::format("stream +0x{:X}, block {}: {}", m_Cursor, m_Block, message);
		}
		return false;
	}

	void XStream::Advance(std::uint64_t size) {
		m_Pos += size;
		m_HighWater[m_Block] = std::max(m_HighWater[m_Block], m_Pos);
	}

	void XStream::Switch(int block) {
		if (block == m_Block) {
			return;
		}
		m_BlockPos[m_Block] = m_Pos;
		m_Block = block;
		m_Pos = m_BlockPos[block];
	}

	void XStream::Push(int block) {
		if (block < 0 || block >= static_cast<int>(kXBlockCount)) {
			Fail(std::format("push of block {}", block));
			return;
		}
		if (m_Depth >= static_cast<int>(m_Stack.size())) {
			Fail("stream position stack overflow");
			return;
		}
		// Block 9 is replaced by the current block unless sub_7FF72769F980 says otherwise, and it never does
		// (it returns 0): collision data pushed "to block 9" is stored inline in the current block.
		if (block == XBlockCollisionAlias) {
			block = m_Block;
		}
		Frame& frame = m_Stack[m_Depth++];
		frame.block = m_Block;
		Switch(block);
		frame.pos = m_Pos;
	}

	void XStream::Pop() {
		if (m_Depth <= 0) {
			Fail("stream position stack underflow");
			return;
		}
		const Frame& frame = m_Stack[--m_Depth];
		if (m_Block == XBlockTemp) {
			m_Pos = frame.pos;
		}
		else if (m_Block == 1) {
			m_Pos = (m_Pos + 15) & ~15ull;
		}
		Switch(frame.block);
	}

	void XStream::PreloadRoot(std::uint64_t type, std::uint64_t alignment, std::size_t size) {
		if (PreloadInTempBlock(type)) {
			return;
		}
		m_PreloadBlock1 = ((m_PreloadBlock1 + alignment - 1) & ~(alignment - 1)) + size;
		m_PreloadBlock1High = std::max(m_PreloadBlock1High, m_PreloadBlock1);
	}

	void XStream::PreloadFieldEnd(std::uint64_t type) {
		if (PreloadInTempBlock(type)) {
			PreloadSave();
			return;
		}
		m_PreloadBlock1 = (m_PreloadBlock1 + 15) & ~15ull;
		m_PreloadBlock1High = std::max(m_PreloadBlock1High, m_PreloadBlock1);
	}

	void XStream::PreloadSave() {
		m_PreloadBlock1 += 104;
		m_PreloadBlock1High = std::max(m_PreloadBlock1High, m_PreloadBlock1);
	}

	std::uint64_t XStream::Alloc(std::uint64_t alignment) {
		m_Pos = (m_Pos + alignment - 1) & ~(alignment - 1);
		m_HighWater[m_Block] = std::max(m_HighWater[m_Block], m_Pos);
		return m_Pos;
	}

	bool XStream::Load(void* dst, std::size_t size) {
		if (Failed()) {
			return false;
		}
		if (size && XBlockIsStored(m_Block)) {
			if (m_Stream.size() - m_Cursor < size) {
				return Fail(std::format("read of {} bytes runs past the end of the stream", size));
			}
			if (dst) {
				std::memcpy(dst, m_Stream.data() + m_Cursor, size);
			}
			if (m_LoadLog) {
				m_LoadLog->push_back({ m_Block, m_Pos, m_Cursor, size });
			}
			m_Cursor += size;
		}
		else if (dst) {
			std::memset(dst, 0, size);
		}
		Advance(size);
		return true;
	}

	bool XStream::LoadString(std::string* out) {
		if (Failed()) {
			return false;
		}
		const auto* begin = reinterpret_cast<const char*>(m_Stream.data() + m_Cursor);
		const std::size_t length = strnlen(begin, m_Stream.size() - m_Cursor);
		if (length == m_Stream.size() - m_Cursor) {
			return Fail("inline string runs past the end of the stream");
		}
		if (out) {
			out->assign(begin, length);
		}
		if (m_StringLog) {
			m_StringLog->emplace((static_cast<std::uint64_t>(m_Block) << 60) | m_Pos, std::string(begin, length));
		}
		m_Cursor += length + 1;
		Advance(length + 1);
		return true;
	}

	std::uint64_t XStream::Insert() {
		const int saved = m_Block;
		Switch(XBlockVirtual);
		const std::uint64_t slot = (m_Pos + 7) & ~7ull;
		m_Pos = slot;
		Advance(8);
		Switch(saved);
		if (m_InsertLog) {
			m_InsertLog->emplace((static_cast<std::uint64_t>(XBlockVirtual) << 60) | slot, m_Cursor);
		}
		return slot;
	}

	void XStream::Restore(std::size_t cursor, int block, const std::array<std::uint64_t, kXBlockCount>& positions) {
		m_Cursor = cursor;
		m_BlockPos = positions;
		m_HighWater = positions;
		m_HighWater[XBlockTemp] = ~0ull;
		m_Block = block;
		m_Pos = positions[block];
		m_Depth = 0;
	}

	std::array<std::uint64_t, kXBlockCount> XStream::Positions() const {
		std::array<std::uint64_t, kXBlockCount> out = m_BlockPos;
		out[m_Block] = m_Pos;
		return out;
	}

	bool XStream::Reference(std::uint64_t stored, const char* what) {
		const std::uint64_t value = stored - 1;
		const int block = static_cast<int>(value >> 60);
		const std::uint64_t offset = value & 0x0FFFFFFFFFFFFFFFull;
		++m_References;
		if (m_Detached) {
			return true;
		}
		if (block >= static_cast<int>(kXBlockCount) || offset >= std::max<std::uint64_t>(m_HighWater[block], 1)) {
			return Fail(std::format("{}: reference 0x{:X} (block {} +0x{:X}) points past anything loaded", what, stored,
				block, offset));
		}
		return true;
	}
}
