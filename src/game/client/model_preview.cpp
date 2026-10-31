// model_preview.cpp
// In-game player model selection with a rotating 3D preview.
//
// How it works:
//  - Console commands (modelmenu, modelmenu_next, modelmenu_prev, modelmenu_apply) change the state here.
//  - Every frame V_CalcRefdef calls modelpreview::Apply(). While the preview is open, the viewmodel
//    entity is switched to the selected player model and placed in front of the camera, so the engine
//    draws it on top of the scene (same pipeline as the weapon viewmodel: no wall clipping).
//  - When the preview is closed, the original viewmodel state is restored.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
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
struct SavedViewmodel
{
	bool valid = false;
	model_t *model = nullptr;
	int modelindex = 0;
	int sequence = 0;
	int body = 0;
	float frame = 0;
	float framerate = 0;
	float animtime = 0;
};

bool s_bOpen = false;
bool s_bAnimDirty = true;
bool s_bMenuShown = false;
float s_flAnimStart = 0;
int s_Index = 0;
int s_iSequence = 0;
model_t *s_pModel = nullptr;
std::vector<std::string> s_Models;
SavedViewmodel s_Saved;

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

void ShowCurrent()
{
	if (s_Models.empty())
		return;

	char buf[128];
	snprintf(buf, sizeof(buf), "%s  [%d/%d]", s_Models[s_Index].c_str(), s_Index + 1, (int)s_Models.size());
	gEngfuncs.pfnCenterPrint(buf);
}

void SaveViewmodel(cl_entity_t *view)
{
	s_Saved.valid = true;
	s_Saved.model = view->model;
	s_Saved.modelindex = view->curstate.modelindex;
	s_Saved.sequence = view->curstate.sequence;
	s_Saved.body = view->curstate.body;
	s_Saved.frame = view->curstate.frame;
	s_Saved.framerate = view->curstate.framerate;
	s_Saved.animtime = view->curstate.animtime;
}

void RestoreViewmodel(cl_entity_t *view)
{
	view->model = s_Saved.model;
	view->curstate.modelindex = s_Saved.modelindex;
	view->curstate.sequence = s_Saved.sequence;
	view->curstate.body = s_Saved.body;
	view->curstate.frame = s_Saved.frame;
	view->curstate.framerate = s_Saved.framerate;
	view->curstate.animtime = s_Saved.animtime;
	view->latched.sequencetime = 0;
	view->latched.prevsequence = s_Saved.sequence;
	s_Saved.valid = false;
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
	int RowHeight() const
	{
		int sw, sh;
		vgui2::surface()->GetScreenSize(sw, sh);
		return std::max(22, sh / 30);
	}

	int PerPage() const
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

void modelpreview::Apply(ref_params_s *pparams)
{
	cl_entity_t *view = gEngfuncs.GetViewModel();
	if (!view)
		return;

	// The preview is not available in intermission or spectator modes.
	if (s_bOpen && (pparams->intermission || pparams->spectator || g_iUser1))
		s_bOpen = false;

	// The list can be closed with its own close button
	if (s_bOpen && s_hMenu.Get() && !s_hMenu->IsVisible() && modelmenu_panel.GetBool() && s_bMenuShown)
		s_bOpen = false;

	if (!s_bOpen)
	{
		s_bMenuShown = false;
		HideMenu();
		if (s_Saved.valid)
			RestoreViewmodel(view);
		return;
	}

	if (!s_Saved.valid)
		SaveViewmodel(view);

	if (!s_pModel)
		LoadPreviewModel();

	if (!s_pModel)
	{
		s_bOpen = false;
		RestoreViewmodel(view);
		return;
	}

	// Camera basis
	Vector forward, right, up;
	AngleVectors(pparams->viewangles, forward, right, up);

	float dist = modelmenu_dist.GetFloat();
	float side = modelmenu_right.GetFloat();
	float height = modelmenu_up.GetFloat();

	float time = gEngfuncs.GetClientTime();
	float yaw = pparams->viewangles[1] + 180.0f + fmodf(time * modelmenu_spin.GetFloat(), 360.0f);

	for (int i = 0; i < 3; i++)
	{
		float o = pparams->vieworg[i] + forward[i] * dist + right[i] * side + up[i] * height;
		view->origin[i] = o;
		view->curstate.origin[i] = o;
		view->latched.prevorigin[i] = o;
	}

	view->angles[0] = 0;
	view->angles[1] = yaw;
	view->angles[2] = 0;
	view->curstate.angles[0] = 0;
	view->curstate.angles[1] = yaw;
	view->curstate.angles[2] = 0;
	view->latched.prevangles[0] = 0;
	view->latched.prevangles[1] = yaw;
	view->latched.prevangles[2] = 0;

	if (s_hMenu.Get() && s_hMenu->IsVisible())
		s_bMenuShown = true;

	// Model and animation
	view->model = s_pModel;
	view->curstate.sequence = s_iSequence;
	view->curstate.body = 0;
	view->curstate.skin = 0;
	view->curstate.rendermode = kRenderNormal;
	view->curstate.renderfx = kRenderFxNone;
	view->curstate.renderamt = 255;
	view->curstate.gaitsequence = 0;

	// Neutral pose. Bone controllers (torso twist etc.) and blend values of the viewmodel entity are
	// left at whatever the weapon code wrote (usually 0 = the END of the range), which twists the upper
	// body against the legs. 127 is the middle of the range, i.e. "looking straight ahead".
	for (int i = 0; i < 4; i++)
	{
		view->curstate.controller[i] = 127;
		view->latched.prevcontroller[i] = 127;
	}
	for (int i = 0; i < 2; i++)
	{
		view->curstate.blending[i] = 127;
		view->latched.prevblending[i] = 127;
	}

	if (s_bAnimDirty)
	{
		s_flAnimStart = time;
		s_bAnimDirty = false;
	}

	// Rewrite the whole animation state EVERY frame. Weapon code (draw, idle, jump...) keeps writing
	// into this entity through pfnWeaponAnim, which made the preview model nod, duck or jerk.
	view->curstate.frame = 0;
	view->curstate.framerate = 1;
	view->curstate.animtime = s_flAnimStart;
	view->latched.sequencetime = 0;
	view->latched.prevsequence = s_iSequence;
	view->latched.prevframe = 0;

}
