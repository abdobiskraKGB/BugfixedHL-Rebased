#include <algorithm>
#include <string>
#include <vgui/IImage.h>
#include <vgui/IScheme.h>
#include <vgui/IInputInternal.h>
#include <vgui/ISurface.h>
#include <vgui/ISystem.h>
#include <vgui/ILocalize.h>
#include <vgui_controls/Label.h>
#include <vgui_controls/SectionedListPanel.h>
#include <vgui_controls/ImageList.h>
#include <vgui_controls/Menu.h>
#include "hud.h"
#include "cl_util.h"
#include "cl_voice_status.h"
#include "client_vgui.h"
#include "client_steam_context.h"
#include "hud/chat.h"
#include "hud/text_message.h"
#include "vgui/client_viewport.h"
#include "vgui/score_panel.h"
#include "vgui/avatar_image.h"

#define STEAM_PROFILE_URL "http://steamcommunity.com/profiles/"

ConVar hud_scoreboard_mousebtn("hud_scoreboard_mousebtn", "1", FCVAR_ARCHIVE);
ConVar hud_scoreboard_showavatars("hud_scoreboard_showavatars", "1", FCVAR_ARCHIVE);
ConVar hud_scoreboard_showloss("hud_scoreboard_showloss", "1", FCVAR_ARCHIVE);
ConVar hud_scoreboard_efftype("hud_scoreboard_efftype", "0", FCVAR_ARCHIVE);
ConVar hud_scoreboard_effpercent("hud_scoreboard_effpercent", "0", FCVAR_ARCHIVE);
ConVar hud_scoreboard_showsteamid("hud_scoreboard_showsteamid", "1", FCVAR_ARCHIVE);
ConVar hud_scoreboard_showeff("hud_scoreboard_showeff", "1", FCVAR_ARCHIVE);
ConVar hud_scoreboard_size("hud_scoreboard_size", "0", FCVAR_ARCHIVE);
ConVar hud_scoreboard_spacing_normal("hud_scoreboard_spacing_normal", "0", FCVAR_ARCHIVE);
ConVar hud_scoreboard_spacing_compact("hud_scoreboard_spacing_compact", "0", FCVAR_ARCHIVE);
ConVar hud_scoreboard_twoteams("hud_scoreboard_twoteams", "1", FCVAR_ARCHIVE, "Show two teams side by side (two lists) when the match has exactly two teams");

namespace
{

int s_iMutedIconTexture = -1;

class CPlayerImage : public vgui2::IImage
{
public:
	void SetAvatar(CAvatarImage *pAvatar)
	{
		m_pAvatar = pAvatar;

		if (m_pAvatar)
		{
			m_pAvatar->SetPos(m_iX, m_iY);
			m_pAvatar->SetOffset(m_iOffX, m_iOffY);
			m_pAvatar->SetSize(m_iWide, m_iTall);
			m_pAvatar->SetColor(m_DrawColor);
		}
	}

	void SetMuted(bool state)
	{
		m_bIsMuted = state;
	}

	// Call to Paint the image
	// Image will draw within the current panel context at the specified position
	virtual void Paint() override
	{
		if (m_pAvatar)
			m_pAvatar->Paint();

		if (m_bIsMuted && s_iMutedIconTexture != -1)
		{
			vgui2::surface()->DrawSetTexture(s_iMutedIconTexture);
			vgui2::surface()->DrawSetColor(m_DrawColor);
			vgui2::surface()->DrawTexturedRect(m_iX + m_iOffX, m_iY + m_iOffY,
			    m_iX + m_iOffX + m_iWide, m_iY + m_iOffY + m_iTall);
		}
	}

	// Set the position of the image
	virtual void SetPos(int x, int y) override
	{
		m_iX = x;
		m_iY = y;
		SetAvatar(m_pAvatar);
	}

	virtual void SetOffset(int x, int y)
	{
		m_iOffX = x;
		m_iOffY = y;
		SetAvatar(m_pAvatar);
	}

	// Gets the size of the content
	virtual void GetContentSize(int &wide, int &tall) override
	{
		wide = m_iWide;
		tall = m_iTall;
	}

	// Get the size the image will actually draw in (usually defaults to the content size)
	virtual void GetSize(int &wide, int &tall) override
	{
		GetContentSize(wide, tall);
	}

	// Sets the size of the image
	virtual void SetSize(int wide, int tall) override
	{
		m_iWide = wide;
		m_iTall = tall;
		SetAvatar(m_pAvatar);
	}

	// Set the draw color
	virtual void SetColor(Color col) override
	{
		m_DrawColor = col;
		SetAvatar(m_pAvatar);
	}

private:
	int m_iX = 0, m_iY = 0;
	int m_iOffX = 0, m_iOffY = 0;
	int m_iWide = 0, m_iTall = 0;
	Color m_DrawColor = Color(255, 255, 255, 255);
	CAvatarImage *m_pAvatar = nullptr;
	bool m_bIsMuted = false;
};

// The colored box behind the list of a team: a banner with the name of the team, the number of
// players and the score (big, on the right), and a frame around the banner and the list.
class CTeamBox : public vgui2::Panel
{
	typedef vgui2::Panel BaseClass;

public:
	CTeamBox(vgui2::Panel *parent, const char *panelName)
	    : BaseClass(parent, panelName)
	{
		SetMouseInputEnabled(false);
		SetKeyBoardInputEnabled(false);
		SetPaintBackgroundEnabled(false);

		m_pName = CreateLabel("BoxName", vgui2::Label::a_west);
		m_pCount = CreateLabel("BoxCount", vgui2::Label::a_west);
		m_pScore = CreateLabel("BoxScore", vgui2::Label::a_east);
	}

	void SetFonts(vgui2::HFont nameFont, vgui2::HFont scoreFont)
	{
		if (nameFont)
		{
			m_pName->SetFont(nameFont);
			m_pCount->SetFont(nameFont);
		}
		if (scoreFont)
			m_pScore->SetFont(scoreFont);
	}

	void SetTeamColor(Color color) { m_Color = color; }

	void SetTexts(const wchar_t *name, const wchar_t *count, const wchar_t *score)
	{
		m_pName->SetText(name);
		m_pCount->SetText(count);
		m_pScore->SetText(score);
	}

	// Places the banner labels. bannerTall = height of the banner, in pixels.
	void SetBanner(int bannerTall)
	{
		m_iBannerTall = bannerTall;

		int wide = 0, tall = 0;
		GetSize(wide, tall);
		const int pad = bannerTall / 5;
		const int scoreWide = wide / 3;
		m_pName->SetBounds(pad, 0, wide - scoreWide - pad, bannerTall / 2 + pad / 2);
		m_pCount->SetBounds(pad, bannerTall / 2, wide - scoreWide - pad, bannerTall / 2);
		m_pScore->SetBounds(wide - scoreWide - pad, 0, scoreWide, bannerTall);
	}

