#pragma once

#include <cstdint>
#include <string>
#include "../hud/arabic_text.h"

// Display-only adapter. Never pass its output to ClientCmd, say, or name.
namespace ConsoleArabicText
{
inline bool IsHighSurrogate(std::uint32_t c) { return c >= 0xD800 && c <= 0xDBFF; }
inline bool IsLowSurrogate(std::uint32_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

// The supplied ToVisualWide treats UTF-16 code units as separate characters.
// Decode surrogate pairs here so bidi reordering cannot split an emoji in two.
inline std::u32string DecodeWide(const std::wstring &text)
{
	std::u32string result;
	result.reserve(text.size());
	for (size_t i = 0; i < text.size(); ++i)
	{
		std::uint32_t c = static_cast<std::uint32_t>(text[i]);
		if (sizeof(wchar_t) == 2 && IsHighSurrogate(c))
		{
			if (i + 1 < text.size() && IsLowSurrogate(static_cast<std::uint32_t>(text[i + 1])))
			{
				c = 0x10000 + ((c - 0xD800) << 10) + (static_cast<std::uint32_t>(text[++i]) - 0xDC00);
			}
			else
				c = 0xFFFD;
		}
		else if (IsHighSurrogate(c) || IsLowSurrogate(c) || c > 0x10FFFF)
			c = 0xFFFD;

		result.push_back(static_cast<char32_t>(c));
	}
	return result;
}

inline std::wstring EncodeWide(const std::u32string &text)
{
	std::wstring result;
	for (char32_t value : text)
	{
		std::uint32_t c = static_cast<std::uint32_t>(value);
		if (IsHighSurrogate(c) || IsLowSurrogate(c) || c > 0x10FFFF)
			c = 0xFFFD;
		if (sizeof(wchar_t) == 2 && c >= 0x10000)
		{
			c -= 0x10000;
			result.push_back(static_cast<wchar_t>(0xD800 + (c >> 10)));
			result.push_back(static_cast<wchar_t>(0xDC00 + (c & 0x3FF)));
		}
		else
			result.push_back(static_cast<wchar_t>(c));
	}
	return result;
}

struct Preview
{
	std::wstring visual;
	bool containsArabic = false;
};

inline Preview MakePreview(const std::wstring &logical)
{
	const std::u32string codepoints = DecodeWide(logical);
	Preview result;
	result.containsArabic = ArabicText::ContainsArabic(codepoints);
	// Console syntax has an LTR base direction. Arabic runs are shaped/reordered
	// inside it, while command names, numbers and quotes remain logical input.
	result.visual = EncodeWide(ArabicText::ToVisual(codepoints, ArabicText::DIR_LTR));
	return result;
}

// True when the last strong (letter) character of the text is Arabic, i.e. the next typed character
// would continue an RTL run and the caret has to be drawn at the left edge of that run.
inline bool LastStrongIsArabic(const std::u32string &codepoints)
{
	for (size_t i = codepoints.size(); i-- > 0;)
	{
		const auto cls = ArabicText::detail::Classify(codepoints[i]);
		if (cls == ArabicText::detail::C_R)
			return true;
		if (cls == ArabicText::detail::C_L)
			return false;
	}
	return false;
}

// Like MakePreview, but also places a caret ('|' or a blank of the same width class) at the LOGICAL
// end of the text. Used when the preview replaces the native input line, whose own caret gets covered.
// When the text ends in Arabic, a non-joining hamza is appended before shaping so that the caret lands
// where the next Arabic letter would appear; that hamza is then replaced by the caret.
inline Preview MakePreviewWithCaret(const std::wstring &logical, bool caretOn)
{
	const std::u32string codepoints = DecodeWide(logical);
	Preview result;
	result.containsArabic = ArabicText::ContainsArabic(codepoints);
	const char32_t caret = caretOn ? U'|' : U' ';

	if (result.containsArabic && LastStrongIsArabic(codepoints))
	{
		const std::u32string base = ArabicText::ToVisual(codepoints, ArabicText::DIR_LTR);
		std::u32string probe = codepoints;
		probe.push_back(U'\u0621');
		std::u32string shaped = ArabicText::ToVisual(probe, ArabicText::DIR_LTR);

		// Everything to the right of the inserted glyph is identical to the plain shaping
		size_t suffix = 0;
		while (suffix < base.size() && suffix < shaped.size() &&
		       base[base.size() - 1 - suffix] == shaped[shaped.size() - 1 - suffix])
			++suffix;

		if (shaped.size() == base.size() + 1 && suffix < shaped.size())
		{
			const size_t at = shaped.size() - suffix - 1;
			if (shaped[at] == U'\u0621' || shaped[at] == U'\uFE80')
			{
				shaped[at] = caret;
				result.visual = EncodeWide(shaped);
				return result;
			}
		}
	}

	// Text ends in Latin/neutral (or shaping was unexpected): the caret simply follows the text
	std::u32string withCaret = codepoints;
	withCaret.push_back(caret);
	result.visual = EncodeWide(ArabicText::ToVisual(withCaret, ArabicText::DIR_LTR));
	return result;
}
}
