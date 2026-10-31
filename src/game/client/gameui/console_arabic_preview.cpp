#include <algorithm>
#include <cstdio>
#include <string>
#include <IEngineVGui.h>
#include <KeyValues.h>
#include <convar.h>
#include <tier1/strtools.h>
#include <tier2/tier2.h>
#include <vgui/IPanel.h>
#include <vgui/IVGui.h>
#include <vgui/IScheme.h>
#include <vgui/ISurface.h>
#include <vgui_controls/Label.h>
#include <vgui_controls/Panel.h>
#include "hud.h"
#include "cl_util.h"
#include "console.h"
#include "console_arabic_preview.h"
#include "console_arabic_text.h"

static ConVar con_arabic_preview("con_arabic_preview", "1", FCVAR_BHL_ARCHIVE,
	"Show Arabic text typed in the console correctly joined and ordered (0 = off)");
static ConVar con_arabic_preview_overlay("con_arabic_preview_overlay", "1", FCVAR_BHL_ARCHIVE,
	"1 = draw the Arabic text over the console input line (like the chat), 0 = show it in a bar above the input");
static ConVar con_arabic_preview_bg("con_arabic_preview_bg", "", FCVAR_BHL_ARCHIVE,
	"Background color of the overlay as \"R G B A\" (empty = use the console input color)");
static ConVar con_arabic_preview_fg("con_arabic_preview_fg", "", FCVAR_BHL_ARCHIVE,
	"Text color of the overlay as \"R G B A\" (empty = use the console input color)");

namespace
{
constexpr int POLL_INTERVAL_MS = 50;
constexpr int MAX_TREE_DEPTH = 12;
constexpr int MAX_TREE_PANELS = 512;
constexpr size_t MAX_QUERY_CHARS = 4096;
// GameUI answers "GetText" with GetText(wbuf, 255 BYTES) = at most ~125 characters (verified in the
// shipped GameUI.dll). A text this long may be cut, so the real input line must stay visible.
constexpr size_t LEGACY_QUERY_LIMIT = 120;

bool SameName(const char *a, const char *b)
{
	return a && V_stricmp(a, b) == 0;
}

bool ParseOverlayColor(const char *text, Color &out)
{
	int r, g, b, a = 255;
	if (!text || sscanf(text, "%d %d %d %d", &r, &g, &b, &a) < 3)
		return false;
	out = Color(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255), std::clamp(a, 0, 255));
	return true;
}

bool IsConsolePanel(vgui2::VPANEL panel)
{
	const char *name = g_pVGuiPanel->GetName(panel);
	const char *type = g_pVGuiPanel->GetClassName(panel);
	return SameName(name, "Console") || SameName(name, "GameConsole") ||
		SameName(name, "ConsoleDialog") || SameName(name, "GameConsoleDialog") ||
		SameName(type, "CGameConsoleDialog");
}

// Walk only live panels returned by IPanel, never a guessed C++ object layout.
// No foreign panel pointers or VPANELs are retained between ticks.
vgui2::VPANEL FindConsole(vgui2::VPANEL panel, int depth, int &budget)
{
	if (!panel || depth > MAX_TREE_DEPTH || budget-- <= 0 || !g_pVGuiPanel->IsVisible(panel))
		return 0;
	if (IsConsolePanel(panel))
		return panel;
	const int count = g_pVGuiPanel->GetChildCount(panel);
	for (int i = 0; i < count && budget > 0; ++i)
	{
		if (auto found = FindConsole(g_pVGuiPanel->GetChild(panel, i), depth + 1, budget))
			return found;
	}
	return 0;
}

struct EntrySearch
{
	vgui2::VPANEL panel = 0;
	int score = 0;
	bool ambiguous = false;
};

// The input line of the shipped GameUI is a "TabCatchingTextEntry" named "ConsoleEntry"
void FindEntry(vgui2::VPANEL panel, int depth, int &budget, EntrySearch &best)
{
	if (!panel || depth > MAX_TREE_DEPTH || budget-- <= 0 || !g_pVGuiPanel->IsVisible(panel))
		return;
	const char *name = g_pVGuiPanel->GetName(panel);
	const char *type = g_pVGuiPanel->GetClassName(panel);
	int score = 0;
	if (g_pVGuiPanel->IsKeyBoardInputEnabled(panel) && g_pVGuiPanel->IsEnabled(panel))
	{
		if (SameName(name, "ConsoleEntry") || SameName(name, "ConsoleTextEntry") ||
			SameName(name, "CommandEntry"))
			score = 100;
		else if (SameName(type, "TextEntry") || SameName(type, "CConsoleEntry") ||
			SameName(type, "CGameConsoleEntry"))
			score = 50;
	}
	if (score > best.score)
		best = { panel, score, false };
	else if (score && score == best.score)
		best.ambiguous = true;
	const int count = g_pVGuiPanel->GetChildCount(panel);
	for (int i = 0; i < count && budget > 0; ++i)
		FindEntry(g_pVGuiPanel->GetChild(panel, i), depth + 1, budget, best);
}

