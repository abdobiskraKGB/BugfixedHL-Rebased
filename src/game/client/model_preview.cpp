// model_preview.cpp
// In-game player model selection with a rotating 3D preview.
//
// How it works:
//  - Console commands (modelmenu, modelmenu_next, modelmenu_prev, modelmenu_apply) change the state here.
//  - Every frame V_CalcRefdef calls modelpreview::Apply(). While the preview is open, an entity owned by
//    this file (not the weapon viewmodel) is given the selected player model, placed in front of the
//    camera and handed to the engine with CL_CreateVisibleEntity. It does not depend on the weapon, so
//    it also works while dead or spectating, and weapon animations cannot disturb it.
//  - GameStudioModelRenderer draws these entities with a reduced depth range (like the viewmodel), so
//    walls never hide them.
//  - The scoreboard asks for small team models through ShowScoreboardModel().

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <FileSystem.h>
#include <tier1/strtools.h>
#include <vgui/ISurface.h>
#include <vgui_controls/Button.h>
#include <vgui_controls/Controls.h>
#include <vgui_controls/Frame.h>

#include "hud.h"
#include "cl_util.h"
#include "const.h"
#include "com_model.h"
#include "studio.h"
#include "cl_entity.h"
#include "ref_params.h"
#include "r_studioint.h"

#include "model_preview.h"
#include "vgui/client_viewport.h"
#include "common/entity_types.h"

extern engine_studio_api_t IEngineStudio;

// ---------------------------------------------------------------------------
// Settings (tweakable in-game, so no recompiling is needed to calibrate the position)
// ---------------------------------------------------------------------------
static ConVar modelmenu_models("modelmenu_models", "", FCVAR_BHL_ARCHIVE,
    "Explicit list of models for the preview, e.g. \"blue;red\" (team mods). Empty = scan models/player/");
static ConVar modelmenu_dist("modelmenu_dist", "110", FCVAR_BHL_ARCHIVE, "Preview distance from the camera");
static ConVar modelmenu_right("modelmenu_right", "35", FCVAR_BHL_ARCHIVE, "Preview offset to the right (negative = left)");
static ConVar modelmenu_up("modelmenu_up", "-30", FCVAR_BHL_ARCHIVE, "Preview vertical offset (negative = lower)");
static ConVar modelmenu_panel("modelmenu_panel", "1", FCVAR_BHL_ARCHIVE, "Show the clickable model list next to the 3D preview (0 = preview only, use modelmenu_next/prev)");
static ConVar modelmenu_close_on_apply("modelmenu_close_on_apply", "0", FCVAR_BHL_ARCHIVE, "Close the menu after a model was chosen with the mouse");
static ConVar modelmenu_spin("modelmenu_spin", "45", FCVAR_BHL_ARCHIVE, "Preview rotation speed in degrees/second (0 = still)");

