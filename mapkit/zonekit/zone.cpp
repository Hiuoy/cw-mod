#include "zone.hpp"
#include "patch.hpp"

#include <fstream>

namespace MapKit::Zone {
	bool ReadWholeFile(const std::filesystem::path& path, std::vector<std::uint8_t>& data) {
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file) {
			return false;
		}
		const std::streamsize size = file.tellg();
		file.seekg(0);
		data.resize(static_cast<std::size_t>(size));
		return size == 0 || static_cast<bool>(file.read(reinterpret_cast<char*>(data.data()), size));
	}

	bool WriteWholeFile(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		return file && file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
	}

	bool LoadZone(const std::filesystem::path& ffPath, LoadedZone& out, std::string& error) {
		out = LoadedZone{};
		out.name = ffPath.stem().string();

		std::vector<std::uint8_t> file;
		if (!ReadWholeFile(ffPath, file)) {
			error = "could not read " + ffPath.string();
			return false;
		}
		if (!ReadFastFile(file, out.baseHeader, out.stream, &out.blocks, error)) {
			error = ffPath.filename().string() + ": " + error;
			return false;
		}
		out.header = out.baseHeader;
		out.baseStreamSize = out.stream.size();

		std::filesystem::path fdPath = ffPath;
		fdPath.replace_extension(".fd");
		std::error_code ec;
		if (!std::filesystem::exists(fdPath, ec)) {
			return true;
		}

		std::vector<std::uint8_t> patchData;
		PatchFile patch;
		if (!ReadWholeFile(fdPath, patchData) || !ReadPatchFile(patchData, patch, error)) {
			error = fdPath.filename().string() + ": " + error;
			return false;
		}
		if (patch.base.ToStruct() != out.baseHeader.ToStruct()) {
			error = "Patch file exists but is not for this fast file '" + out.name + "'";
			return false;
		}

		std::vector<std::uint8_t> current;
		if (!ApplyVcdiff(patch.delta, out.stream, current, error)) {
			error = fdPath.filename().string() + ": " + error;
			return false;
		}
		if (current.size() != patch.result.StreamSize()) {
			error = fdPath.filename().string() + ": patched stream is " + std::to_string(current.size())
				+ " bytes, header says " + std::to_string(patch.result.StreamSize());
			return false;
		}

		out.stream = std::move(current);
		out.header = std::move(patch.result);
		out.patched = true;
		return true;
	}
}
