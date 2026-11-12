// Read-only RTTI scan of a loaded legacy module (MSVC x64 RTTI layout).
//
// Why: a legacy plugin class that derives from a game interface carries a vtable laid out for 1.6.1170.
// If 1.7.99 inserted virtual functions into that interface, the game now calls the wrong slots of the
// plugin's vtable. This cannot be fixed by ID translation. The scan finds such classes from the plugin's
// own RTTI, so the report names the class instead of guessing.
//
// Interfaces with inserted slots: CommonLibSSE-NG 9955ae3 (kAE1799AddedVFuncCount + VersionShift). [Verified]
//   include/RE/M/MenuEventHandler.h:42-59      2 slots inserted at index 2
//   include/RE/P/PlayerInputHandler.h:39-52    2 slots inserted at index 2 (HeldStateHandler, LookHandler, ... derive)
//   include/RE/B/BSSystemUtility.h:53-56       1 slot  inserted at index 2
//   include/RE/B/BSSaveDataSystemUtility.h:137 6 slots inserted at index 5
// Legacy slot counts: CommonLibSSE-NG 539d4ce include/RE/M/MenuEventHandler.h:20-26 (6 slots). [Verified]
#include "common.h"

#include <algorithm>

namespace bridge
{
	namespace
	{
		struct VtableChange
		{
			const char* key;  // RTTI name without the ".?AV"/".?AU" prefix
			const char* cls;
			unsigned    insertAt;
			unsigned    added;
			const char* effect;
		};

		constexpr VtableChange kChanges[] = {
			{ "MenuEventHandler@RE@@", "RE::MenuEventHandler", 2, 2,
				"1.7.99 calls ProcessKinect/Thumbstick/MouseMove/Button at slots 4-7 (were 2-5); slots 2-3 are new (MotionGesture, Sixaxis)" },
			{ "PlayerInputHandler@RE@@", "RE::PlayerInputHandler", 2, 2,
				"1.7.99 calls ProcessThumbstick/MouseMove/Button at slots 4-6 (were 2-4); slots 2-3 are new (MotionGesture, Sixaxis)" },
			{ "BSSystemUtility@RE@@", "RE::BSSystemUtility", 2, 1, "every slot from 2 moved up by 1" },
			{ "BSSaveDataSystemUtility@RE@@", "RE::BSSaveDataSystemUtility", 5, 6, "every slot from 5 moved up by 6" },
		};

		struct Image
		{
			std::uintptr_t base{ 0 };
			std::uint32_t  size{ 0 };
			struct Sec
			{
				std::uint32_t rva, size;
				bool          exec;
			};
			std::vector<Sec> secs;

			bool in(std::uint64_t rva, std::size_t n) const { return rva < size && n <= size - rva; }
			template <class T>
			const T* at(std::uint64_t rva) const { return in(rva, sizeof(T)) ? reinterpret_cast<const T*>(base + rva) : nullptr; }
			bool exec(std::uint64_t rva) const
			{
				for (const auto& s : secs) {
					if (s.exec && rva >= s.rva && rva - s.rva < s.size) {
						return true;
					}
				}
				return false;
			}
			std::string name_at(std::uint64_t rva) const
			{
				std::string out;
				for (; in(rva, 1) && out.size() < 256; ++rva) {
					const char c = *reinterpret_cast<const char*>(base + rva);
					if (!c) {
						break;
					}
					out += c;
				}
				return out;
			}
		};

		// MSVC x64 RTTI records (all pointers are image-relative).
		struct COL
		{
			std::uint32_t signature;  // 1 on x64
			std::uint32_t offset;     // offset of this vtable's subobject in the complete object
			std::uint32_t cdOffset;
			std::int32_t  typeDescriptor;
			std::int32_t  classDescriptor;
			std::int32_t  self;
		};
		struct CHD
		{
			std::uint32_t signature;
			std::uint32_t attributes;
			std::uint32_t numBaseClasses;
			std::int32_t  baseClassArray;
		};
		struct BCD
		{
			std::int32_t  typeDescriptor;
			std::uint32_t numContainedBases;
			std::int32_t  mdisp, pdisp, vdisp;
			std::uint32_t attributes;
			std::int32_t  classDescriptor;
		};

		std::string strip(const std::string& rtti)
		{
			// ".?AVFoo@RE@@" -> "Foo@RE@@"
			return rtti.size() > 4 && rtti.compare(0, 3, ".?A") == 0 ? rtti.substr(4) : rtti;
		}

		// "Foo@Bar@@" -> "Bar::Foo" (simple names only; templates are left as they are)
		std::string pretty(const std::string& s)
		{
			if (s.find('?') != std::string::npos || s.size() < 3 || s.compare(s.size() - 2, 2, "@@") != 0) {
				return s;
			}
			std::vector<std::string> parts;
			std::size_t              pos = 0;
			const std::string        body = s.substr(0, s.size() - 2);
			while (pos <= body.size()) {
				const auto at = body.find('@', pos);
				parts.push_back(body.substr(pos, at == std::string::npos ? std::string::npos : at - pos));
				pos = at == std::string::npos ? body.size() + 1 : at + 1;
			}
			std::string out;
			for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
				out += (out.empty() ? "" : "::") + *it;
			}
			return out;
		}