	virtual void Paint() override
	{
		int wide = 0, tall = 0;
		GetSize(wide, tall);

		// banner
		vgui2::surface()->DrawSetColor(m_Color.r(), m_Color.g(), m_Color.b(), 80);
		vgui2::surface()->DrawFilledRect(0, 0, wide, m_iBannerTall);

		// frame around the banner and the list
		vgui2::surface()->DrawSetColor(m_Color.r(), m_Color.g(), m_Color.b(), 200);
		vgui2::surface()->DrawFilledRect(0, 0, wide, 1);
		vgui2::surface()->DrawFilledRect(0, tall - 1, wide, tall);
		vgui2::surface()->DrawFilledRect(0, 0, 1, tall);
		vgui2::surface()->DrawFilledRect(wide - 1, 0, wide, tall);
		vgui2::surface()->DrawFilledRect(0, m_iBannerTall - 1, wide, m_iBannerTall);
	}

private:
	vgui2::Label *CreateLabel(const char *name, vgui2::Label::Alignment align)
	{
		vgui2::Label *label = new vgui2::Label(this, name, "");
		label->SetPaintBackgroundEnabled(false);
		label->SetContentAlignment(align);
		label->SetFgColor(Color(255, 255, 255, 255));
		return label;
	}

	vgui2::Label *m_pName = nullptr;
	vgui2::Label *m_pCount = nullptr;
	vgui2::Label *m_pScore = nullptr;
	Color m_Color = Color(255, 255, 255, 255);
	int m_iBannerTall = 0;
};

CTeamBox *AsBox(vgui2::Panel *panel)
{
	return static_cast<CTeamBox *>(panel);
}

}

CScorePanel::CScorePanel()
    : BaseClass(nullptr, VIEWPORT_PANEL_SCORE)
{
	SetTitle("", true);
	SetCloseButtonVisible(false);
	SetMoveable(false);
	SetSizeable(false);
	SetProportional(true);
	SetKeyBoardInputEnabled(false);
	SetMouseInputEnabled(false);
	SetScheme("ClientScheme");

	// Header labels
	m_pServerNameLabel = new vgui2::Label(this, "ServerName", "A Half-Life Server");
	m_pMapNameLabel = new vgui2::Label(this, "MapName", "Map: crossfire");
	m_pPlayerCountLabel = new vgui2::Label(this, "PlayerCount", "2/32");

	// Player list
	m_pPlayerList = new vgui2::SectionedListPanel(this, "PlayerList");
	m_pPlayerList->SetMouseInputEnabled(true);
	m_pPlayerList->SetVerticalScrollbar(false);

	wchar_t *specTag = g_pVGuiLocalize->Find("#BHL_Scores_PlayerSpec");
	if (specTag)
	{
		Q_WStringToUTF8(specTag, m_szSpectatorTag, sizeof(m_szSpectatorTag));
	}

	// Image list
	m_pImageList = new vgui2::ImageList(true);
	m_pPlayerList->SetImageList(m_pImageList, true);

	// Two-team layout: one list per team (hidden until needed)
	m_pListLeft = CreateTeamList("PlayerListLeft");
	m_pListRight = CreateTeamList("PlayerListRight");

	m_pBoxLeft = new CTeamBox(this, "BoxLeft");
	m_pBoxLeft->SetVisible(false);
	m_pBoxRight = new CTeamBox(this, "BoxRight");
	m_pBoxRight->SetVisible(false);

	for (int i = 1; i <= MAX_PLAYERS; i++)
	{
		CPlayerImage *pImg = new CPlayerImage();
		pImg->SetSize(32, 32);
		m_pImageList->SetImageAtIndex(i, pImg);
	}

	m_iMutedIconTexture = -1;

	CreatePlayerMenu();

	LoadControlSettings(VGUI2_ROOT_DIR "resource/ScorePanel.res");
	SetVisible(false);
}

void CScorePanel::UpdateServerName()
{
	wchar_t wbuf[MAX_SERVERNAME_LENGTH];
	g_pVGuiLocalize->ConvertANSIToUnicode(g_pViewport->GetServerName(), wbuf, sizeof(wbuf));
	m_pServerNameLabel->SetText(wbuf);
}

void CScorePanel::EnableMousePointer(bool bEnable)
{
	if (bEnable && !IsVisible())
		return;

	SetMouseInputEnabled(bEnable);
	SetKeyBoardInputEnabled(false);

	int x = gEngfuncs.GetWindowCenterX();
	int y = gEngfuncs.GetWindowCenterY();
	vgui2::input()->SetCursorPos(x, y);
}

void CScorePanel::UpdateOnPlayerInfo(int client)
{
	if (!IsVisible())
		return;

	RestoreSize();
	UpdateClientInfo(client);
	UpdateScoresAndCounts();
	Resize();
}

void CScorePanel::DeathMsg(int killer, int victim)
{
	if (!GetThisPlayerInfo())
	{
		// Not yet connected
		return;
	}

	if (victim == GetThisPlayerInfo()->GetIndex())
	{
		// if we were the one killed, set the scoreboard to indicate killer
		m_flKillerHighlightStart = gHUD.m_flTime;
		m_iKillerIndex = killer;
	}
}

void CScorePanel::ApplySchemeSettings(vgui2::IScheme *pScheme)
{
	BaseClass::ApplySchemeSettings(pScheme);

	m_pPlayerList->SetBorder(pScheme->GetBorder("FrameBorder"));
	m_pPlayerList->SetPaintBackgroundType(0);

	for (vgui2::SectionedListPanel *teamList : { m_pListLeft, m_pListRight })
	{
		teamList->SetBorder(pScheme->GetBorder("FrameBorder"));
		teamList->SetPaintBackgroundType(0);
		teamList->SetBgColor(Color(0, 0, 0, 84));
	}

	UpdateBannerFonts();

	m_ThisPlayerBgColor = pScheme->GetColor("ThisPlayerBgColor", Color(0, 0, 0, 0));
	m_KillerBgColor = pScheme->GetColor("KillerBgColor", Color(0, 0, 0, 0));

	s_iMutedIconTexture = m_iMutedIconTexture;
}

void CScorePanel::OnThink()
{
	if (m_iKillerIndex != 0 && m_PlayerData[m_iKillerIndex].nItemID != -1)
	{
		// Update bg color
		UpdateClientInfo(m_iKillerIndex);
	}

	if (m_flLastUpdateTime + UPDATE_PERIOD <= gEngfuncs.GetAbsoluteTime())
	{
		UpdateAllClients();
	}
}

void CScorePanel::OnCommand(const char *command)
{
	if (!Q_stricmp(command, "MenuMute"))
	{
		OnPlayerMenuCommand(MenuAction::Mute);
	}
	else if (!Q_stricmp(command, "MenuSteamProfile"))
	{
		OnPlayerMenuCommand(MenuAction::SteamProfile);
	}
	else if (!Q_stricmp(command, "MenuSteamURL"))
	{
		OnPlayerMenuCommand(MenuAction::SteamURL);
	}
	else if (!Q_stricmp(command, "MenuCopyName"))
	{
		OnPlayerMenuCommand(MenuAction::CopyName);
	}
	else if (!Q_stricmp(command, "MenuCopyNameRaw"))
	{
		OnPlayerMenuCommand(MenuAction::CopyNameRaw);
	}
	else if (!Q_stricmp(command, "MenuCopySteamID"))
	{
		OnPlayerMenuCommand(MenuAction::CopySteamID);
	}
	else if (!Q_stricmp(command, "MenuCopySteamID64"))
	{
		OnPlayerMenuCommand(MenuAction::CopySteamID64);
	}
	else
	{
		BaseClass::OnCommand(command);
	}
}

