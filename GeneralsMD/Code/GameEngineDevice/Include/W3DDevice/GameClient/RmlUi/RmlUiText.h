/*
**	Command & Conquer Generals Zero Hour(tm)
**	DX9 port: the game's UTF-16 text to RmlUi's UTF-8 and back.
*/

// Ronin @feature 28/10/2026 DX9: WideCharToMultiByte, not UnicodeString::translate, which goes through the process code page.
// Malformed input becomes U+FFFD; an empty input gives an empty output.
#pragma once

#include "Common/UnicodeString.h"
#include <windows.h>
#include <string>

inline std::string unicodeToUtf8(const UnicodeString &text)
{
	const WideChar *s = text.str();
	const int length = s ? (int)wcslen(s) : 0;
	if (length == 0)
		return std::string();
	const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, s, length, nullptr, 0, nullptr, nullptr);
	std::string out(bytes > 0 ? bytes : 0, '\0');
	if (bytes > 0)
		::WideCharToMultiByte(CP_UTF8, 0, s, length, &out[0], bytes, nullptr, nullptr);
	return out;
}

inline UnicodeString utf8ToUnicode(const std::string &text)
{
	UnicodeString out;
	if (text.empty())
		return out;
	const int chars = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
	if (chars <= 0)
		return out;
	std::wstring wide(chars, L'\0');
	::MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &wide[0], chars);
	out.set(wide.c_str());
	return out;
}
