#include "common.h"

#include <shlobj.h>

namespace bridge
{
	namespace
	{
		HANDLE  g_log = INVALID_HANDLE_VALUE;
		SRWLOCK g_logLock = SRWLOCK_INIT;

		void write_raw(const char* s, std::size_t n)
		{
			if (g_log != INVALID_HANDLE_VALUE) {
				DWORD written = 0;
				WriteFile(g_log, s, static_cast<DWORD>(n), &written, nullptr);
			}
		}
	}

	std::string narrow(std::wstring_view w)
	{
		if (w.empty()) {
			return {};
		}
		const int   n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
		std::string s(static_cast<std::size_t>(n > 0 ? n : 0), '\0');
		if (n > 0) {
			WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
		}
		return s;
	}

	std::wstring widen(std::string_view s)
	{
		if (s.empty()) {
			return {};
		}
		const int    n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
		std::wstring w(static_cast<std::size_t>(n > 0 ? n : 0), L'\0');
		if (n > 0) {
			MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
		}
		return w;
	}

	std::wstring exe_path()
	{
		std::wstring buf(1024, L'\0');
		for (;;) {
			const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
			if (n < buf.size()) {
				buf.resize(n);
				return buf;
			}
			buf.resize(buf.size() * 2);
		}
	}

	std::wstring game_dir()
	{
		auto       p = exe_path();
		const auto pos = p.find_last_of(L"\\/");
		return pos == std::wstring::npos ? std::wstring() : p.substr(0, pos + 1);
	}

	std::wstring base_name(std::wstring_view path)
	{
		const auto pos = path.find_last_of(L"\\/");
		return std::wstring(pos == std::wstring_view::npos ? path : path.substr(pos + 1));
	}

	bool iequals(std::wstring_view a, std::wstring_view b)
	{
		return a.size() == b.size() &&
		       CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
	}

	bool file_exists(const std::wstring& path)
	{
		const DWORD attr = GetFileAttributesW(path.c_str());
		return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
	}