const char *CScorePanel::GetName()
{
	return VIEWPORT_PANEL_SCORE;
}

void CScorePanel::Reset()
{
	if (IsVisible())
	{
		ShowPanel(false);
	}

	m_iKillerIndex = 0;
	m_flKillerHighlightStart = 0;
}

void CScorePanel::ShowPanel(bool state)
{
	if (state == IsVisible())
		return;

	if (state)
	{
		FullUpdate();

		// Make active unless chat is visible
		// (possible if scoreboard is activated by intermission when chat is open)
		if (CHudChat::Get()->GetMessageMode() == MM_NONE)
			Activate();
	}
	else
	{
		SetMouseInputEnabled(false);
		SetKeyBoardInputEnabled(false);
		m_pPlayerMenu->SetVisible(false);
	}

	SetVisible(state);
}

vgui2::VPANEL CScorePanel::GetVPanel()
{
	return BaseClass::GetVPanel();
}

bool CScorePanel::IsVisible()
{
	return BaseClass::IsVisible();
}

void CScorePanel::SetParent(vgui2::VPANEL parent)
{
	BaseClass::SetParent(parent);
}

void CScorePanel::OnItemContextMenu(int itemID)
{
	OpenPlayerMenu(itemID);
}

void CScorePanel::FullUpdate()
{
	UpdateServerName();
	UpdateMapName();
	RefreshItems();
}

void CScorePanel::UpdateMapName()
{
	char buf[64];
	wchar_t wbuf[64];
	V_FileBase(gEngfuncs.pfnGetLevelName(), buf, sizeof(buf));
	g_pVGuiLocalize->ConvertANSIToUnicode(buf, wbuf, sizeof(wbuf));
	m_pMapNameLabel->SetText(wbuf);
}

void CScorePanel::RefreshItems()
{
	std::fill(m_TeamData.begin(), m_TeamData.end(), TeamData());
	std::fill(m_PlayerData.begin(), m_PlayerData.end(), PlayerData());
	std::fill(m_IsTeamSectionCreated.begin(), m_IsTeamSectionCreated.end(), false);
	m_pPlayerList->RemoveAll();
	m_pPlayerList->RemoveAllSections();
	m_pListLeft->RemoveAll();
	m_pListLeft->RemoveAllSections();
	m_pListRight->RemoveAll();
	m_pListRight->RemoveAllSections();

	RestoreSize();

	// Assign player teams, calculate team scores
	for (int i = 1; i <= MAX_PLAYERS; i++)
	{
		CPlayerInfo *pi = GetPlayerInfo(i)->Update();
		PlayerData &pd = m_PlayerData[i];
		pd.bIsConnected = pi->IsConnected();

		if (!pd.bIsConnected)
			continue;

		pd.nItemID = -1;
		pd.nTeamID = GetPlayerTeam(pi);

		TeamData &td = m_TeamData[pd.nTeamID];
		td.iFrags += pi->GetFrags();
		td.iDeaths += pi->GetDeaths();
		td.iPlayerCount++;
	}

	// Override team scores and add them to sorting list
	int iTeamCount = 0;

	for (int i = 0; i <= MAX_TEAMS; i++)
	{
		TeamData &td = m_TeamData[i];

		if (td.iPlayerCount == 0)
			continue;

		CTeamInfo *ti = GetTeamInfo(i);

		if (ti->IsScoreOverriden())
		{
			td.iFrags = ti->GetFrags();
			td.iDeaths = ti->GetDeaths();
		}

		m_SortedTeamIDs[iTeamCount] = i;
		iTeamCount++;
	}

	// Sort teams based on the score
	std::sort(m_SortedTeamIDs.begin(), m_SortedTeamIDs.begin() + iTeamCount, [&](int ilhs, int irhs) {
		const TeamData &lhs = m_TeamData[ilhs];
		const TeamData &rhs = m_TeamData[irhs];

		// Compare kills
		if (lhs.iFrags > rhs.iFrags)
			return true;
		else if (lhs.iFrags < rhs.iFrags)
			return false;

		// Compare deaths if kills are equal
		if (lhs.iDeaths < rhs.iDeaths)
			return true;
		else if (lhs.iDeaths > rhs.iDeaths)
			return false;

		// Compare idx if everything is equal
		return ilhs > irhs;
	});

	// Exactly two real teams: show them side by side. The team with the lower number is on the left,
	// so the sides never swap while the score changes.
	m_bTwoTeamMode = false;
	if (hud_scoreboard_twoteams.GetBool() && iTeamCount == 2 && m_SortedTeamIDs[0] != 0 && m_SortedTeamIDs[1] != 0)
	{
		m_bTwoTeamMode = true;
		m_iLeftTeam = (m_SortedTeamIDs[0] < m_SortedTeamIDs[1]) ? m_SortedTeamIDs[0] : m_SortedTeamIDs[1];
		m_iRightTeam = (m_SortedTeamIDs[0] < m_SortedTeamIDs[1]) ? m_SortedTeamIDs[1] : m_SortedTeamIDs[0];
	}
	SetLayoutMode(m_bTwoTeamMode);

	// Create header before any other sections
	CreateSection(HEADER_SECTION_ID);

	// Create sections for teams
	if (iTeamCount != 1 || m_SortedTeamIDs[0] != 0)
	{
		for (int i = 0; i < iTeamCount; i++)
		{
			CreateSection(m_SortedTeamIDs[i]);
		}
	}

	// Create last section for spectators
	if (m_TeamData[TEAM_SPECTATOR].iPlayerCount > 0)
	{
		CreateSection(TEAM_SPECTATOR);
	}

	UpdateAllClients();
}

void CScorePanel::CreateSection(int nTeamID)
{
	if (m_IsTeamSectionCreated[nTeamID])
		return;

	m_IsTeamSectionCreated[nTeamID] = true;

	if (m_bTwoTeamMode && nTeamID == HEADER_SECTION_ID)
	{
		// Not needed: in the two-team layout the first row of each team shows the column titles
		return;
	}

	CreateSectionIn(ListForTeam(nTeamID), nTeamID);
}

