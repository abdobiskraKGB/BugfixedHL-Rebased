#include <cstdlib>
#include <iostream>
#include <string>
#include "../gameui/console_arabic_text.h"

static int checks = 0;
static void Check(bool passed, const char *name)
{
	++checks;
	if (!passed)
	{
		std::cerr << "FAIL: " << name << '\n';
		std::exit(1);
	}
}

int main()
{
	using namespace ConsoleArabicText;
	Check(MakePreview(L"").visual.empty(), "empty input");
	Check(!MakePreview(L"status").containsArabic, "ASCII does not show Arabic preview");
	Check(MakePreview(L"bind x \"say hello\"; status").visual == L"bind x \"say hello\"; status",
		"ASCII syntax is unchanged");
	const std::wstring logical = L"name \"\u0633\u0644\u0627\u0645\"";
	const std::wstring original = logical;
	const auto preview = MakePreview(logical);
	Check(preview.containsArabic, "Arabic detection");
	Check(preview.visual == L"name \"\uFEE1\uFEFC\uFEB3\"", "salaam forms, ligature, command and quotes");
	Check(logical == original, "source command is never mutated");
	Check(MakePreview(L"\u0628\u0628").visual == L"\uFE90\uFE91", "joining and visual order");
	Check(MakePreview(L"\u0644\u0627").visual == L"\uFEFB", "lam-alef ligature");
	const auto mixed = MakePreview(L"say \"\u0645\u0631\u062D\u0628\u0627 123 test\"");
	Check(mixed.visual.find(L"123") != std::wstring::npos, "digits are not reversed");
	Check(mixed.visual.find(L"test") != std::wstring::npos, "Latin run is not reversed");
	Check(mixed.visual.compare(0, 5, L"say \"") == 0 && mixed.visual.back() == L'"', "command delimiters preserved");
	const std::u32string scalarText = U"A\U0001F600\u0633\U0001F680\U0010FFFF";
	Check(DecodeWide(EncodeWide(scalarText)) == scalarText, "Unicode scalar round trip");
	const auto emoji = DecodeWide(MakePreview(EncodeWide(U"\u0628 \U0001F600 \u0628")).visual);
	Check(emoji.find(U'\U0001F600') != std::u32string::npos, "emoji survives bidi as one scalar");
	Check(emoji.find(U'\uFFFD') == std::u32string::npos, "valid emoji is not replaced");
	std::wstring broken(1, static_cast<wchar_t>(0xD800));
	Check(DecodeWide(broken) == U"\uFFFD", "unpaired high surrogate is replaced");
	broken[0] = static_cast<wchar_t>(0xDC00);
	Check(DecodeWide(broken) == U"\uFFFD", "unpaired low surrogate is replaced");
	const std::wstring presentation = L"name \"\uFEFB\"";
	Check(MakePreview(presentation).visual == presentation, "already-shaped input is not shaped twice");
	const std::wstring vocalized = L"\u0628\u064E";
	Check(MakePreview(vocalized).visual == L"\uFE8F" && vocalized.size() == 2,
		"existing strip-diacritics setting affects display only");
	{
		// caret after Arabic text sits at the left edge of the Arabic run
		const auto c1 = MakePreviewWithCaret(L"name \u0639\u0628\u062F\u0648", true);
		Check(c1.visual == L"name |\uFEED\uFEAA\uFE92\uFECB", "caret left of trailing Arabic run");
		Check(MakePreviewWithCaret(L"name \u0639\u0628\u062F\u0648", false).visual == L"name  \uFEED\uFEAA\uFE92\uFECB",
			"blink-off keeps the same width");
		// caret after Latin text follows it
		Check(MakePreviewWithCaret(L"say \u0645\u0631\u062D\u0628\u0627 hi", true).visual.back() == L'|', "caret after Latin tail");
		Check(MakePreviewWithCaret(L"status", true).visual == L"status|", "plain ASCII");
		// the caret never changes the shaping of the real letters
		Check(MakePreviewWithCaret(L"\u0628\u0628", true).visual == L"|\uFE90\uFE91", "joining preserved with caret");
		// a hamza typed by the user must not be mistaken for the caret
		Check(MakePreviewWithCaret(L"\u0621\u0621", true).visual == L"|\uFE80\uFE80", "typed hamza kept");
	}
	{
		// text that starts with Arabic is a right-to-left paragraph (caret at the left end)
		bool rtl = false;
		MakePreviewWithCaret(L"\u0639\u0628\u062F\u0648 name", true, &rtl);
		Check(rtl, "Arabic first -> RTL paragraph");
		MakePreviewWithCaret(L"name \u0639\u0628\u062F\u0648", true, &rtl);
		Check(!rtl, "Latin first -> LTR paragraph");
		Check(MakePreviewWithCaret(L"\u0639\u0628\u062F\u0648", true).visual.front() == L'|', "RTL: caret at the left end");
	}
	std::cout << checks << " checks passed; wchar_t=" << sizeof(wchar_t) << " bytes\n";
	return 0;
}