namespace
{
bool s_bOpen = false;
bool s_bAnimDirty = true;
bool s_bMenuShown = false;
float s_flAnimStart = 0;
int s_Index = 0;
int s_iSequence = 0;
model_t *s_pModel = nullptr;
std::vector<std::string> s_Models;

void ModelPath(const char *name, char *out, size_t size)
{
	snprintf(out, size, "models/player/%s/%s.mdl", name, name);
}

void AddModelIfValid(const char *name)
{
	if (!name || !name[0])
		return;

	for (const std::string &m : s_Models)
	{
		if (!Q_stricmp(m.c_str(), name))
			return;
	}

	char path[256];
	ModelPath(name, path, sizeof(path));

	if (!g_pFullFileSystem->FileExists(path))
		return;

	s_Models.emplace_back(name);
}

void BuildList()
{
	s_Models.clear();

	const char *forced = modelmenu_models.GetString();
	if (forced && forced[0])
	{
		// Explicit list (team mods): "blue;red"
		std::string list = forced;
		size_t pos = 0;
		while (pos <= list.size())
		{
			size_t end = list.find(';', pos);
			if (end == std::string::npos)
				end = list.size();
			AddModelIfValid(list.substr(pos, end - pos).c_str());
			pos = end + 1;
		}
		return;
	}

	// Scan models/player/<name>/<name>.mdl
	FileFindHandle_t handle;
	for (const char *f = g_pFullFileSystem->FindFirst("models/player/*", &handle); f; f = g_pFullFileSystem->FindNext(handle))
	{
		if (f[0] == '.')
			continue;
		if (!g_pFullFileSystem->FindIsDirectory(handle))
			continue;
		AddModelIfValid(f);
	}
	g_pFullFileSystem->FindClose(handle);

	std::sort(s_Models.begin(), s_Models.end(), [](const std::string &a, const std::string &b)
	    { return Q_stricmp(a.c_str(), b.c_str()) < 0; });
}

int FindCurrentModelIndex()
{
	cl_entity_t *lp = gEngfuncs.GetLocalPlayer();
	if (!lp)
		return 0;

	const char *cur = GetPlayerInfo(lp->index)->Update()->GetModel();
	if (!cur)
		return 0;

	for (size_t i = 0; i < s_Models.size(); i++)
	{
		if (!Q_stricmp(s_Models[i].c_str(), cur))
			return (int)i;
	}
	return 0;
}

//! Finds a standing idle sequence in the model.
int FindIdleSequence(model_t *model)
{
	if (!model)
		return 0;

	studiohdr_t *hdr = (studiohdr_t *)IEngineStudio.Mod_Extradata(model);
	if (!hdr)
		return 0;

	mstudioseqdesc_t *seqs = (mstudioseqdesc_t *)((byte *)hdr + hdr->seqindex);

	for (int i = 0; i < hdr->numseq; i++)
	{
		if (!Q_stricmp(seqs[i].label, "idle1"))
			return i;
	}

	for (int i = 0; i < hdr->numseq; i++)
	{
		if (strstr(seqs[i].label, "idle") && !strstr(seqs[i].label, "crouch"))
			return i;
	}

	return 0;
}

void LoadPreviewModel()
{
	s_pModel = nullptr;

	if (s_Models.empty())
		return;

	char path[256];
	ModelPath(s_Models[s_Index].c_str(), path, sizeof(path));

	s_pModel = IEngineStudio.Mod_ForName(path, 0);
	s_iSequence = FindIdleSequence(s_pModel);
	s_bAnimDirty = true;
}

// Entities handed to the engine every frame (static: the engine keeps the pointers until it rendered)
cl_entity_t s_MenuEnt;
cl_entity_t s_TeamEnt[2];

struct TeamRequest
{
	std::string name;
	model_t *model = nullptr;
	int sequence = 0;
	float animStart = 0;
	float lastRequest = -100;
	int x = 0, y = 0, size = 0;
};

TeamRequest s_Team[2];

bool IsSafeModelName(const char *name)
{
	if (!name || !name[0] || strlen(name) > 31)
		return false;

	for (const char *c = name; *c; ++c)
	{
		const char ch = *c;
		if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-'))
			return false;
	}

	return true;
}

model_t *LoadPlayerModel(const char *name, int &sequence)
{
	if (!IsSafeModelName(name))
		return nullptr;

	char path[256];
	ModelPath(name, path, sizeof(path));

	if (!g_pFullFileSystem->FileExists(path))
		return nullptr;

	model_t *model = IEngineStudio.Mod_ForName(path, 0);
	sequence = FindIdleSequence(model);
	return model;
}

// Fills a fresh entity: neutral pose (controllers/blends in the middle of their range) and an animation
// clock that only we control.
void FillEntity(cl_entity_t &e, model_t *model, const float *origin, float yaw, int sequence, float animStart)
{
	memset(&e, 0, sizeof(e));

	e.model = model;
	e.curstate.sequence = sequence;
	e.curstate.framerate = 1;
	e.curstate.animtime = animStart;
	e.curstate.frame = 0;
	e.curstate.scale = 1;
	e.curstate.rendermode = kRenderNormal;
	e.curstate.renderamt = 255;
	e.latched.prevsequence = sequence;

	for (int i = 0; i < 3; i++)
	{
		e.origin[i] = e.curstate.origin[i] = e.latched.prevorigin[i] = origin[i];
		e.angles[i] = e.curstate.angles[i] = e.latched.prevangles[i] = (i == 1) ? yaw : 0.0f;
	}

	for (int i = 0; i < 4; i++)
		e.curstate.controller[i] = e.latched.prevcontroller[i] = 127;
	for (int i = 0; i < 2; i++)
		e.curstate.blending[i] = e.latched.prevblending[i] = 127;
}

void ShowCurrent()
{
	if (s_Models.empty())
		return;

	char buf[128];
	snprintf(buf, sizeof(buf), "%s  [%d/%d]", s_Models[s_Index].c_str(), s_Index + 1, (int)s_Models.size());
	gEngfuncs.pfnCenterPrint(buf);
}

void PreviewIndex(int index)
{
	if (index < 0 || index >= (int)s_Models.size() || index == s_Index)
		return;

	s_Index = index;
	LoadPreviewModel();
}

void ApplyIndex(int index)
{
	if (index < 0 || index >= (int)s_Models.size())
		return;

	char cmd[128];
	snprintf(cmd, sizeof(cmd), "model %s", s_Models[index].c_str());
	EngineClientCmd(cmd);
	ConPrintf("Model changed to: %s\n", s_Models[index].c_str());
}

class CModelMenu;

//! One name in the list: hovering previews the model, clicking applies it.
class CModelButton : public vgui2::Button
{
	DECLARE_CLASS_SIMPLE(CModelButton, vgui2::Button);

public:
	CModelButton(vgui2::Panel *parent, const char *name, const char *text, int index)
	    : BaseClass(parent, name, text, nullptr, nullptr)
	    , m_iIndex(index)
	{
	}