class CConsoleArabicPreview : public vgui2::Panel
{
	DECLARE_CLASS_SIMPLE(CConsoleArabicPreview, vgui2::Panel);

public:
	explicit CConsoleArabicPreview(vgui2::Panel *owner)
		: BaseClass(owner, "ConsoleArabicPreviewController")
	{
		SetSize(0, 0);
		SetPaintEnabled(false);
		SetPaintBackgroundEnabled(false);
		SetMouseInputEnabled(false);
		SetKeyBoardInputEnabled(false);
		vgui2::ivgui()->AddTickSignal(GetVPanel(), POLL_INTERVAL_MS);
	}

	~CConsoleArabicPreview() override
	{
		vgui2::ivgui()->RemoveTickSignal(GetVPanel());
		// The label belongs to a foreign parent. A DHANDLE detects if GameUI
		// already destroyed it, and explicit deletion prevents callbacks into
		// an unloaded client DLL if the console survives client shutdown.
		if (m_preview.Get())
			delete m_preview.Get();
		m_preview = nullptr;
	}

	void OnTick() override { Refresh(false); }

	void PrintStatus()
	{
		Refresh(true);
		ConPrintf("Arabic console preview: %s\n", m_status.c_str());
		ConPrintf("Enabled: %d, overlay: %d. Reads the input through IPanel::RequestInfo(GetText); no input hooks.\n",
			con_arabic_preview.GetBool() ? 1 : 0, con_arabic_preview_overlay.GetBool() ? 1 : 0);
		ConPrintf("This GameUI returns at most ~%d characters; longer lines are left to the native input.\n",
			(int)LEGACY_QUERY_LIMIT);
		ConPrintf("If the entry is not found, capture vgui_dumptree while the console is open.\n");
	}

private:
	void Hide(const char *status)
	{
		m_status = status;
		if (m_preview.Get())
			m_preview->SetVisible(false);
	}

	vgui2::HFont GetPreviewFont(vgui2::VPANEL console)
	{
		// Reuse the font that already renders this mod's Arabic chat.
		auto chat = vgui2::scheme()->GetScheme("ChatScheme");
		if (chat)
		{
			if (auto scheme = vgui2::scheme()->GetIScheme(chat))
			{
				if (auto font = scheme->GetFont("ChatFont", false))
					return font;
			}
		}
		if (auto scheme = vgui2::scheme()->GetIScheme(g_pVGuiPanel->GetScheme(console)))
			return scheme->GetFont("Default", false);
		return 0;
	}

	// The console input uses the old GameUI scheme (WindowBgColor / WindowFgColor)
	void GetPreviewColors(vgui2::VPANEL entry, Color &fg, Color &bg)
	{
		fg = Color(235, 245, 220, 255);
		bg = Color(28, 34, 26, 255);
		if (auto scheme = vgui2::scheme()->GetIScheme(g_pVGuiPanel->GetScheme(entry)))
		{
			fg = scheme->GetColor("WindowFgColor", fg);
			bg = scheme->GetColor("WindowBgColor", bg);
		}
		ParseOverlayColor(con_arabic_preview_fg.GetString(), fg);
		ParseOverlayColor(con_arabic_preview_bg.GetString(), bg);
		bg[3] = 255; // the native text underneath must not shine through
	}

