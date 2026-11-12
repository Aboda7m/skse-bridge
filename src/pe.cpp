// Read-only inspection of a legacy DLL file. The file is parsed from disk; nothing is executed.
#include "common.h"

#include <bcrypt.h>

#include <algorithm>

namespace bridge
{
	namespace
	{
		struct Image
		{
			const std::vector<std::uint8_t>& d;
			const IMAGE_NT_HEADERS64*        nt{ nullptr };
			const IMAGE_SECTION_HEADER*      sec{ nullptr };
			unsigned                         nsec{ 0 };

			template <class T>
			const T* at(std::size_t off, std::size_t count = 1) const
			{
				if (off > d.size() || count > (d.size() - off) / sizeof(T)) {
					return nullptr;
				}
				return reinterpret_cast<const T*>(d.data() + off);
			}

			// RVA -> file offset; returns SIZE_MAX if unmapped.
			std::size_t off(std::uint32_t rva) const
			{
				for (unsigned i = 0; i < nsec; ++i) {
					const auto& s = sec[i];
					if (rva >= s.VirtualAddress && rva - s.VirtualAddress < s.SizeOfRawData) {
						return static_cast<std::size_t>(s.PointerToRawData) + (rva - s.VirtualAddress);
					}
				}
				return SIZE_MAX;
			}

			std::string cstr(std::uint32_t rva) const
			{
				const std::size_t o = off(rva);
				if (o == SIZE_MAX) {
					return {};
				}
				std::string s;
				for (std::size_t i = o; i < d.size() && d[i] && s.size() < 512; ++i) {
					s += static_cast<char>(d[i]);
				}
				return s;
			}
		};

		bool contains(const std::vector<std::uint8_t>& d, const void* pat, std::size_t n)
		{
			const auto* p = static_cast<const std::uint8_t*>(pat);
			return std::search(d.begin(), d.end(), p, p + n) != d.end();
		}

		bool contains_text(const std::vector<std::uint8_t>& d, std::string_view ascii)
		{
			if (contains(d, ascii.data(), ascii.size())) {
				return true;
			}
			const std::wstring w(ascii.begin(), ascii.end());
			return contains(d, w.data(), w.size() * sizeof(wchar_t));
		}