	void OnCursorEntered() override
	{
		BaseClass::OnCursorEntered();
		PreviewIndex(m_iIndex);
	}

	void DoClick() override;

	int m_iIndex;
};

//! Left side list of the model names. The 3D model itself is drawn by Apply() on the right side.
class CModelMenu : public vgui2::Frame
{
	DECLARE_CLASS_SIMPLE(CModelMenu, vgui2::Frame);

public:
	CModelMenu()
	    : BaseClass(nullptr, "ModelMenuPanel")
	{
		SetTitle("Player model", false);
		SetMoveable(false);
		SetSizeable(false);
		SetCloseButtonVisible(true);
		SetProportional(false);
		SetVisible(false);

		m_pPrev = new vgui2::Button(this, "ModelMenuPrev", "<", this, "PagePrev");
		m_pNext = new vgui2::Button(this, "ModelMenuNext", ">", this, "PageNext");
	}

	void Open()
	{
		int sw, sh;
		vgui2::surface()->GetScreenSize(sw, sh);
		// Left third of the screen; the model is shown on the right
		SetBounds(sw / 40, sh / 8, std::max(220, sw * 3 / 10), sh * 3 / 4);

		m_iPage = std::max(0, s_Index / std::max(1, PerPage()));
		Rebuild();
		SetVisible(true);
		MoveToFront();
	}

	void Rebuild()
	{
		for (CModelButton *b : m_Buttons)
			b->SetVisible(false);

		const int count = (int)s_Models.size();
		const int perPage = PerPage();
		const int pages = std::max(1, (count + perPage - 1) / perPage);
		m_iPage = std::min(std::max(m_iPage, 0), pages - 1);

		const int width = GetWide();
		const int rowH = RowHeight();
		const int first = m_iPage * perPage;

		for (int slot = 0; slot < perPage && first + slot < count; slot++)
		{
			// Buttons are created once and reused between pages
			while ((int)m_Buttons.size() <= slot)
			{
				char name[32];
				snprintf(name, sizeof(name), "ModelButton_%d", (int)m_Buttons.size());
				m_Buttons.push_back(new CModelButton(this, name, "", 0));
			}

			CModelButton *button = m_Buttons[slot];
			button->m_iIndex = first + slot;
			button->SetText(s_Models[first + slot].c_str());
			button->SetBounds(12, 34 + slot * (rowH + 3), width - 24, rowH);
			button->SetContentAlignment(vgui2::Label::a_west);
			button->SetVisible(true);
		}

		const int footerY = GetTall() - 34;
		m_pPrev->SetBounds(12, footerY, 40, 24);
		m_pNext->SetBounds(width - 52, footerY, 40, 24);
		m_pPrev->SetEnabled(m_iPage > 0);
		m_pNext->SetEnabled(m_iPage + 1 < pages);

		char title[96];
		snprintf(title, sizeof(title), "Player model  (%d/%d)", m_iPage + 1, pages);
		SetTitle(title, false);
	}