void CScorePanel::CreateSectionIn(vgui2::SectionedListPanel *list, int nTeamID)
{
	// true for the two lists of the two-team layout (compact columns)
	const bool two = (list != m_pPlayerList);

	// The row with the column titles: the header section in the normal layout,
	// the first row of each team in the two-team layout.
	const bool titleRow = (nTeamID == HEADER_SECTION_ID) || (two && nTeamID != TEAM_SPECTATOR);

	list->AddSection(nTeamID, "", StaticPlayerSortFuncByFrags);
	list->SetSectionFgColor(nTeamID, g_pViewport->GetTeamColor(nTeamID));

	if (nTeamID == HEADER_SECTION_ID)
	{
		list->SetSectionAlwaysVisible(nTeamID);
		list->SetSectionDividerColor(nTeamID, Color(0, 0, 0, 0));
	}
	else if (two && titleRow)
	{
		list->SetSectionAlwaysVisible(nTeamID);
	}

	// Avatar
	list->AddColumnToSection(nTeamID, "avatar", "",
	    vgui2::SectionedListPanel::COLUMN_IMAGE | vgui2::SectionedListPanel::COLUMN_CENTER,
	    two ? m_iTwoColAvatar : m_iColumnWidthAvatar);

	// Name
	const char *nameCol;

	if (titleRow)
		nameCol = "#PlayerName";
	else
		nameCol = "??? (?/?)";

	list->AddColumnToSection(nTeamID, "name", nameCol,
	    vgui2::SectionedListPanel::COLUMN_BRIGHT | (gHUD.GetColorCodeAction() == ColorCodeAction::Handle ? vgui2::SectionedListPanel::COLUMN_COLORED : 0),
	    two ? m_iTwoNameWide : GetNameColumnWidth());

	// SteamID
	if (two ? m_bTwoShowSteam : hud_scoreboard_showsteamid.GetBool())
	{
		list->AddColumnToSection(nTeamID, "steamid", titleRow ? "#BHL_Scores_ColSteamID" : "",
		    vgui2::SectionedListPanel::COLUMN_BRIGHT,
		    two ? m_iTwoColSteamID : m_iColumnWidthSteamID);
	}

	// Efficiency
	if (two ? m_bTwoShowEff : hud_scoreboard_showeff.GetBool())
	{
		list->AddColumnToSection(nTeamID, "eff", titleRow ? "#BHL_Scores_ColEff" : "???",
		    vgui2::SectionedListPanel::COLUMN_BRIGHT,
		    two ? m_iTwoColEff : m_iColumnWidthEff);
	}

	// Frags
	list->AddColumnToSection(nTeamID, "frags", titleRow ? "#PlayerScore" : "???",
	    vgui2::SectionedListPanel::COLUMN_BRIGHT,
	    two ? m_iTwoColFrags : m_iColumnWidthFrags);

	// Deaths
	list->AddColumnToSection(nTeamID, "deaths", titleRow ? "#PlayerDeath" : "???",
	    vgui2::SectionedListPanel::COLUMN_BRIGHT,
	    two ? m_iTwoColDeaths : m_iColumnWidthDeaths);

	// Ping
	const char *pingLabel;

	if (titleRow)
		pingLabel = hud_scoreboard_showloss.GetBool() ? "#BHL_Scores_ColPingLoss" : "#BHL_Scores_ColPing";
	else
		pingLabel = "";

	list->AddColumnToSection(nTeamID, "ping", pingLabel,
	    vgui2::SectionedListPanel::COLUMN_BRIGHT,
	    two ? m_iTwoColPing : m_iColumnWidthPing);
}

void CScorePanel::UpdateAllClients()
{
	for (int i = 1; i <= MAX_PLAYERS; i++)
	{
		UpdateClientInfo(i);
	}

	UpdateScoresAndCounts();
	Resize();

	m_flLastUpdateTime = gEngfuncs.GetAbsoluteTime();
}

void CScorePanel::UpdateClientInfo(int client)
{
	CPlayerInfo *pi = GetPlayerInfo(client);
	PlayerData &pd = m_PlayerData[client];

	if (pi->IsConnected() && !pd.bIsConnected)
	{
		// Player just connected
		pd.bIsConnected = true;

		if (pd.nItemID != -1 && ListForTeam(pd.nTeamID)->IsItemIDValid(pd.nItemID))
			ListForTeam(pd.nTeamID)->RemoveItem(pd.nItemID);

		pd.nItemID = -1;
		pd.nTeamID = pi->GetTeamNumber();
	}
	else if (!pi->IsConnected() && pd.bIsConnected)
	{
		// Player disconnected
		pd.bIsConnected = false;

		ListForTeam(pd.nTeamID)->RemoveItem(pd.nItemID);
		pd.nItemID = -1;
		pd.nTeamID = 0;
	}

	Assert(pd.bIsConnected == pi->IsConnected());

	// Skip unconnected players
	if (!pi->IsConnected())
		return;

	if (GetPlayerTeam(pi) != pd.nTeamID)
	{
		// Player changed team
		ListForTeam(pd.nTeamID)->RemoveItem(pd.nItemID);
		pd.nItemID = -1;
		pd.nTeamID = pi->GetTeamNumber();
	}

	// Create section for player's team if need to
	CreateSection(pd.nTeamID);

	KeyValues *playerKv = new KeyValues("data");

	{
		char buf[128];
		playerKv->SetInt("client", client);

		if (pi->IsThisPlayer())
			playerKv->SetBool("thisplayer", true);

		// Avatar
		UpdateClientIcon(pi);
		playerKv->SetInt("avatar", client); // Client index == index into m_pImageList

		// Name
		if (pi->IsSpectator())
		{
			snprintf(buf, sizeof(buf), "%s %s%s",
			    pi->GetDisplayName(gHUD.GetColorCodeAction() == ColorCodeAction::Strip),
			    gHUD.GetColorCodeAction() == ColorCodeAction::Handle ? "^0" : "",
			    m_szSpectatorTag);
		}
		else
		{
			snprintf(buf, sizeof(buf), "%s", pi->GetDisplayName(gHUD.GetColorCodeAction() == ColorCodeAction::Strip));
		}
		playerKv->SetString("name", buf);

		// SteamID
		playerKv->SetString("steamid", pi->GetSteamID());

		// Efficiency
		float eff = CalculateEfficiency(pi->GetFrags(), pi->GetDeaths());
		if (hud_scoreboard_effpercent.GetBool())
			snprintf(buf, sizeof(buf), "%.0f%%", eff * 100.0);
		else
			snprintf(buf, sizeof(buf), "%.2f", eff);
		playerKv->SetString("eff", buf);

		// Frags & deaths
		playerKv->SetInt("frags", pi->GetFrags());
		playerKv->SetInt("deaths", pi->GetDeaths());

		// Ping
		if (hud_scoreboard_showloss.GetBool())
		{
			snprintf(buf, sizeof(buf), "%d/%d", pi->GetPing(), pi->GetPacketLoss());
			playerKv->SetString("ping", buf);
		}
		else
		{
			playerKv->SetInt("ping", pi->GetPing());
		}
	}

	vgui2::SectionedListPanel *list = ListForTeam(pd.nTeamID);

	if (pd.nItemID == -1)
	{
		// Create player's row
		pd.nItemID = list->AddItem(pd.nTeamID, playerKv);
		list->SetItemFgColor(pd.nItemID, gHUD.GetClientColor(client, NoTeamColor::White));
		list->InvalidateLayout();
	}
	else
	{
		// Update player's row
		list->ModifyItem(pd.nItemID, pd.nTeamID, playerKv);
	}

	list->SetItemBgColor(pd.nItemID, GetPlayerBgColor(pi));

	playerKv->deleteThis();
}

