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
#include "entity_types.h"
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
static ConVar modelmenu_skills("modelmenu_skills", "1", FCVAR_BHL_ARCHIVE, "After turning to face you the model shows walk/run/jump/attack, one every 3 seconds (0 = only idle)");
static ConVar modelmenu_turn("modelmenu_turn", "1.4", FCVAR_BHL_ARCHIVE, "Seconds the model needs for its turn when another model is selected (0 = no turn)");
static ConVar hud_scoreboard_teammodel_crop("hud_scoreboard_teammodel_crop", "40", FCVAR_BHL_ARCHIVE, "Height in game units of the model shown in the scoreboard (72 = the whole body, 36 = the upper half)");

namespace
{
bool s_bOpen = false;
bool s_bAnimDirty = true;
bool s_bMenuShown = false;
float s_flTurnStart = -100.0f;
std::vector<int> s_Skills; // idle, walk, run, jump, attack...
float s_flSavedDrawViewModel = -1.0f; // r_drawviewmodel before the preview hid the weapon
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

//! First sequence whose label equals @p exact or contains @p part (and none of the words in @p not1/@p not2).
int FindSequence(model_t *model, const char *exact, const char *part, const char *not1, const char *not2)
{
	studiohdr_t *hdr = model ? (studiohdr_t *)IEngineStudio.Mod_Extradata(model) : nullptr;
	if (!hdr)
		return -1;

	mstudioseqdesc_t *seqs = (mstudioseqdesc_t *)((byte *)hdr + hdr->seqindex);

	for (int i = 0; i < hdr->numseq; i++)
	{
		if (exact && !Q_stricmp(seqs[i].label, exact))
			return i;
	}

	for (int i = 0; part && i < hdr->numseq; i++)
	{
		if (strstr(seqs[i].label, part) && (!not1 || !strstr(seqs[i].label, not1)) && (!not2 || !strstr(seqs[i].label, not2)))
			return i;
	}

	return -1;
}

//! The moves shown one after another: idle first, then walking, running, jumping and a melee attack.
void BuildSkills(model_t *model)
{
	s_Skills.clear();
	s_Skills.push_back(FindIdleSequence(model));

	const int found[] = {
		FindSequence(model, "walk", "walk", "crouch", "back"),
		FindSequence(model, "run", "run", "crouch", "back"),
		FindSequence(model, "jump", "jump", "long", nullptr),
		FindSequence(model, "ref_shoot_crowbar", "crowbar", "aim", "crouch"),
	};

	for (int seq : found)
	{
		if (seq >= 0 && std::find(s_Skills.begin(), s_Skills.end(), seq) == s_Skills.end())
			s_Skills.push_back(seq);
	}
}

void LoadPreviewModel()
{
	s_pModel = nullptr;

	if (s_Models.empty())
		return;

	char path[256];
	ModelPath(s_Models[s_Index].c_str(), path, sizeof(path));

	s_pModel = IEngineStudio.Mod_ForName(path, 0);
	BuildSkills(s_pModel);
	s_iSequence = s_Skills.empty() ? 0 : s_Skills[0];
	s_bAnimDirty = true;
	s_flTurnStart = gEngfuncs.GetClientTime(); // the new model faces you, then turns once
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
	bool visible = false; // handed to the engine this frame (the scissor rectangle is valid)
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

// Orientation that makes a model face the camera and stay upright ON THE SCREEN whatever the view angles
// are (looking up or down must not tilt it). Same conventions as the engine: AngleMatrix for the view,
// and the studio renderer negates the pitch of an entity.
void AngleToMatrix(const float *a, float m[3][3])
{
	const float d = 3.14159265f / 180.0f;
	const float sp = sinf(a[0] * d), cp = cosf(a[0] * d);
	const float sy = sinf(a[1] * d), cy = cosf(a[1] * d);
	const float sr = sinf(a[2] * d), cr = cosf(a[2] * d);
	m[0][0] = cp * cy;
	m[1][0] = cp * sy;
	m[2][0] = -sp;
	m[0][1] = sr * sp * cy + cr * -sy;
	m[1][1] = sr * sp * sy + cr * cy;
	m[2][1] = sr * cp;
	m[0][2] = cr * sp * cy + -sr * -sy;
	m[1][2] = cr * sp * sy + -sr * cy;
	m[2][2] = cr * cp;
}

//! @p turn: extra rotation around the model's own vertical axis, in degrees (0 = looking straight at you)
void FacingCameraAngles(const float *viewAngles, float turn, float *out)
{
	float cam[3][3];
	AngleToMatrix(viewAngles, cam);

	// the model looks back at the camera: half a turn around its own up axis, plus the extra turn
	const float t = (180.0f + turn) * 3.14159265f / 180.0f;
	const float c = cosf(t), sn = sinf(t);
	const float rz[3][3] = { { c, -sn, 0 }, { sn, c, 0 }, { 0, 0, 1 } };

	float m[3][3];
	for (int i = 0; i < 3; i++)
	{
		for (int j = 0; j < 3; j++)
			m[i][j] = cam[i][0] * rz[0][j] + cam[i][1] * rz[1][j] + cam[i][2] * rz[2][j];
	}

	const float xy = sqrtf(m[0][0] * m[0][0] + m[1][0] * m[1][0]);
	float yaw, pitch, roll;
	if (xy > 0.001f)
	{
		yaw = atan2f(m[1][0], m[0][0]);
		pitch = atan2f(-m[2][0], xy);
		roll = atan2f(m[2][1], m[2][2]);
	}
	else
	{
		yaw = atan2f(-m[0][1], m[1][1]);
		pitch = atan2f(-m[2][0], xy);
		roll = 0;
	}

	const float r = 180.0f / 3.14159265f;
	out[0] = -pitch * r; // the studio renderer negates the pitch of entities
	out[1] = yaw * r;
	out[2] = roll * r;
}

// Fills a fresh entity: neutral pose (controllers/blends in the middle of their range) and an animation
// clock that only we control.
void FillEntity(cl_entity_t &e, model_t *model, const float *origin, const float *angles, int sequence, float animStart)
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
		e.angles[i] = e.curstate.angles[i] = e.latched.prevangles[i] = angles[i];
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

bool modelpreview::GetScissor(const cl_entity_t *ent, int &x, int &y, int &w, int &h)
{
	for (int slot = 0; slot < 2; slot++)
	{
		if (ent == &s_TeamEnt[slot] && s_Team[slot].visible)
		{
			x = s_Team[slot].x;
			y = s_Team[slot].y;
			w = h = s_Team[slot].size;
			return true;
		}
	}

	return false;
}

void modelpreview::Reset()
{
	// Model pointers and the client clock do not survive a map change
	s_bOpen = false;
	HideMenu();
	s_pModel = nullptr;
	s_Skills.clear();
	if (s_flSavedDrawViewModel >= 0.0f)
	{
		gEngfuncs.Cvar_SetValue("r_drawviewmodel", s_flSavedDrawViewModel);
		s_flSavedDrawViewModel = -1.0f;
	}

	for (TeamRequest &t : s_Team)
	{
		t.name.clear();
		t.model = nullptr;
		t.lastRequest = -100;
		t.visible = false;
	}
}

void modelpreview::Apply(ref_params_s *pparams)
{
	const float time = gEngfuncs.GetClientTime();

	// The client clock starts again at every map: a stamp from the future is a stale one
	if (s_flTurnStart > time)
		s_flTurnStart = time;

	// The list/preview is not shown in the intermission camera
	if (s_bOpen && pparams->intermission)
		s_bOpen = false;

	// The list can be closed with its own close button
	if (s_bOpen && s_hMenu.Get() && !s_hMenu->IsVisible() && modelmenu_panel.GetBool() && s_bMenuShown)
		s_bOpen = false;

	Vector forward, right, up;
	AngleVectors(pparams->viewangles, forward, right, up);

	bool menuDrawn = false;

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

			// A new model first looks straight at you, makes one turn, and then shows its moves,
			// one every 3 seconds: idle -> walk -> run -> jump -> attack -> idle ...
			const float turnTime = modelmenu_turn.GetFloat();
			const float since = time - s_flTurnStart;
			float turn = 0.0f;
			int sequence = s_Skills.empty() ? s_iSequence : s_Skills[0];
			float animStart = s_flAnimStart;

			if (turnTime > 0.0f && since >= 0.0f && since < turnTime)
			{
				const float k = since / turnTime;
				turn = 360.0f * k * k * (3.0f - 2.0f * k); // eases in and out
			}

			if (modelmenu_skills.GetBool() && s_Skills.size() > 1 && since >= turnTime)
			{
				const int step = (int)((since - turnTime) / 3.0f);
				sequence = s_Skills[(step + 1) % (int)s_Skills.size()];
				animStart = s_flTurnStart + turnTime + step * 3.0f; // every move starts at its first frame
			}

			const float dist = modelmenu_dist.GetFloat();
			const float side = modelmenu_right.GetFloat();
			const float height = modelmenu_up.GetFloat();

			float origin[3], angles[3];
			for (int i = 0; i < 3; i++)
				origin[i] = pparams->vieworg[i] + forward[i] * dist + right[i] * side + up[i] * height;

			FacingCameraAngles(pparams->viewangles, turn, angles);

			FillEntity(s_MenuEnt, s_pModel, origin, angles, sequence, animStart);
			gEngfuncs.CL_CreateVisibleEntity(ET_NORMAL, &s_MenuEnt);
			menuDrawn = true;
		}
	}

