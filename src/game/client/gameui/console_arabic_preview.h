#pragma once

namespace vgui2 { class Panel; }

// Call only from the VGUI/client thread, after VGui_InitInterfacesList.
void ConsoleArabicPreview_Init(vgui2::Panel *owner);
// Call before the client or GameUI module is unloaded. Safe to call twice.
void ConsoleArabicPreview_Shutdown();