	bool read_file(const std::wstring& path, std::vector<std::uint8_t>& out, std::string& err)
	{
		out.clear();
		const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE) {
			err = "cannot open (GetLastError=" + std::to_string(GetLastError()) + ")";
			return false;
		}
		LARGE_INTEGER size{};
		if (!GetFileSizeEx(h, &size) || size.QuadPart > 0x40000000ll) {
			err = "cannot size file or file larger than 1 GiB";
			CloseHandle(h);
			return false;
		}
		out.resize(static_cast<std::size_t>(size.QuadPart));
		std::size_t done = 0;
		while (done < out.size()) {
			DWORD       got = 0;
			const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out.size() - done, 1u << 24));
			if (!ReadFile(h, out.data() + done, want, &got, nullptr) || got == 0) {
				err = "read failed (GetLastError=" + std::to_string(GetLastError()) + ")";
				CloseHandle(h);
				return false;
			}
			done += got;
		}
		CloseHandle(h);
		return true;
	}

	std::wstring Version::wstr(wchar_t sep) const
	{
		std::wstring s;
		for (int i = 0; i < 4; ++i) {
			if (i) {
				s += sep;
			}
			s += std::to_wstring(v[i]);
		}
		return s;
	}

	std::string Version::str(char sep) const { return narrow(wstr(static_cast<wchar_t>(sep))); }

	bool Version::parse(std::wstring_view text, Version& out)
	{
		Version     r{};
		int         idx = 0;
		bool        any = false;
		std::uint32_t cur = 0;
		for (const wchar_t c : text) {
			if (c >= L'0' && c <= L'9') {
				cur = cur * 10 + static_cast<std::uint32_t>(c - L'0');
				if (cur > 0xFFFF) {
					return false;
				}
				any = true;
			} else if (c == L'.' || c == L'-' || c == L',') {
				if (!any || idx >= 3) {
					return false;
				}
				r.v[idx++] = static_cast<std::uint16_t>(cur);
				cur = 0;
				any = false;
			} else if (c == L' ' || c == L'\0') {
				continue;
			} else {
				return false;
			}
		}
		if (!any) {
			return false;
		}
		r.v[idx] = static_cast<std::uint16_t>(cur);
		out = r;
		return true;
	}

	Version Version::unpack(std::uint32_t p)
	{
		Version r{};
		r.v[0] = static_cast<std::uint16_t>((p >> 24) & 0xFF);
		r.v[1] = static_cast<std::uint16_t>((p >> 16) & 0xFF);
		r.v[2] = static_cast<std::uint16_t>((p >> 4) & 0xFFF);
		r.v[3] = static_cast<std::uint16_t>(p & 0xF);
		return r;
	}

	// Reads the exe version the same way the legacy CommonLib does
	// (NG 539d4ce src/REL/Version.cpp:7-33): the ProductVersion string.
	bool exe_product_version(Version& out)
	{
		const auto path = exe_path();
		DWORD      dummy = 0;
		const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
		if (!size) {
			return false;
		}
		std::vector<char> buf(size);
		if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) {
			return false;
		}
		void* p = nullptr;
		UINT  len = 0;
		if (VerQueryValueW(buf.data(), L"\\StringFileInfo\\040904B0\\ProductVersion", &p, &len) && p && len) {
			if (Version::parse(std::wstring_view(static_cast<const wchar_t*>(p), len), out)) {
				return true;
			}
		}
		if (VerQueryValueW(buf.data(), L"\\", &p, &len) && p && len >= sizeof(VS_FIXEDFILEINFO)) {
			const auto* ffi = static_cast<const VS_FIXEDFILEINFO*>(p);
			out.v[0] = HIWORD(ffi->dwProductVersionMS);
			out.v[1] = LOWORD(ffi->dwProductVersionMS);
			out.v[2] = HIWORD(ffi->dwProductVersionLS);
			out.v[3] = LOWORD(ffi->dwProductVersionLS);
			return true;
		}
		return false;
	}

	std::wstring log_dir()
	{
		std::wstring dir;
		PWSTR        docs = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docs)) && docs) {
			const std::wstring root = std::wstring(docs) + L"\\My Games\\";
			// [Assumption] Steam uses "Skyrim Special Edition", GOG "Skyrim Special Edition GOG"
			// (NG 539d4ce src/SKSE/Logger.cpp:82-90). Pick GOG only if it is the one that exists.
			const std::wstring steam = root + L"Skyrim Special Edition";
			const std::wstring gog = root + L"Skyrim Special Edition GOG";
			const bool         useGog = GetFileAttributesW(steam.c_str()) == INVALID_FILE_ATTRIBUTES &&
			                            GetFileAttributesW(gog.c_str()) != INVALID_FILE_ATTRIBUTES;
			dir = (useGog ? gog : steam) + L"\\SKSE";
			SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
			dir += L"\\";
		}
		if (docs) {
			CoTaskMemFree(docs);
		}
		if (dir.empty()) {
			dir = game_dir();
		}
		return dir;
	}

	void log_open()
	{
		if (g_log != INVALID_HANDLE_VALUE) {
			return;
		}
		g_log = CreateFileW((log_dir() + L"SKSE-Bridge.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, nullptr);
	}

	void logf(const char* fmt, ...)
	{
		char    msg[2048];
		va_list args;
		va_start(args, fmt);
		const int n = std::vsnprintf(msg, sizeof(msg), fmt, args);
		va_end(args);
		if (n < 0) {
			return;
		}
		SYSTEMTIME st{};
		GetLocalTime(&st);
		char      line[2200];
		const int m = std::snprintf(line, sizeof(line), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] [t%lu] %s\r\n",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
			static_cast<unsigned long>(GetCurrentThreadId()), msg);
		if (m <= 0) {
			return;
		}
		AcquireSRWLockExclusive(&g_logLock);
		write_raw(line, std::min<std::size_t>(static_cast<std::size_t>(m), sizeof(line) - 1));
		if (g_log != INVALID_HANDLE_VALUE) {
			FlushFileBuffers(g_log);  // a fatal dialog in a legacy plugin may kill the process
		}
		ReleaseSRWLockExclusive(&g_logLock);
	}
}
