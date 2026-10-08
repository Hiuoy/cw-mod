#include "xwriter.hpp"
#include "xstream.hpp"

#include <cstring>

namespace MapKit::Zone {
	XWriter::XWriter(int block) : m_Block(block) {
		m_Stack.reserve(64);
	}

	void XWriter::Switch(int block) {
		if (block == m_Block) {
			return;
		}
		m_BlockPos[m_Block] = m_Pos;
		m_Block = block;
		m_Pos = m_BlockPos[block];
	}

	void XWriter::Push(int block) {
		if (block == XBlockCollisionAlias) {
			block = m_Block;
		}
		Frame frame;
		frame.block = m_Block;
		Switch(block);
		frame.pos = m_Pos;
		m_Stack.push_back(frame);
	}

	void XWriter::Pop() {
		if (m_Stack.empty()) {
			return;
		}
		const Frame frame = m_Stack.back();
		m_Stack.pop_back();
		if (m_Block == XBlockTemp) {
			m_Pos = frame.pos;
		}
		else if (m_Block == 1) {
			m_Pos = (m_Pos + 15) & ~15ull;
		}
		Switch(frame.block);
	}

	std::uint64_t XWriter::Alloc(std::uint64_t alignment) {
		m_Pos = (m_Pos + alignment - 1) & ~(alignment - 1);
		return m_Pos;
	}

	void XWriter::Write(const void* data, std::size_t size) {
		if (XBlockIsStored(m_Block) && size) {
			const auto* bytes = static_cast<const std::uint8_t*>(data);
			m_Bytes.insert(m_Bytes.end(), bytes, bytes + size);
		}
		m_Pos += size;
	}

	void XWriter::WriteString(std::string_view text) {
		Write(text.data(), text.size());
		const char nul = 0;
		Write(&nul, 1);
	}
}