		std::string sha256_hex(const std::vector<std::uint8_t>& d)
		{
			BCRYPT_ALG_HANDLE  alg = nullptr;
			BCRYPT_HASH_HANDLE hash = nullptr;
			UCHAR              out[32]{};
			std::string        hex;
			if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
				return "<bcrypt unavailable>";
			}
			if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
				std::size_t done = 0;
				bool        ok = true;
				while (ok && done < d.size()) {
					const ULONG chunk = static_cast<ULONG>(std::min<std::size_t>(d.size() - done, 1u << 24));
					ok = BCryptHashData(hash, const_cast<PUCHAR>(d.data() + done), chunk, 0) >= 0;
					done += chunk;
				}
				if (ok && BCryptFinishHash(hash, out, sizeof(out), 0) >= 0) {
					static const char* digits = "0123456789abcdef";
					for (const UCHAR b : out) {
						hex += digits[b >> 4];
						hex += digits[b & 15];
					}
				}
				BCryptDestroyHash(hash);
			}
			BCryptCloseAlgorithmProvider(alg, 0);
			return hex.empty() ? "<hash failed>" : hex;
		}
	}

	bool PeReport::imports_fn(std::string_view fn) const
	{
		for (const auto& imp : imports) {
			for (const auto& n : imp.names) {
				if (n == fn) {
					return true;
				}
			}
		}
		return false;
	}

	// Case A: constant-AE build (only the versionlib path string survives).
	// Case B: multi-runtime build (both "version-{}.bin" and "versionlib-{}.bin" present,
	//         NG 539d4ce include/REL/ID.h:317-323). Heuristic on strings, not proof.
	const char* PeReport::lineage() const
	{
		if (strOffsetsMap && strVersionlib) {
			return strVersionSE ? "B" : "A";
		}
		return "unknown";
	}

	PeReport inspect_pe(const std::wstring& path)
	{
		PeReport                  r;
		std::vector<std::uint8_t> d;
		if (!read_file(path, d, r.error)) {
			return r;
		}
		r.fileSize = d.size();
		r.sha256 = sha256_hex(d);

		Image       img{ d };
		const auto* dos = img.at<IMAGE_DOS_HEADER>(0);
		if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0) {
			r.error = "not a PE file (no MZ header)";
			return r;
		}
		const auto ntOff = static_cast<std::size_t>(dos->e_lfanew);
		img.nt = img.at<IMAGE_NT_HEADERS64>(ntOff);
		if (!img.nt || img.nt->Signature != IMAGE_NT_SIGNATURE) {
			r.error = "not a PE file (no PE signature)";
			return r;
		}
		r.timeDateStamp = img.nt->FileHeader.TimeDateStamp;
		r.x64 = img.nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
		        img.nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
		if (!r.x64) {
			r.error = "not an x64 PE32+ image";
			return r;
		}
		img.nsec = img.nt->FileHeader.NumberOfSections;
		img.sec = img.at<IMAGE_SECTION_HEADER>(
			ntOff + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + img.nt->FileHeader.SizeOfOptionalHeader, img.nsec);
		if (!img.sec) {
			r.error = "truncated section table";
			return r;
		}
		const auto& dirs = img.nt->OptionalHeader.DataDirectory;
		const auto  ndirs = img.nt->OptionalHeader.NumberOfRvaAndSizes;

		// exports
		std::uint32_t versionDataRva = 0;
		if (ndirs > IMAGE_DIRECTORY_ENTRY_EXPORT && dirs[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress) {
			const auto* exp = img.at<IMAGE_EXPORT_DIRECTORY>(img.off(dirs[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress));
			if (exp && exp->NumberOfNames < 65536) {
				const auto* names = img.at<std::uint32_t>(img.off(exp->AddressOfNames), exp->NumberOfNames);
				const auto* ords = img.at<std::uint16_t>(img.off(exp->AddressOfNameOrdinals), exp->NumberOfNames);
				const auto* funcs = img.at<std::uint32_t>(img.off(exp->AddressOfFunctions), exp->NumberOfFunctions);
				for (std::uint32_t i = 0; names && i < exp->NumberOfNames; ++i) {
					auto name = img.cstr(names[i]);
					if (name == "SKSEPlugin_Load") {
						r.hasLoad = true;
					}
					if (name == "SKSEPlugin_Version" && ords && funcs && ords[i] < exp->NumberOfFunctions) {
						versionDataRva = funcs[ords[i]];
					}
					r.exports.push_back(std::move(name));
				}
			}
		}
		if (versionDataRva) {
			if (const auto* vd = img.at<PluginVersionData>(img.off(versionDataRva))) {
				r.versionData = *vd;
				r.hasVersionData = true;
			}
		}

		// imports
		if (ndirs > IMAGE_DIRECTORY_ENTRY_IMPORT && dirs[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress) {
			std::size_t o = img.off(dirs[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
			for (unsigned guard = 0; o != SIZE_MAX && guard < 4096; ++guard, o += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
				const auto* desc = img.at<IMAGE_IMPORT_DESCRIPTOR>(o);
				if (!desc || (!desc->Name && !desc->FirstThunk)) {
					break;
				}
				PeImport imp;
				imp.dll = img.cstr(desc->Name);
				const std::uint32_t thunkRva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
				std::size_t         to = img.off(thunkRva);
				for (unsigned g2 = 0; to != SIZE_MAX && g2 < 65536; ++g2, to += sizeof(std::uint64_t)) {
					const auto* thunk = img.at<std::uint64_t>(to);
					if (!thunk || !*thunk) {
						break;
					}
					if (*thunk & IMAGE_ORDINAL_FLAG64) {
						imp.names.push_back("#" + std::to_string(*thunk & 0xFFFF));
					} else {
						imp.names.push_back(img.cstr(static_cast<std::uint32_t>(*thunk) + 2));  // skip Hint
					}
				}
				r.imports.push_back(std::move(imp));
			}
		}

		// debug directory (CodeView RSDS)
		if (ndirs > IMAGE_DIRECTORY_ENTRY_DEBUG && dirs[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress) {
			const auto        n = dirs[IMAGE_DIRECTORY_ENTRY_DEBUG].Size / sizeof(IMAGE_DEBUG_DIRECTORY);
			const auto* const dbg = img.at<IMAGE_DEBUG_DIRECTORY>(img.off(dirs[IMAGE_DIRECTORY_ENTRY_DEBUG].VirtualAddress), n);
			for (std::size_t i = 0; dbg && i < n; ++i) {
				if (dbg[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || dbg[i].SizeOfData < 24) {
					continue;
				}
				const auto* cv = img.at<std::uint8_t>(dbg[i].PointerToRawData, dbg[i].SizeOfData);
				if (!cv || std::memcmp(cv, "RSDS", 4) != 0) {
					continue;
				}
				GUID g;
				std::memcpy(&g, cv + 4, sizeof(g));
				std::memcpy(&r.pdbAge, cv + 20, 4);
				char buf[64];
				std::snprintf(buf, sizeof(buf), "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
					static_cast<unsigned long>(g.Data1), g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
					g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
				r.pdbGuid = buf;
				for (std::size_t k = 24; k < dbg[i].SizeOfData && cv[k]; ++k) {
					r.pdbPath += static_cast<char>(cv[k]);
				}
				r.hasPdb = true;
				break;
			}
		}

		r.strOffsetsMap = contains_text(d, "CommonLibSSEOffsets");
		r.strVersionlib = contains_text(d, "versionlib-");
		r.strVersionSE = contains_text(d, "Plugins/version-");
		r.strSkseRuntime = contains_text(d, "SKSE_RUNTIME");
		r.ok = true;
		return r;
	}

	void log_report(const PeReport& r)
	{
		if (!r.ok) {
			logf("  inspect: FAILED: %s", r.error.c_str());
			if (!r.sha256.empty()) {
				logf("  sha256: %s", r.sha256.c_str());
			}
			return;
		}
		logf("  size: %llu bytes", static_cast<unsigned long long>(r.fileSize));
		logf("  sha256: %s", r.sha256.c_str());
		logf("  pe: x64=%d TimeDateStamp=0x%08X", r.x64 ? 1 : 0, r.timeDateStamp);
		if (r.hasPdb) {
			logf("  pdb: guid=%s age=%u path=%s", r.pdbGuid.c_str(), r.pdbAge, r.pdbPath.c_str());
		} else {
			logf("  pdb: no CodeView RSDS record");
		}
		std::string line;
		for (const auto& e : r.exports) {
			line += e + " ";
		}
		logf("  exports (%zu): %s", r.exports.size(), line.c_str());
		if (r.hasVersionData) {
			const auto& v = r.versionData;
			char        name[257]{}, author[257]{};
			std::memcpy(name, v.pluginName, 256);
			std::memcpy(author, v.author, 256);
			logf("  SKSEPlugin_Version: dataVersion=%u pluginVersion=%s name=\"%s\" author=\"%s\"", v.dataVersion,
				Version::unpack(v.pluginVersion).str('.').c_str(), name, author);
			logf("    versionIndependence=0x%X (AddressLibrary=%d Signatures=%d StructsPost629=%d) versionIndependenceEx=0x%X (NoStructUse=%d) xseMinimum=%s",
				v.versionIndependence, (v.versionIndependence & 1) ? 1 : 0, (v.versionIndependence & 2) ? 1 : 0,
				(v.versionIndependence & 4) ? 1 : 0, v.versionIndependenceEx, (v.versionIndependenceEx & 1) ? 1 : 0,
				Version::unpack(v.xseMinimum).str('.').c_str());
			line.clear();
			for (const auto cv : v.compatibleVersions) {
				if (cv) {
					line += Version::unpack(cv).str('.') + " ";
				}
			}
			logf("    compatibleVersions: %s", line.empty() ? "(none)" : line.c_str());
		} else {
			logf("  SKSEPlugin_Version: not exported or not readable");
		}
		static const char* const flagged[] = { "OpenFileMappingW", "CreateFileMappingW", "MapViewOfFile",
			"GetFileVersionInfoSizeW", "GetFileVersionInfoW", "GetFileVersionInfoA", "GetFileVersionInfoExW", "VerQueryValueW",
			"GetEnvironmentVariableW", "LoadLibraryW", "LoadLibraryA", "LoadLibraryExW", "CreateFileW" };
		line.clear();
		for (const char* f : flagged) {
			if (r.imports_fn(f)) {
				line += std::string(f) + " ";
			}
		}
		logf("  flagged imports: %s", line.empty() ? "(none)" : line.c_str());
		for (const auto& imp : r.imports) {
			line.clear();
			for (const auto& n : imp.names) {
				if (line.size() > 1700) {
					line += "...";
					break;
				}
				line += n + " ";
			}
			logf("  import %s (%zu): %s", imp.dll.c_str(), imp.names.size(), line.c_str());
		}
		logf("  strings: CommonLibSSEOffsets=%d versionlib-=%d Plugins/version-=%d SKSE_RUNTIME=%d",
			r.strOffsetsMap ? 1 : 0, r.strVersionlib ? 1 : 0, r.strVersionSE ? 1 : 0, r.strSkseRuntime ? 1 : 0);
		logf("  lineage: Case %s (string heuristic)", r.lineage());
	}
}