void CScorePanel::UpdateClientIcon(CPlayerInfo *pi)
{
	CPlayerImage *pImg = static_cast<CPlayerImage *>(m_pImageList->GetImage(pi->GetIndex()));

	// Update size
	int size = GetClientIconSize();
	pImg->SetSize(size, size);

	// Update muted state
	pImg->SetMuted(GetClientVoiceMgr()->IsPlayerBlocked(pi->GetIndex()));

	// Update avatar
	uint64 steamID64 = pi->GetValidSteamID64();
	if (hud_scoreboard_showavatars.GetBool() && ClientSteamContext().SteamFriends() && ClientSteamContext().SteamUtils() && steamID64 != 0)
	{
		CSteamID steamIDForPlayer(steamID64);
		auto it = m_PlayerAvatars.find(steamIDForPlayer);

		if (it == m_PlayerAvatars.end())
		{
			CAvatarImage *pAvatar = new CAvatarImage();
			pAvatar->SetDrawFriend(false);
			pAvatar->SetAvatarSteamID(steamIDForPlayer);

			pImg->SetAvatar(pAvatar);
			m_PlayerAvatars.insert({ steamIDForPlayer, pAvatar });
		}
		else
		{
			pImg->SetAvatar(it->second);
		}
	}
	else
	{
		pImg->SetAvatar(nullptr);
	}
}

void CScorePanel::UpdateScoresAndCounts()
{
	// Reset team data
	for (int i = 1; i <= MAX_TEAMS; i++)
	{
		TeamData &td = m_TeamData[i];
		td.iPlayerCount = 0;
		td.iFrags = 0;
		td.iDeaths = 0;
	}

	// Refresh scores
	int iPlayerCount = 0;

	for (int i = 1; i <= MAX_PLAYERS; i++)
	{
		CPlayerInfo *pi = GetPlayerInfo(i);

		if (!pi->IsConnected())
			continue;

		TeamData &td = m_TeamData[pi->GetTeamNumber()];
		td.iFrags += pi->GetFrags();
		td.iDeaths += pi->GetDeaths();
		
		if (GetPlayerTeam(pi) != TEAM_SPECTATOR)
			td.iPlayerCount++;

		iPlayerCount++;
	}

	char buf[128];
	wchar_t wbuf[128];

	auto fnUpdateTeamHeader = [&](const char *pszTeamName, int nTeamID) {
		TeamData &td = m_TeamData[nTeamID];

		// Team name and player count
		wchar_t wbuf2[128];
		wchar_t *localizedName = nullptr;
		if (pszTeamName[0] == L'#' && (localizedName = g_pVGuiLocalize->Find(pszTeamName)))
		{
			// localizedName set to localized name
		}
		else
		{
			// set localizedName to pszTeamName converted to WString
			g_pVGuiLocalize->ConvertANSIToUnicode(pszTeamName, wbuf2, sizeof(wbuf2));
			localizedName = wbuf2;
		}

		if (m_bTwoTeamMode && nTeamID != TEAM_SPECTATOR)
		{
			// In the two-team layout the totals of the team are shown in the banner above its list
			UpdateTeamBanner(nTeamID, localizedName, td.iPlayerCount, iPlayerCount, td.iFrags);
			return;
		}

		V_snwprintf(wbuf, 128, L"%ls (%d/%d)", localizedName, td.iPlayerCount, iPlayerCount);
		ListForTeam(nTeamID)->ModifyColumn(nTeamID, "name", wbuf);

		// Team efficiency
		float eff = CalculateEfficiency(td.iFrags, td.iDeaths);
		if (hud_scoreboard_effpercent.GetBool())
			snprintf(buf, sizeof(buf), "%.0f%%", eff * 100.0);
		else
			snprintf(buf, sizeof(buf), "%.2f", eff);
		g_pVGuiLocalize->ConvertANSIToUnicode(buf, wbuf, sizeof(wbuf));
		ListForTeam(nTeamID)->ModifyColumn(nTeamID, "eff", wbuf);

		// Team frags
		snprintf(buf, sizeof(buf), "%d", td.iFrags);
		g_pVGuiLocalize->ConvertANSIToUnicode(buf, wbuf, sizeof(wbuf));
		ListForTeam(nTeamID)->ModifyColumn(nTeamID, "frags", wbuf);

		// Team deaths
		snprintf(buf, sizeof(buf), "%d", td.iDeaths);
		g_pVGuiLocalize->ConvertANSIToUnicode(buf, wbuf, sizeof(wbuf));
		ListForTeam(nTeamID)->ModifyColumn(nTeamID, "deaths", wbuf);
	};

	// Update team score and player count
	for (int i = 1; i <= MAX_TEAMS; i++)
	{
		TeamData &td = m_TeamData[i];

		if (td.iPlayerCount == 0)
			continue;

		CTeamInfo *ti = GetTeamInfo(i);

		if (ti->IsScoreOverriden())
		{
			td.iFrags = ti->GetFrags();
			td.iDeaths = ti->GetDeaths();
		}

		fnUpdateTeamHeader(ti->GetDisplayName(), i);
	}

	// Update spectator team
	fnUpdateTeamHeader("#Spectators", TEAM_SPECTATOR);

	// Update total player count
	snprintf(buf, sizeof(buf), "%d/%d", iPlayerCount, gEngfuncs.GetMaxClients());
	m_pPlayerCountLabel->SetText(buf);
}

int CScorePanel::GetNameColumnWidth()
{
	int w = m_iColumnWidthName;

	if (!hud_scoreboard_showsteamid.GetBool())
		w += m_iColumnWidthSteamID;

	if (!hud_scoreboard_showeff.GetBool())
		w += m_iColumnWidthEff;

	return w;
}

int CScorePanel::GetPlayerTeam(CPlayerInfo *pi)
{
	if (pi->GetTeamName()[0] == '\0' && g_pViewport->GetNumberOfNonEmptyTeamPlayers() > 0)
		return TEAM_SPECTATOR;
	else
		return pi->GetTeamNumber();
}

Color CScorePanel::GetPlayerBgColor(CPlayerInfo *pi)
{
	if (pi->IsThisPlayer())
	{
		return m_ThisPlayerBgColor;
	}
	else if (m_iKillerIndex == pi->GetIndex())
	{
		Color color = m_KillerBgColor;
		float t = m_flKillerHighlightStart;
		float dt = HIGHLIGHT_KILLER_TIME;
		float k = -color.a() / dt;
		float b = -k * (t + dt);
		float a = k * gHUD.m_flTime + b;
		if (a > color.a() || a <= 0)
		{
			m_iKillerIndex = 0;
			m_flKillerHighlightStart = 0;
			return Color(0, 0, 0, 0);
		}
		else
		{
			color[3] = (int)a;
			return color;
		}
	}

	return Color(0, 0, 0, 0);
}