	void OnCommand(const char *command) override
	{
		if (!V_stricmp(command, "PagePrev"))
		{
			m_iPage--;
			Rebuild();
		}
		else if (!V_stricmp(command, "PageNext"))
		{
			m_iPage++;
			Rebuild();
		}
		else
		{
			BaseClass::OnCommand(command);
		}
	}

	void OnMouseWheeled(int delta) override
	{
		m_iPage -= delta > 0 ? 1 : -1;
		Rebuild();
	}

private:
	int RowHeight()
	{
		int sw, sh;
		vgui2::surface()->GetScreenSize(sw, sh);
		return std::max(22, sh / 30);
	}

	int PerPage()
	{
		const int usable = GetTall() - 34 - 44;
		return std::max(4, usable / (RowHeight() + 3));
	}

	std::vector<CModelButton *> m_Buttons;
	vgui2::Button *m_pPrev = nullptr;
	vgui2::Button *m_pNext = nullptr;
	int m_iPage = 0;
};

vgui2::DHANDLE<CModelMenu> s_hMenu;

void CModelButton::DoClick()
{
	BaseClass::DoClick();
	PreviewIndex(m_iIndex);
	ApplyIndex(m_iIndex);

	if (modelmenu_close_on_apply.GetBool())
	{
		s_bOpen = false;
		if (s_hMenu.Get())
			s_hMenu->SetVisible(false);
	}
}

void ShowMenu()
{
	if (!g_pViewport || !modelmenu_panel.GetBool())
		return;

	if (!s_hMenu.Get())
	{
		CModelMenu *menu = new CModelMenu();
		menu->SetParent(g_pViewport->GetVPanel());
		menu->MakeReadyForUse();
		s_hMenu = menu;
	}

	s_hMenu->Open();
}

void HideMenu()
{
	if (s_hMenu.Get() && s_hMenu->IsVisible())
		s_hMenu->SetVisible(false);
}

void Step(int delta)
{
	if (!s_bOpen || s_Models.empty())
		return;

	int count = (int)s_Models.size();
	s_Index = ((s_Index + delta) % count + count) % count;
	LoadPreviewModel();
	if (!s_hMenu.Get() || !s_hMenu->IsVisible())
		ShowCurrent();
}
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
CON_COMMAND(modelmenu, "Opens/closes the 3D player model selection preview")
{
	if (s_bOpen)
	{
		s_bOpen = false;
		HideMenu();
		return;
	}

	if (!gEngfuncs.GetLocalPlayer() || gEngfuncs.GetMaxClients() <= 0)
		return;

	BuildList();

	if (s_Models.empty())
	{
		ConPrintf("modelmenu: no player models found.\n");
		return;
	}

	s_Index = FindCurrentModelIndex();
	LoadPreviewModel();
	s_bOpen = true;
	ShowMenu();
	if (!s_hMenu.Get() || !s_hMenu->IsVisible())
		ShowCurrent();
}

CON_COMMAND(modelmenu_next, "Preview: next model")
{
	Step(1);
}

CON_COMMAND(modelmenu_prev, "Preview: previous model")
{
	Step(-1);
}

CON_COMMAND(modelmenu_apply, "Preview: apply the selected model")
{
	if (!s_bOpen || s_Models.empty())
		return;

	ApplyIndex(s_Index);
	s_bOpen = false;
	HideMenu();
}

// ---------------------------------------------------------------------------
// Per-frame hook
// ---------------------------------------------------------------------------
bool modelpreview::IsOpen()
{
	return s_bOpen;
}

bool modelpreview::HasPlayerModel(const char *name)
{
	if (!IsSafeModelName(name))
		return false;

	// The scoreboard asks often; look at the disk at most every 3 seconds per name
	static std::map<std::string, std::pair<float, bool>> s_Cache;
	const float now = gEngfuncs.GetClientTime();
	auto it = s_Cache.find(name);
	if (it != s_Cache.end() && now - it->second.first < 3.0f && now >= it->second.first)
		return it->second.second;

	char path[256];
	ModelPath(name, path, sizeof(path));
	const bool exists = g_pFullFileSystem->FileExists(path);
	s_Cache[name] = std::make_pair(now, exists);
	return exists;
}

bool modelpreview::ShowScoreboardModel(int slot, const char *name, int x, int y, int size)
{
	if (slot < 0 || slot > 1 || size <= 0)
		return false;

	TeamRequest &t = s_Team[slot];

	if (t.name != (name ? name : ""))
	{
		t.name = name ? name : "";
		t.sequence = 0;
		t.model = LoadPlayerModel(t.name.c_str(), t.sequence);

		// No model with the name of the team on this computer: a helmet stands in for it
		if (!t.model)
			t.model = LoadPlayerModel("helmet", t.sequence);

		t.animStart = gEngfuncs.GetClientTime();
	}

	if (!t.model)
		return false;

	t.x = x;
	t.y = y;
	t.size = size;
	t.lastRequest = gEngfuncs.GetClientTime();
	return true;
}

bool modelpreview::IsPreviewEntity(const cl_entity_t *ent)
{
	return ent && (ent == &s_MenuEnt || ent == &s_TeamEnt[0] || ent == &s_TeamEnt[1]);
}

void modelpreview::Apply(ref_params_s *pparams)
{
	const float time = gEngfuncs.GetClientTime();

	// The list/preview is not shown in the intermission camera
	if (s_bOpen && pparams->intermission)
		s_bOpen = false;

	// The list can be closed with its own close button
	if (s_bOpen && s_hMenu.Get() && !s_hMenu->IsVisible() && modelmenu_panel.GetBool() && s_bMenuShown)
		s_bOpen = false;

	Vector forward, right, up;
	AngleVectors(pparams->viewangles, forward, right, up);

	if (!s_bOpen)
	{
		s_bMenuShown = false;
		HideMenu();
	}
	else
	{
		if (!s_pModel)
			LoadPreviewModel();

		if (!s_pModel)
		{
			s_bOpen = false;
		}
		else
		{
			if (s_hMenu.Get() && s_hMenu->IsVisible())
				s_bMenuShown = true;

			if (s_bAnimDirty)
			{
				s_flAnimStart = time;
				s_bAnimDirty = false;
			}

			const float dist = modelmenu_dist.GetFloat();
			const float side = modelmenu_right.GetFloat();
			const float height = modelmenu_up.GetFloat();
			const float yaw = pparams->viewangles[1] + 180.0f + fmodf(time * modelmenu_spin.GetFloat(), 360.0f);

			float origin[3];
			for (int i = 0; i < 3; i++)
				origin[i] = pparams->vieworg[i] + forward[i] * dist + right[i] * side + up[i] * height;

			FillEntity(s_MenuEnt, s_pModel, origin, yaw, s_iSequence, s_flAnimStart);
			gEngfuncs.CL_CreateVisibleEntity(ET_NORMAL, &s_MenuEnt);
		}
	}

	// Team models requested by the scoreboard (screen rectangle -> a place in front of the camera)
	const float fov = (gHUD.m_iFOV > 0) ? (float)gHUD.m_iFOV : 90.0f;
	const float tanHalf = tanf(fov * 0.5f * 3.14159265f / 180.0f);
	const float screenW = (float)ScreenWidth;
	const float screenH = (float)ScreenHeight;
	const float modelHeight = 72.0f;

	for (int slot = 0; slot < 2; slot++)
	{
		TeamRequest &t = s_Team[slot];
		if (!t.model || time - t.lastRequest > 0.25f || t.size <= 0 || screenW <= 0 || tanHalf <= 0)
			continue;

		// distance at which a player model is exactly as tall as the requested square
		float dist = modelHeight * (screenW * 0.5f) / ((float)t.size * tanHalf);
		dist = std::min(std::max(dist, 30.0f), 3000.0f);
		const float unitsPerPixel = tanHalf * dist / (screenW * 0.5f);

		const float dx = (t.x + t.size * 0.5f) - screenW * 0.5f;
		const float dy = (t.y + t.size * 0.5f) - screenH * 0.5f;

		float origin[3];
		for (int i = 0; i < 3; i++)
		{
			origin[i] = pparams->vieworg[i] + forward[i] * dist + right[i] * (dx * unitsPerPixel)
			    - up[i] * (dy * unitsPerPixel) - up[i] * (modelHeight * 0.5f);
		}

		FillEntity(s_TeamEnt[slot], t.model, origin, pparams->viewangles[1] + 180.0f, t.sequence, t.animStart);
		gEngfuncs.CL_CreateVisibleEntity(ET_NORMAL, &s_TeamEnt[slot]);
	}
}