		bool readable_exec(std::uint64_t addr)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
				return false;
			}
			const DWORD p = mbi.Protect & 0xFF;
			return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
		}
	}

	int scan_rtti(HMODULE mod, const std::string& modName)
	{
		Image img;
		img.base = reinterpret_cast<std::uintptr_t>(mod);
		const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(img.base);
		const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(img.base + dos->e_lfanew);
		img.size = nt->OptionalHeader.SizeOfImage;
		const auto* sec = IMAGE_FIRST_SECTION(nt);
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
			img.secs.push_back({ sec[i].VirtualAddress, std::max(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData),
				(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 });
		}

		// A vtable is preceded by a pointer to its COL. Find COLs by their self-RVA, in non-executable
		// initialized-data sections only.
		int         vtables = 0, gameDerived = 0, affected = 0;
		std::string derivedNames;
		for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
			const auto& s = sec[i];
			if ((s.Characteristics & IMAGE_SCN_MEM_EXECUTE) || !(s.Characteristics & IMAGE_SCN_CNT_INITIALIZED_DATA)) {
				continue;
			}
			const std::uint32_t end = s.VirtualAddress + std::min(s.Misc.VirtualSize, s.SizeOfRawData);
			for (std::uint32_t rva = (s.VirtualAddress + 7) & ~7u; rva + 16 <= end; rva += 8) {
				const std::uint64_t p = *reinterpret_cast<const std::uint64_t*>(img.base + rva);
				if (p < img.base || p - img.base >= img.size) {
					continue;
				}
				const std::uint64_t colRva = p - img.base;
				const COL*          col = img.at<COL>(colRva);
				if (!col || col->signature != 1 || static_cast<std::uint64_t>(col->self) != colRva) {
					continue;
				}
				const std::uint64_t vtRva = rva + 8;
				const std::uint64_t first = *reinterpret_cast<const std::uint64_t*>(img.base + vtRva);
				if (!readable_exec(first)) {
					continue;  // not a vtable (COL referenced from elsewhere)
				}
				++vtables;
				const std::string cls = strip(img.name_at(static_cast<std::uint32_t>(col->typeDescriptor) + 16));
				const CHD*        chd = img.at<CHD>(static_cast<std::uint32_t>(col->classDescriptor));
				if (!chd || chd->numBaseClasses > 256) {
					continue;
				}
				std::vector<std::string> bases;
				for (std::uint32_t b = 1; b < chd->numBaseClasses; ++b) {  // [0] is the class itself
					const auto* entry = img.at<std::int32_t>(static_cast<std::uint32_t>(chd->baseClassArray) + 4ull * b);
					const BCD*  bcd = entry ? img.at<BCD>(static_cast<std::uint32_t>(*entry)) : nullptr;
					if (bcd) {
						bases.push_back(strip(img.name_at(static_cast<std::uint32_t>(bcd->typeDescriptor) + 16)));
					}
				}
				const bool ownClass = cls.find("@RE@@") == std::string::npos;
				const bool fromGame = std::any_of(bases.begin(), bases.end(), [](const std::string& b) { return b.find("@RE@@") != std::string::npos; });
				if (!ownClass || !fromGame || col->offset != 0) {
					continue;  // report each plugin class once, from its primary vtable
				}
				++gameDerived;
				if (derivedNames.size() < 1200) {
					derivedNames += pretty(cls) + " ";
				}
				for (const auto& ch : kChanges) {
					if (std::find(bases.begin(), bases.end(), ch.key) == bases.end()) {
						continue;
					}
					unsigned slots = 0;
					for (; slots < 512; ++slots) {
						const auto* q = img.at<std::uint64_t>(vtRva + 8ull * slots);
						if (!q || !readable_exec(*q)) {
							break;
						}
					}
					++affected;
					logf("  RTTI: INCOMPATIBLE VTABLE: plugin class %s derives from %s, whose vtable gained %u slot(s) at index %u in 1.7.99. "
						 "Its vtable (%s+0x%llX, %u slots) is laid out for 1.6. %s. The game will call the wrong functions of this class.",
						pretty(cls).c_str(), ch.cls, ch.added, ch.insertAt, modName.c_str(), static_cast<unsigned long long>(vtRva), slots, ch.effect);
				}
			}
		}
		logf("  RTTI: %d vtables, %d plugin classes derive from CommonLib RE:: types%s%s", vtables, gameDerived,
			gameDerived ? ": " : "", derivedNames.c_str());
		logf("  RTTI: %d class(es) with a vtable change the bridge cannot adapt", affected);
		return affected;
	}
}
