#ifndef MODEL_PREVIEW_H
#define MODEL_PREVIEW_H

struct ref_params_s;

//! In-game player model picker with a 3D preview (drawn through the viewmodel entity).
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
//! Replaces the viewmodel with the selected player model while the preview is open
//! and restores the weapon viewmodel after it is closed.
void Apply(struct ref_params_s *pparams);
}

#endif