	void Refresh(bool diagnostic)
	{
		if (!g_pVGuiPanel || !g_pEngineVGui || !vgui2::surface())
		{
			Hide("VGUI interfaces unavailable");
			return;
		}
		if (!con_arabic_preview.GetBool() && !diagnostic)
		{
			Hide("disabled");
			return;
		}
		int budget = MAX_TREE_PANELS;
		auto console = FindConsole(g_pEngineVGui->GetPanel(PANEL_ROOT), 0, budget);
		if (!console)
		{
			Hide("no visible console panel found");
			return;
		}
		EntrySearch entry;
		budget = MAX_TREE_PANELS;
		FindEntry(console, 0, budget, entry);
		if (!entry.panel || entry.ambiguous)
		{
			Hide(entry.ambiguous ? "multiple input candidates; preview disabled" : "console input panel not found");
			return;
		}

		// GoldSrc KeyValues has virtual methods/deleteThis for crossing module
		// boundaries. KeyValuesAD owns the query; never free the text pointer
		// returned by GetWString and never cast the entry to TextEntry*.
		static const wchar_t s_wszMissing[] = L"\x01<none>";
		KeyValuesAD query("GetText");
		if (!g_pVGuiPanel->RequestInfo(entry.panel, query))
		{
			Hide("this GameUI does not answer GetText; no unsafe fallback attempted");
			return;
		}
		const wchar_t *text = query->GetWString("text", s_wszMissing);
		if (text == s_wszMissing || !text)
		{
			Hide("this GameUI does not provide the text as Unicode; no unsafe fallback attempted");
			return;
		}
		size_t length = 0;
		while (length < MAX_QUERY_CHARS && text[length])
			++length;
		if (length == MAX_QUERY_CHARS)
		{
			Hide("text query exceeded the preview size limit");
			return;
		}
		std::wstring body(text, length);
		if (!ConsoleArabicText::MakePreview(body).containsArabic || !con_arabic_preview.GetBool())
		{
			Hide("Unicode text query OK; waiting for Arabic input (or preview disabled)");
			return;
		}
		const bool possiblePrefix = length >= LEGACY_QUERY_LIMIT;
		const bool overlay = con_arabic_preview_overlay.GetBool();
		if (overlay && possiblePrefix)
		{
			// Covering the input with a truncated text would hide part of the command
			Hide("line is longer than GameUI can report; showing the native input");
			return;
		}
		const auto font = GetPreviewFont(console);
		if (!font)
		{
			Hide("no preview font available");
			return;
		}

		int cx, cy, cw, ch, ex, ey, ew, eh, left, top, right, bottom;
		g_pVGuiPanel->GetAbsPos(console, cx, cy);
		g_pVGuiPanel->GetSize(console, cw, ch);
		g_pVGuiPanel->GetInset(console, left, top, right, bottom);
		g_pVGuiPanel->GetAbsPos(entry.panel, ex, ey);
		g_pVGuiPanel->GetSize(entry.panel, ew, eh);
		const int height = overlay ? eh : vgui2::surface()->GetFontTall(font) + 8;
		// Child coordinates are relative to the parent's inset client area.
		const int x = ex - cx - left;
		const int y = ey - cy - top - (overlay ? 0 : height + 3);
		if (x < 0 || y < 0 || ew <= 8 || height <= 0 ||
			x + ew > cw - left - right || y + height > ch - top - bottom)
		{
			Hide("not enough room for the preview");
			return;
		}

		// Visual text; in overlay mode it carries its own blinking caret at the end of the input
		const bool caretOn = (static_cast<int>(gEngfuncs.GetAbsoluteTime() * 2.0) % 2) == 0;
		const int available = ew - 8 - (overlay ? 0 : 60);
		auto measure = [font](const std::wstring &str) -> int {
			int total = 0;
			for (wchar_t ch2 : str)
			{
				int a, b, c;
				vgui2::surface()->GetCharABCwide(font, ch2, a, b, c);
				total += a + b + c;
			}
			return total;
		};
		std::wstring shown;
		std::wstring logical = body;
		for (;;)
		{
			shown = overlay ? ConsoleArabicText::MakePreviewWithCaret(logical, caretOn).visual
			                : ConsoleArabicText::MakePreview(logical).visual;
			// Keep the newest characters visible when the text is wider than the box
			if (logical.size() <= 1 || measure(shown) <= available)
				break;
			logical.erase(0, 1);
		}
		if (!overlay)
			shown = std::wstring(possiblePrefix ? L"RTL [partial]: " : L"RTL: ") + shown;

		Color fg, bg;
		GetPreviewColors(entry.panel, fg, bg);

		if (!m_preview.Get())
		{
			m_preview = new vgui2::Label(nullptr, "ConsoleArabicPreviewLabel", L"");
			m_preview->SetMouseInputEnabled(false);
			m_preview->SetKeyBoardInputEnabled(false);
			m_preview->SetPaintBackgroundEnabled(true);
			m_preview->SetPaintBorderEnabled(false);
			m_preview->SetContentAlignment(vgui2::Label::a_west);
			m_preview->SetTextInset(4, 0);
			m_preview->SetZPos(1000);
		}
		if (g_pVGuiPanel->GetParent(m_preview->GetVPanel()) != console)
			m_preview->SetParent(console);
		m_preview->SetFont(font);
		m_preview->SetFgColor(fg);
		m_preview->SetBgColor(bg);
		m_preview->SetBounds(x, y, ew, height);
		m_preview->SetText(shown.c_str());
		m_preview->SetVisible(true);
		m_preview->MoveToFront();
		m_status = possiblePrefix ? "active; text may be a legacy-query prefix" : "active; Unicode query OK";
	}

	vgui2::DHANDLE<vgui2::Label> m_preview;
	std::string m_status = "waiting for console";
};

vgui2::DHANDLE<CConsoleArabicPreview> s_controller;
}

void ConsoleArabicPreview_Init(vgui2::Panel *owner)
{
	if (owner && !s_controller.Get())
		s_controller = new CConsoleArabicPreview(owner);
}

void ConsoleArabicPreview_Shutdown()
{
	if (s_controller.Get())
		delete s_controller.Get();
	s_controller = nullptr;
}

CON_COMMAND(con_arabic_preview_status, "Check console Arabic preview support without printing typed text")
{
	if (s_controller.Get())
		s_controller->PrintStatus();
	else
		ConPrintf("Arabic console preview is not initialized. Load a map with this client first.\n");
}
