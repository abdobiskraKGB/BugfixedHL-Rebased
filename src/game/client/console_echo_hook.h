#pragma once

// Re-shapes Arabic commands echoed into the console history ("] name <arabic>").
// Only implemented on Windows, for the GameUI build whose code layout is verified at runtime.
#ifdef PLATFORM_WINDOWS
// hGameUI is the HMODULE of GameUI.dll. Returns false (and changes nothing) if the code does not match.
bool ConsoleEchoHook_Install(void *hGameUI);
// Must be called before GameUI is unloaded and before this DLL is unloaded.
void ConsoleEchoHook_Remove();
#else
inline bool ConsoleEchoHook_Install(void *) { return false; }
inline void ConsoleEchoHook_Remove() {}
#endif
