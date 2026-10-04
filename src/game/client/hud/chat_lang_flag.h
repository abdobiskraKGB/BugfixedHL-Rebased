// chat_lang_flag.h
// Small flag next to the chat input line showing the current keyboard language
// (Arabic -> Algeria, English -> United Kingdom, French -> France).
// The flags are drawn from a tiny pixel map, so no image files are needed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <type_traits>
#include <utility>

// If windows.h is already included (it normally is, through the precompiled header)
// the keyboard layout is read straight from Windows.
#if defined(_WIN32) && defined(_WINDOWS_)
#define CHATLANG_HAS_WINAPI 1
#endif

namespace ChatLang
{

enum class Lang
{
	None,
	Arabic,
	English,
	French
};

constexpr int FLAG_W = 24;
constexpr int FLAG_H = 16;

// G green, W white, R red, B blue
static const char *const kFlagArabic[FLAG_H] = {
	"GGGGGGGGGGGGWWWWWWWWWWWW",
	"GGGGGGGGGGGGWWWWWWWWWWWW",
	"GGGGGGGGGGGGWWWWWWWWWWWW",
	"GGGGGGGGGGRRRRRRWWWWWWWW",
	"GGGGGGGGRRRGWWWWWWWWWWWW",
	"GGGGGGGGRRGGWWRWWWWWWWWW",
	"GGGGGGGRRRGGWWRWWWWWWWWW",
	"GGGGGGGRRRGGRRRRRWWWWWWW",
	"GGGGGGGRRRGGWRRRWWWWWWWW",
	"GGGGGGGRRRGGWRWRWWWWWWWW",
	"GGGGGGGGRRGGWWWWWWWWWWWW",
	"GGGGGGGGRRRGWWWWWWWWWWWW",
	"GGGGGGGGGGRRRRRRWWWWWWWW",
	"GGGGGGGGGGGGWWWWWWWWWWWW",
	"GGGGGGGGGGGGWWWWWWWWWWWW",
	"GGGGGGGGGGGGWWWWWWWWWWWW"
};

static const char *const kFlagEnglish[FLAG_H] = {
	"RRWWBBBBBBWRRWBBBBBBWWRR",
	"WRRWWBBBBBWRRWBBBBBWWRRW",
	"BWWRRWWBBBWRRWBBBWWRRWWB",
	"BBWWRRWWBBWRRWBBWWRRWWBB",
	"BBBBWWRRWWWRRWWWRRWWBBBB",
	"BBBBBWWRRWWRRWWRRWWBBBBB",
	"WWWWWWWWWWWRRWWWWWWWWWWW",
	"RRRRRRRRRRRRRRRRRRRRRRRR",
	"RRRRRRRRRRRRRRRRRRRRRRRR",
	"WWWWWWWWWWWRRWWWWWWWWWWW",
	"BBBBBWWRRWWRRWWRRWWBBBBB",
	"BBBBWWRRWWWRRWWWRRWWBBBB",
	"BBWWRRWWBBWRRWBBWWRRWWBB",
	"BWWRRWWBBBWRRWBBBWWRRWWB",
	"WRRWWBBBBBWRRWBBBBBWWRRW",
	"RRWWBBBBBBWRRWBBBBBBWWRR"
};

static const char *const kFlagFrench[FLAG_H] = {
	"BBBBBBBBWWWWWWWWRRRRRRRR",
	"BBBBBBBBWWWWWWWWRRRRRRRR",
	"BBBBBBBBWWWWWWWWRRRRRRRR",
	"BBBBBBBBWWWWWWWWRRRRRRRR",
	"BBBBBBBBWWWWWWWWRRRRRRRR",
	"BBBBBBBBWWWWWWWWRRRRRRRR",
	"BBBBBBBBWWWWWWWWRRRRRRRR",
	"BBBBBBBBWWWWWWWWRRRRRRRR",
	"BBBBBBBBWWWWWWWWRRRRRRRR",
	"BBBBBBBBWWWWWWWWRRRRRRRR",
	"BBBBBBBBWWWWWWWWRRRRRRRR",
	"BBBBBBBBWWWWWWWWRRRRRRRR",
	"BBBBBBBBWWWWWWWWRRRRRRRR",
	"BBBBBBBBWWWWWWWWRRRRRRRR",
	"BBBBBBBBWWWWWWWWRRRRRRRR",
	"BBBBBBBBWWWWWWWWRRRRRRRR"
};

inline const char *const *GetFlag(Lang lang)
{
	switch (lang)
	{
	case Lang::Arabic: return kFlagArabic;
	case Lang::English: return kFlagEnglish;
	case Lang::French: return kFlagFrench;
	default: return nullptr;
	}
}

// "AR", "ar", "EN", "fr"... -> Lang
inline Lang ParseShortCode(const wchar_t *code)
{
	if (!code || !code[0] || !code[1])
		return Lang::None;

	wchar_t a = code[0], b = code[1];
	if (a >= L'A' && a <= L'Z') a = a - L'A' + L'a';
	if (b >= L'A' && b <= L'Z') b = b - L'A' + L'a';

	if (a == L'a' && b == L'r') return Lang::Arabic;
	if (a == L'e' && b == L'n') return Lang::English;
	if (a == L'f' && b == L'r') return Lang::French;
	return Lang::None;
}

// Uses IInput::GetIMELanguageShortCode when this vgui2 version has it,
// otherwise it compiles to a function that simply reports "unknown".
template <class T>
auto QueryShortCode(T *pInput, wchar_t *buf, int bytes, int)
    -> decltype(pInput->GetIMELanguageShortCode(buf, bytes), bool())
{
	buf[0] = 0;
	pInput->GetIMELanguageShortCode(buf, bytes);
	return true;
}

template <class T>
bool QueryShortCode(T *, wchar_t *buf, int, long)
{
	buf[0] = 0;
	return false;
}

// Primary language id of the active keyboard layout, -1 when not available (needs windows.h).
inline int WindowsLangId()
{
#ifdef CHATLANG_HAS_WINAPI
	return (int)(((uintptr_t)GetKeyboardLayout(0)) & 0x3FF);
#else
	return -1;
#endif
}

inline Lang DetectKeyboardLanguage()
{
	// 1) Windows keyboard layout: 0x01 Arabic, 0x09 English, 0x0C French
	const int win = WindowsLangId();
	if (win >= 0)
	{
		if (win == 0x01) return Lang::Arabic;
		if (win == 0x09) return Lang::English;
		if (win == 0x0C) return Lang::French;
		return Lang::None;
	}

	// 2) vgui2 input method API
	wchar_t buf[16] = { 0 };
	QueryShortCode(vgui2::input(), buf, sizeof(buf), 0);
	return ParseShortCode(buf);
}

} // namespace ChatLang

