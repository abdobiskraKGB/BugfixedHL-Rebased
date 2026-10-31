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
#include <vgui_controls/Controls.h>

#include "hud.h"
#include "cl_util.h"
#include "const.h"
#include "com_model.h"
#include "studio.h"
#include "cl_entity.h"
#include "ref_params.h"
#include "r_studioint.h"

#include "model_preview.h"

extern engine_studio_api_t IEngineStudio;

// ---------------------------------------------------------------------------
// Settings (tweakable in-game, so no recompiling is needed to calibrate the position)
// ---------------------------------------------------------------------------
static ConVar modelmenu_models("modelmenu_models", "", FCVAR_BHL_ARCHIVE,
    "Explicit list of models for the preview, e.g. \"blue;red\" (team mods). Empty = scan models/player/");
static ConVar modelmenu_dist("modelmenu_dist", "110", FCVAR_BHL_ARCHIVE, "Preview distance from the camera");
static ConVar modelmenu_right("modelmenu_right", "35", FCVAR_BHL_ARCHIVE, "Preview offset to the right (negative = left)");
static ConVar modelmenu_up("modelmenu_up", "-30", FCVAR_BHL_ARCHIVE, "Preview vertical offset (negative = lower)");
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

void Step(int delta)
{
	if (!s_bOpen || s_Models.empty())
		return;

	int count = (int)s_Models.size();
	s_Index = ((s_Index + delta) % count + count) % count;
	LoadPreviewModel();
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

	char cmd[128];
	snprintf(cmd, sizeof(cmd), "model %s", s_Models[s_Index].c_str());
	EngineClientCmd(cmd);

	ConPrintf("Model changed to: %s\n", s_Models[s_Index].c_str());
	s_bOpen = false;
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

	if (!s_bOpen)
	{
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
		view->curstate.frame = 0;
		view->curstate.framerate = 1;
		view->curstate.animtime = time;
		view->latched.sequencetime = 0;
		view->latched.prevsequence = s_iSequence;
		view->latched.prevframe = 0;
		s_bAnimDirty = false;
	}
}