float CScorePanel::CalculateEfficiency(int kills, int deaths)
{
	int type = clamp(hud_scoreboard_efftype.GetInt(), 0, 2);

	switch (type)
	{
	case 0:
	{
		// K / D
		if (deaths == 0)
			deaths = 1;
		return (float)kills / deaths;
	}
	case 1:
	{
		// K / (D + 1)
		if (deaths == -1)
			deaths = 0;
		return (float)kills / (deaths + 1);
	}
	case 2:
	{
		// K / (K + D)
		if (kills + deaths == 0)
			return 1.0f;
		return (float)kills / (kills + deaths);
	}
	}

	return 0;
}

int CScorePanel::GetClientIconSize()
{
	return clamp(ActiveList()->GetLineSpacing() - 2, 0, 32);
}

void CScorePanel::CreatePlayerMenu()
{
	m_pPlayerMenu = new vgui2::Menu(this, "PlayerMenu");
	m_pPlayerMenu->SetVisible(false);
	m_pPlayerMenu->AddActionSignalTarget(this);

	m_MenuData.nMuteItemID = m_pPlayerMenu->AddMenuItem("Mute", "#BHL_Scores_MenuMute", "MenuMute", this);
	m_pPlayerMenu->AddSeparator();

	m_MenuData.nProfilePageItemID = m_pPlayerMenu->AddMenuItem("SteamProfile", "#BHL_Scores_MenuSteamProfile", "MenuSteamProfile", this);
	m_MenuData.nProfileUrlItemID = m_pPlayerMenu->AddMenuItem("SteamURL", "#BHL_Scores_MenuSteamURL", "MenuSteamURL", this);
	m_pPlayerMenu->AddSeparator();

	m_pPlayerMenu->AddMenuItem("CopyName", "#BHL_Scores_MenuCopyName", "MenuCopyName", this);
	m_pPlayerMenu->AddMenuItem("CopyNameRaw", "#BHL_Scores_MenuCopyNameRaw", "MenuCopyNameRaw", this);
	m_pPlayerMenu->AddMenuItem("CopySteamID", "#BHL_Scores_MenuCopySteamID", "MenuCopySteamID", this);
	m_pPlayerMenu->AddMenuItem("CopySteamID64", "#BHL_Scores_MenuCopySteamID64", "MenuCopySteamID64", this);
}

void CScorePanel::OpenPlayerMenu(int itemID)
{
	// Set menu info
	m_MenuData.nItemID = itemID;
	m_MenuData.nClient = 0;
	vgui2::SectionedListPanel *menuList = m_pPlayerList;
	if (m_bTwoTeamMode)
		menuList = m_pListRight->IsCursorOver() ? m_pListRight : m_pListLeft; // the list that was clicked
	KeyValues *kv = menuList->GetItemData(itemID);
	if (!kv)
		return;
	m_MenuData.nClient = kv->GetInt("client", 0);
	if (m_MenuData.nClient == 0)
		return;

	// SteamID64
	m_MenuData.nSteamID64 = GetPlayerInfo(m_MenuData.nClient)->GetValidSteamID64();
	if (m_MenuData.nSteamID64 != 0)
	{
		m_pPlayerMenu->SetItemEnabled(m_MenuData.nProfilePageItemID, true);
		m_pPlayerMenu->SetItemEnabled(m_MenuData.nProfileUrlItemID, true);
	}
	else
	{
		m_pPlayerMenu->SetItemEnabled(m_MenuData.nProfilePageItemID, false);
		m_pPlayerMenu->SetItemEnabled(m_MenuData.nProfileUrlItemID, false);
	}

	// Player muting
	bool isMuted = GetClientVoiceMgr()->IsPlayerBlocked(m_MenuData.nClient);
	bool thisPlayer = kv->GetBool("thisplayer", 0);
	if (thisPlayer && !isMuted)
	{
		// Can't mute yourself
		m_pPlayerMenu->UpdateMenuItem(m_MenuData.nMuteItemID, "#BHL_Scores_MenuMute", new KeyValues("Command", "command", "MenuMute"));
		m_pPlayerMenu->SetItemEnabled(m_MenuData.nMuteItemID, false);
	}
	else
	{
		m_pPlayerMenu->SetItemEnabled(m_MenuData.nMuteItemID, true);
		if (isMuted)
		{
			m_pPlayerMenu->UpdateMenuItem(m_MenuData.nMuteItemID, "#BHL_Scores_MenuUnmute", new KeyValues("Command", "command", "MenuMute"));
		}
		else
		{
			m_pPlayerMenu->UpdateMenuItem(m_MenuData.nMuteItemID, "#BHL_Scores_MenuMute", new KeyValues("Command", "command", "MenuMute"));
		}
	}

	m_pPlayerMenu->PositionRelativeToPanel(this, vgui2::Menu::CURSOR, 0, true);
}

void CScorePanel::OnPlayerMenuCommand(MenuAction command)
{
	CPlayerInfo *pi = GetPlayerInfo(m_MenuData.nClient);

	if (!pi->IsConnected())
		return;

	switch (command)
	{
	case MenuAction::Mute:
	{
		if (GetClientVoiceMgr()->IsPlayerBlocked(pi->GetIndex()))
		{
			// Unmute
			GetClientVoiceMgr()->SetPlayerBlockedState(pi->GetIndex(), false);

			char string1[1024];
			snprintf(string1, sizeof(string1), CHudTextMessage::BufferedLocaliseTextString("#Unmuted"), pi->GetDisplayName(true));
			CHudChat::Get()->ChatPrintf(0, "** %s", string1);
		}
		else if (!pi->IsThisPlayer())
		{
			// Mute
			GetClientVoiceMgr()->SetPlayerBlockedState(pi->GetIndex(), true);

			char string1[1024];
			char string2[1024];
			snprintf(string1, sizeof(string1), CHudTextMessage::BufferedLocaliseTextString("#Muted"), pi->GetDisplayName(true));
			snprintf(string2, sizeof(string2), "%s", CHudTextMessage::BufferedLocaliseTextString("#No_longer_hear_that_player"));
			CHudChat::Get()->ChatPrintf(0, "** %s", string1);
			CHudChat::Get()->ChatPrintf(0, "** %s", string2);
		}

		// Muting one player may mute others (if they have identical Unique IDs)
		UpdateAllClients();

		break;
	}
	case MenuAction::SteamProfile:
	{
		if (ClientSteamContext().SteamFriends())
		{
			// Open in overlay
			CSteamID steamId = CSteamID((uint64)m_MenuData.nSteamID64);
			ClientSteamContext().SteamFriends()->ActivateGameOverlayToUser("steamid", steamId);
		}
		else
		{
			// Open in browser
			std::string url = STEAM_PROFILE_URL + std::to_string(m_MenuData.nSteamID64);
			vgui2::system()->ShellExecute("open", url.c_str());
		}

		break;
	}
	case MenuAction::SteamURL:
	{
		std::string url = STEAM_PROFILE_URL + std::to_string(m_MenuData.nSteamID64);
		vgui2::system()->SetClipboardText(url.c_str(), url.size());
		break;
	}
	case MenuAction::CopyName:
	{
		wchar_t name[MAX_PLAYER_NAME + 1];
		g_pVGuiLocalize->ConvertANSIToUnicode(pi->GetDisplayName(true), name, sizeof(name));
		vgui2::system()->SetClipboardText(name, wcslen(name));
		break;
	}
	case MenuAction::CopyNameRaw:
	{
		wchar_t name[MAX_PLAYER_NAME + 1];
		g_pVGuiLocalize->ConvertANSIToUnicode(pi->GetName(), name, sizeof(name));
		vgui2::system()->SetClipboardText(name, wcslen(name));
		break;
	}
	case MenuAction::CopySteamID:
	{
		std::string steamid = "STEAM_" + std::string(pi->GetSteamID());
		vgui2::system()->SetClipboardText(steamid.c_str(), steamid.size());
		break;
	}
	case MenuAction::CopySteamID64:
	{
		std::string steamid = std::to_string(m_MenuData.nSteamID64);
		vgui2::system()->SetClipboardText(steamid.c_str(), steamid.size());
		break;
	}
	}
}