//-----------------------------------------------------------------------------
// Purpose: draws the flag of the current input language
//-----------------------------------------------------------------------------
class CChatLangFlag : public vgui2::Panel
{
	typedef vgui2::Panel BaseClass;

public:
	CChatLangFlag(vgui2::Panel *parent, const char *panelName)
	    : BaseClass(parent, panelName)
	{
		SetMouseInputEnabled(false);
		SetKeyBoardInputEnabled(false);
		SetPaintBackgroundEnabled(false);
		SetSize(ChatLang::FLAG_W + 2, ChatLang::FLAG_H + 2);
		SetVisible(false);
	}

	void SetLang(ChatLang::Lang lang) { m_Lang = lang; }
	ChatLang::Lang GetLang() const { return m_Lang; }

	virtual void Paint()
	{
		const char *const *flag = ChatLang::GetFlag(m_Lang);
		if (!flag)
			return;

		// dark outline so the flag is visible on any background
		vgui2::surface()->DrawSetColor(0, 0, 0, 200);
		vgui2::surface()->DrawFilledRect(0, 0, ChatLang::FLAG_W + 2, ChatLang::FLAG_H + 2);

		for (int y = 0; y < ChatLang::FLAG_H; y++)
		{
			const char *row = flag[y];
			int x = 0;
			while (x < ChatLang::FLAG_W)
			{
				// draw runs of the same color with one rectangle
				int end = x + 1;
				while (end < ChatLang::FLAG_W && row[end] == row[x])
					end++;

				int r = 255, g = 255, b = 255;
				switch (row[x])
				{
				case 'G': r = 0;   g = 98;  b = 51;  break;
				case 'R': r = 210; g = 16;  b = 52;  break;
				case 'B': r = 1;   g = 33;  b = 105; break;
				default: break;
				}

				vgui2::surface()->DrawSetColor(r, g, b, 255);
				vgui2::surface()->DrawFilledRect(1 + x, 1 + y, 1 + end, 2 + y);
				x = end;
			}
		}
	}

private:
	ChatLang::Lang m_Lang = ChatLang::Lang::None;
};