	// While you are alive your weapon would stand in front of the model: it is hidden (r_drawviewmodel 0)
	// while the preview is open and the previous value comes back when it closes
	if (menuDrawn)
	{
		if (s_flSavedDrawViewModel < 0.0f)
		{
			s_flSavedDrawViewModel = gEngfuncs.pfnGetCvarFloat("r_drawviewmodel");
			gEngfuncs.Cvar_SetValue("r_drawviewmodel", 0.0f);
		}
	}
	else if (s_flSavedDrawViewModel >= 0.0f)
	{
		gEngfuncs.Cvar_SetValue("r_drawviewmodel", s_flSavedDrawViewModel);
		s_flSavedDrawViewModel = -1.0f;
	}

	// Team models requested by the scoreboard (screen rectangle -> a place in front of the camera)
	const float fov = (gHUD.m_iFOV > 0) ? (float)gHUD.m_iFOV : 90.0f;
	const float tanHalf = tanf(fov * 0.5f * 3.14159265f / 180.0f);
	const float screenW = (float)ScreenWidth;
	const float screenH = (float)ScreenHeight;

	// Only the upper part of the body is shown (the rest is cut off by the scissor of the renderer):
	// the visible height fills the whole square, and the top of the head is at the top of the square.
	const float modelHeight = 72.0f;
	const float visible = std::min(std::max(hud_scoreboard_teammodel_crop.GetFloat(), 20.0f), modelHeight);