CScorePanel::SizeMode CScorePanel::GetSizeMode()
{
	return (SizeMode)clamp(hud_scoreboard_size.GetInt(), 0, 2);
}

int CScorePanel::GetLineSpacingForHeight(int h)
{
	if (h < 600)
		return 18; // (0; 600)
	if (h < 720)
		return 22; // [600; 720]
	if (h < 800)
		return 24; // [720; 800)
	if (h < 1024)
		return 26; // [800; 1024)
	if (h < 1080)
		return 28; // [1024; 1080)

	return 28; // >= 1080
}

int CScorePanel::GetLineSpacingForNormal()
{
	if (hud_scoreboard_spacing_normal.GetInt() > 0)
		return hud_scoreboard_spacing_normal.GetInt();
	return 0;
}

int CScorePanel::GetLineSpacingForCompact()
{
	if (hud_scoreboard_spacing_compact.GetInt() > 0)
		return hud_scoreboard_spacing_compact.GetInt();
	return GetLineSpacingForHeight(ScreenHeight);
}

void CScorePanel::RestoreSize()
{
	if (GetSizeMode() == SizeMode::Compact)
		SetLineSpacingAll(GetLineSpacingForCompact());
	else
		SetLineSpacingAll(GetLineSpacingForNormal());
}

void CScorePanel::Resize()
{
	if (m_bTwoTeamMode)
	{
		ResizeTwoTeams();
		return;
	}

	SizeMode mode = GetSizeMode();

	// Returns true if scrollbar was enabled
	auto fnUpdateSize = [&](int &height) {
		int wide, tall, x, y;
		int listHeight = 0, addHeight = 0;
		bool bIsOverflowed = false;
		height = 0;

		m_pPlayerList->GetPos(x, y);
		addHeight = y + m_iBottomPadding; // Distance on the top and on the bottom of the player list
		height += addHeight;
		m_pPlayerList->GetContentSize(wide, tall);
		listHeight = max(m_iMinHeight, tall);
		height += listHeight;

		if (ScreenHeight - height < m_iVerticalMargin * 2)
		{
			// It didn't fit
			height = ScreenHeight - m_iVerticalMargin * 2;
			listHeight = height - addHeight;
			m_pPlayerList->SetVerticalScrollbar(true);
			bIsOverflowed = true;
		}
		else
		{
			m_pPlayerList->SetVerticalScrollbar(false);
		}

		m_pPlayerList->GetSize(wide, tall);
		m_pPlayerList->SetSize(wide, listHeight);

		return bIsOverflowed;
	};

	int wide, tall, x, y;
	int height;
	if (fnUpdateSize(height) && mode == SizeMode::Auto)
	{
		// Content overflowed, scrollbar is now visible. Set comapct line spacing
		m_pPlayerList->SetLineSpacingOverride(GetLineSpacingForCompact());

		// Refresh player info to update avatar sizes
		for (int i = 1; i <= MAX_PLAYERS; i++)
		{
			UpdateClientInfo(i);
		}

		// Resize again
		fnUpdateSize(height);
	}

	GetSize(wide, tall);
	SetSize(wide, height);

	// Move to center
	GetPos(x, y);
	y = (ScreenHeight - height) / 2;
	SetPos(x, y);
}

vgui2::SectionedListPanel *CScorePanel::CreateTeamList(const char *name)
{
	vgui2::SectionedListPanel *list = new vgui2::SectionedListPanel(this, name);
	list->SetMouseInputEnabled(true);
	list->SetVerticalScrollbar(false);
	list->SetImageList(m_pImageList, false); // the image list is shared and owned by the first list
	list->AddActionSignalTarget(this);
	list->SetVisible(false);
	return list;
}

vgui2::SectionedListPanel *CScorePanel::ListForTeam(int nTeamID)
{
	if (!m_bTwoTeamMode)
		return m_pPlayerList;

	// spectators are listed under the left team
	if (nTeamID == m_iLeftTeam || nTeamID == TEAM_SPECTATOR)
		return m_pListLeft;

	return m_pListRight;
}

vgui2::SectionedListPanel *CScorePanel::ActiveList()
{
	return m_bTwoTeamMode ? m_pListLeft : m_pPlayerList;
}

void CScorePanel::SetLayoutMode(bool twoTeams)
{
	m_pPlayerList->SetVisible(!twoTeams);
	m_pListLeft->SetVisible(twoTeams);
	m_pListRight->SetVisible(twoTeams);
	m_pBoxLeft->SetVisible(twoTeams);
	m_pBoxRight->SetVisible(twoTeams);

	if (twoTeams)
	{
		GetTwoLayout();
		UpdateBannerFonts();
	}

	if (twoTeams && !m_bLayoutChanged)
	{
		// Remember the normal layout, to restore it when the match has no longer two teams
		int dummy = 0;
		GetPos(m_iNormalPanelX, dummy);
		GetSize(m_iNormalPanelWide, dummy);
		m_pPlayerCountLabel->GetPos(m_iPlayerCountX, m_iPlayerCountY);
		m_bLayoutChanged = true;
	}
	else if (!twoTeams && m_bLayoutChanged)
	{
		int x = 0, y = 0, wide = 0, tall = 0;
		GetPos(x, y);
		GetSize(wide, tall);
		SetPos(m_iNormalPanelX, y);
		SetSize(m_iNormalPanelWide, tall);
		m_pPlayerCountLabel->SetPos(m_iPlayerCountX, m_iPlayerCountY);
		m_bLayoutChanged = false;
	}
}

void CScorePanel::SetLineSpacingAll(int spacing)
{
	m_pPlayerList->SetLineSpacingOverride(spacing);
	m_pListLeft->SetLineSpacingOverride(spacing);
	m_pListRight->SetLineSpacingOverride(spacing);
}

