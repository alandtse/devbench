#pragma once

#include <string>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

// Skyrim's narrow strings (form names, console text, save names, book text) are in the Windows
// ANSI code page; devbench's JSON and its clients are UTF-8. Text crosses that line only here,
// by code page, never by guessing from the bytes.
namespace dvb
{
	namespace detail
	{
		inline bool IsAscii(std::string_view a_text)
		{
			for (const char c : a_text)
				if (static_cast<unsigned char>(c) >= 0x80)
					return false;
			return true;
		}

		inline std::string Convert(std::string_view a_text, UINT a_fromCp, UINT a_toCp)
		{
			if (a_text.empty() || IsAscii(a_text))
				return std::string(a_text);
			const int len = static_cast<int>(a_text.size());
			const int wide = ::MultiByteToWideChar(a_fromCp, 0, a_text.data(), len, nullptr, 0);
			if (wide <= 0)
				return std::string(a_text);
			std::wstring w(static_cast<std::size_t>(wide), L'\0');
			::MultiByteToWideChar(a_fromCp, 0, a_text.data(), len, w.data(), wide);
			const int narrow = ::WideCharToMultiByte(a_toCp, 0, w.data(), wide, nullptr, 0, nullptr, nullptr);
			if (narrow <= 0)
				return std::string(a_text);
			std::string out(static_cast<std::size_t>(narrow), '\0');
			::WideCharToMultiByte(a_toCp, 0, w.data(), wide, out.data(), narrow, nullptr, nullptr);
			return out;
		}
	}

	/// Engine ANSI text (code page a_cp) as UTF-8. Bytes the code page cannot decode become U+FFFD.
	inline std::string FromGame(std::string_view a_text, UINT a_cp = CP_ACP) { return detail::Convert(a_text, a_cp, CP_UTF8); }

	/// A null engine string is empty.
	inline std::string FromGame(const char* a_text, UINT a_cp = CP_ACP) { return a_text ? FromGame(std::string_view(a_text), a_cp) : std::string{}; }

	/// UTF-8 text as engine ANSI text (code page a_cp). Characters outside the code page become '?'.
	inline std::string ToGame(std::string_view a_text, UINT a_cp = CP_ACP) { return detail::Convert(a_text, CP_UTF8, a_cp); }
}
