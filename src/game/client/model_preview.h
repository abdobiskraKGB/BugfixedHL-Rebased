#ifndef MODEL_PREVIEW_H
#define MODEL_PREVIEW_H

struct ref_params_s;
struct cl_entity_s;

//! In-game player model picker with a 3D preview (an entity of its own, drawn on top of the world).
//!
//! Console commands (bind them to keys):
//!   modelmenu        - open/close the preview
//!   modelmenu_next   - next model
//!   modelmenu_prev   - previous model
//!   modelmenu_apply  - send "model <name>" to the server and close
namespace modelpreview
{
//! @returns true while the preview is open.
bool IsOpen();

//! Call at the very end of V_CalcRefdef (view.cpp), every frame.
//! Hands the preview entities (menu model, scoreboard team models) to the engine.
void Apply(struct ref_params_s *pparams);

//! True if models/player/<name>/<name>.mdl exists (name: letters, digits, '_' and '-' only).
bool HasPlayerModel(const char *name);

//! Scoreboard: shows the model of a team inside the given screen square (pixels). Call every frame
//! while the scoreboard is visible. @p slot 0 = left team, 1 = right team.
//! The model "helmet" is used if there is no model named after the team.
//! @returns false if nothing can be shown (no model at all), so a picture should be used instead.
bool ShowScoreboardModel(int slot, const char *name, int x, int y, int size);

//! True for the entities created by this module (the studio renderer draws them over the world).
bool IsPreviewEntity(const struct cl_entity_s *ent);
}

#endif