bool CScorePanel::FitTwoLists(int &height)
{
	int x = 0, y = 0, wide = 0;
	m_pPlayerList->GetPos(x, y); // the banners start where the normal list starts
	const int listY = y + m_iTwoBannerTall + m_iTwoBannerGap;
	const int addHeight = listY + m_iBottomPadding; // distance on the top and on the bottom of the lists

	int leftTall = 0, rightTall = 0;
	m_pListLeft->GetContentSize(wide, leftTall);
	m_pListRight->GetContentSize(wide, rightTall);

	int listHeight = (leftTall > rightTall) ? leftTall : rightTall;
	if (listHeight < m_iMinHeight)
		listHeight = m_iMinHeight;
	height = addHeight + listHeight;

	bool overflowed = false;
	if (ScreenHeight - height < m_iVerticalMargin * 2)
	{
		// It didn't fit
		height = ScreenHeight - m_iVerticalMargin * 2;
		listHeight = height - addHeight;
		overflowed = true;
	}

	m_pListLeft->SetVerticalScrollbar(overflowed);
	m_pListRight->SetVerticalScrollbar(overflowed);

	const int leftX = m_iTwoListGap;
	const int rightX = m_iTwoListGap + m_iTwoListWideReal + m_iTwoListGap;
	m_pListLeft->SetBounds(leftX, listY, m_iTwoListWideReal, listHeight);
	m_pListRight->SetBounds(rightX, listY, m_iTwoListWideReal, listHeight);

	// banner + frame: from the top of the banner to the bottom of the list
	const int boxTall = m_iTwoBannerTall + m_iTwoBannerGap + listHeight;
	m_pBoxLeft->SetBounds(leftX, y, m_iTwoListWideReal, boxTall);
	m_pBoxRight->SetBounds(rightX, y, m_iTwoListWideReal, boxTall);
	AsBox(m_pBoxLeft)->SetBanner(m_iTwoBannerTall);
	AsBox(m_pBoxRight)->SetBanner(m_iTwoBannerTall);

	return overflowed;
}

void CScorePanel::ResizeTwoTeams()
{
	int height = 0;
	if (FitTwoLists(height) && GetSizeMode() == SizeMode::Auto)
	{
		// Content overflowed, scrollbars are now visible. Set compact line spacing
		SetLineSpacingAll(GetLineSpacingForCompact());

		// Refresh player info to update avatar sizes
		for (int i = 1; i <= MAX_PLAYERS; i++)
		{
			UpdateClientInfo(i);
		}

		// Resize again
		FitTwoLists(height);
	}

	SetSize(m_iTwoPanelWideReal, height);

	// Move to center
	int screenWide = 0, screenTall = 0;
	vgui2::surface()->GetScreenSize(screenWide, screenTall);
	SetPos((screenWide - m_iTwoPanelWideReal) / 2, (ScreenHeight - height) / 2);

	// Player count in the top right corner
	int labelWide = 0, labelTall = 0;
	m_pPlayerCountLabel->GetSize(labelWide, labelTall);
	m_pPlayerCountLabel->SetPos(m_iTwoPanelWideReal - m_iTwoListGap - labelWide, m_iPlayerCountY);
}

void CScorePanel::GetTwoLayout()
{
	int screenWide = 0, screenTall = 0;
	vgui2::surface()->GetScreenSize(screenWide, screenTall);

	// the panel never gets wider than the screen
	int panelWide = m_iTwoPanelWide;
	if (panelWide > screenWide - 16)
		panelWide = screenWide - 16;

	const int gap = m_iTwoListGap; // between the lists and on both sides
	const int listWide = (panelWide - 3 * gap) / 2;

	m_bTwoShowSteam = hud_scoreboard_showsteamid.GetBool();
	m_bTwoShowEff = hud_scoreboard_showeff.GetBool();

	// the name column takes the rest. When it is too small, the SteamID and then the Eff column are dropped.
	const int fixed = m_iTwoColAvatar + m_iTwoColFrags + m_iTwoColDeaths + m_iTwoColPing;
	int nameWide = listWide - fixed
	    - (m_bTwoShowSteam ? m_iTwoColSteamID : 0)
	    - (m_bTwoShowEff ? m_iTwoColEff : 0);

	if (nameWide < m_iTwoMinName && m_bTwoShowSteam)
	{
		m_bTwoShowSteam = false;
		nameWide += m_iTwoColSteamID;
	}
	if (nameWide < m_iTwoMinName && m_bTwoShowEff)
	{
		m_bTwoShowEff = false;
		nameWide += m_iTwoColEff;
	}

	m_iTwoListWideReal = listWide;
	m_iTwoPanelWideReal = 2 * listWide + 3 * gap;
	m_iTwoNameWide = nameWide;
}

void CScorePanel::UpdateBannerFonts()
{
	// ChatFont for the names. The big score uses "ScoreBig": add it to the Fonts of ChatScheme.res
	// (see the instructions). If it does not exist, the name font is used.
	vgui2::IScheme *pChatScheme = vgui2::scheme()->GetIScheme(vgui2::scheme()->GetScheme("ChatScheme"));
	m_hBannerNameFont = pChatScheme ? pChatScheme->GetFont("ChatFont") : 0;
	m_hBannerScoreFont = pChatScheme ? pChatScheme->GetFont("ScoreBig") : 0;
	if (!m_hBannerScoreFont)
		m_hBannerScoreFont = m_hBannerNameFont;

	AsBox(m_pBoxLeft)->SetFonts(m_hBannerNameFont, m_hBannerScoreFont);
	AsBox(m_pBoxRight)->SetFonts(m_hBannerNameFont, m_hBannerScoreFont);
}

void CScorePanel::UpdateTeamBanner(int nTeamID, const wchar_t *name, int teamPlayers, int totalPlayers, int score)
{
	CTeamBox *box = AsBox(nTeamID == m_iLeftTeam ? m_pBoxLeft : m_pBoxRight);

	wchar_t count[32];
	wchar_t scoreText[16];
	V_snwprintf(count, 32, L"%d / %d", teamPlayers, totalPlayers);
	V_snwprintf(scoreText, 16, L"%d", score);

	box->SetTeamColor(g_pViewport->GetTeamColor(nTeamID));
	box->SetTexts(name, count, scoreText);
}

bool CScorePanel::StaticPlayerSortFuncByFrags(vgui2::SectionedListPanel *list, int itemID1, int itemID2)
{
	KeyValues *it1 = list->GetItemData(itemID1);
	KeyValues *it2 = list->GetItemData(itemID2);
	Assert(it1 && it2);

	// first compare frags
	int v1 = it1->GetInt("frags");
	int v2 = it2->GetInt("frags");
	if (v1 > v2)
		return true;
	else if (v1 < v2)
		return false;

	// next compare deaths
	v1 = it1->GetInt("deaths");
	v2 = it2->GetInt("deaths");
	if (v1 > v2)
		return false;
	else if (v1 < v2)
		return true;

	// the same, so compare itemID's (as a sentinel value to get deterministic sorts)
	return itemID1 < itemID2;
}
