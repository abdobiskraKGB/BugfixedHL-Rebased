// Center print (print_center) drawn with the chat font: UTF-8 text (Arabic names!) is shown correctly,
// and the message is removed when a map or a server changes.
//
// The engine's own center print only knows an 8-bit font and keeps showing text through server changes,
// spectator mode and intermission.

#include <string>
#include <vector>
#include <tier1/strtools.h>
#include <vgui/ISurface.h>
#include <vgui_controls/Controls.h>
#include <vgui_controls/Panel.h>
#include <convar.h>

#include "hud.h"
#include "cl_util.h"
#include "hud/arabic_text.h"
#include "vgui/client_viewport.h"

static ConVar hud_centerprint_vgui("hud_centerprint_vgui", "1", FCVAR_BHL_ARCHIVE, "Draw print_center messages with the chat font (UTF-8 and Arabic work); 0 = engine's own center print");

namespace
{
constexpr int MAX_LINES = 8;
constexpr float MIN_TIME = 1.5f;

vgui2::HFont GetCenterFont()
{
	// the font that already draws the Arabic chat
	auto chat = vgui2::scheme()->GetScheme("ChatScheme");
	if (chat)
	{
		if (auto scheme = vgui2::scheme()->GetIScheme(chat))
		{
			if (auto font = scheme->GetFont("ChatFont", false))
				return font;
		}
	}

	return 0;
}

class CCenterPrintPanel : public vgui2::Panel
{
	DECLARE_CLASS_SIMPLE(CCenterPrintPanel, vgui2::Panel);

public:
	explicit CCenterPrintPanel(vgui2::Panel *parent)
	    : BaseClass(parent, "CenterPrintPanel")
	{
		SetPaintBackgroundEnabled(false);
		SetMouseInputEnabled(false);
		SetKeyBoardInputEnabled(false);
		SetPos(0, 0);
		FitToScreen();
		SetVisible(true);
	}

	void Show(const char *text)
	{
		m_Lines.clear();

		std::string line;
		for (const char *c = text; ; ++c)
		{
			if (*c == '\n' || *c == 0)
			{
				// shaped and reordered, like every other Arabic text of the client
				const std::string visual = ArabicText::ToVisualUtf8(line);
				wchar_t wide[512];
				V_UTF8ToUnicode(visual.c_str(), wide, sizeof(wide));
				m_Lines.push_back(wide);
				line.clear();

				if (*c == 0 || (int)m_Lines.size() >= MAX_LINES)
					break;
			}
			else
			{
				line += *c;
			}
		}

		float seconds = gEngfuncs.pfnGetCvarFloat("scr_centertime");
		if (seconds < MIN_TIME)
			seconds = MIN_TIME;
		m_flEnd = gEngfuncs.GetAbsoluteTime() + seconds;
	}

	void Clear()
	{
		m_Lines.clear();
		m_flEnd = 0;
	}

	void PerformLayout() override
	{
		FitToScreen();
	}

	void Paint() override
	{
		if (m_Lines.empty())
			return;

		if (gEngfuncs.GetAbsoluteTime() > m_flEnd)
		{
			m_Lines.clear();
			return;
		}

		const vgui2::HFont font = GetCenterFont();
		if (!font)
			return;

		int sw, sh;
		GetSize(sw, sh);
		const int lineH = vgui2::surface()->GetFontTall(font) + 2;
		int y = (int)(sh * 0.30f) - (int)m_Lines.size() * lineH / 2;

		vgui2::surface()->DrawSetTextFont(font);
		for (const std::wstring &line : m_Lines)
		{
			int tw = 0, th = 0;
			vgui2::surface()->GetTextSize(font, line.c_str(), tw, th);
			const int x = (sw - tw) / 2;

			// shadow, then the text
			vgui2::surface()->DrawSetTextColor(0, 0, 0, 255);
			vgui2::surface()->DrawSetTextPos(x + 1, y + 1);
			vgui2::surface()->DrawPrintText(line.c_str(), (int)line.size());
			vgui2::surface()->DrawSetTextColor(255, 255, 255, 255);
			vgui2::surface()->DrawSetTextPos(x, y);
			vgui2::surface()->DrawPrintText(line.c_str(), (int)line.size());

			y += lineH;
		}
	}

private:
	void FitToScreen()
	{
		int w, h;
		vgui2::surface()->GetScreenSize(w, h);
		SetSize(w, h);
	}

	std::vector<std::wstring> m_Lines;
	float m_flEnd = 0;
};

vgui2::DHANDLE<CCenterPrintPanel> s_hPanel;
}

void CenterPrint_Show(const char *text)
{
	if (!text)
		return;

	if (!hud_centerprint_vgui.GetBool() || !g_pViewport || !vgui2::surface())
	{
		gEngfuncs.pfnCenterPrint(text);
		return;
	}

	if (!s_hPanel.Get())
		s_hPanel = new CCenterPrintPanel(g_pViewport);

	s_hPanel->Show(text);
}

// New map or server: nothing of the old messages may stay on the screen
void CenterPrint_Clear()
{
	if (s_hPanel.Get())
		s_hPanel->Clear();
}