	for (int slot = 0; slot < 2; slot++)
	{
		TeamRequest &t = s_Team[slot];
		t.visible = false;

		// "age < 0": the map changed and the clock started again
		const float age = time - t.lastRequest;
		if (!t.model || age < 0.0f || age > 0.25f || t.size <= 0 || screenW <= 0 || tanHalf <= 0)
			continue;

		float dist = visible * (screenW * 0.5f) / ((float)t.size * tanHalf);
		dist = std::min(std::max(dist, 30.0f), 3000.0f);
		const float unitsPerPixel = tanHalf * dist / (screenW * 0.5f);

		const float dx = (t.x + t.size * 0.5f) - screenW * 0.5f;
		const float dy = (t.y + t.size * 0.5f) - screenH * 0.5f;
		const float centerHeight = modelHeight - visible * 0.5f; // height of the model at the center of the square

		float origin[3], angles[3];
		for (int i = 0; i < 3; i++)
		{
			origin[i] = pparams->vieworg[i] + forward[i] * dist + right[i] * (dx * unitsPerPixel)
			    - up[i] * (dy * unitsPerPixel) - up[i] * centerHeight;
		}

		FacingCameraAngles(pparams->viewangles, 0.0f, angles);

		FillEntity(s_TeamEnt[slot], t.model, origin, angles, t.sequence, t.animStart);
		gEngfuncs.CL_CreateVisibleEntity(ET_NORMAL, &s_TeamEnt[slot]);
		t.visible = true;
	}
}
