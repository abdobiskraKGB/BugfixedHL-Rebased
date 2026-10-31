#include <cstdint>
#include <cstring>
#include <string>

#pragma warning(push, 3)
#include <winsani_in.h>
#include <windows.h>
#include <winsani_out.h>
#pragma warning(pop)

#include <convar.h>
#include "hud.h"
#include "cl_util.h"
#include "hud/arabic_text.h"
#include "console_echo_hook.h"

// Why this exists: when you press Enter in the console, GameUI itself prints "] " and then the raw
// command text into the history through its internal CGameConsoleDialog::Print. That text is in
// logical order and unshaped, so Arabic looks reversed and disconnected. Nothing in the client sees
// this call, so the single call instruction that prints the command text is redirected here.
//
// Verified against GameUI.dll (build 8684, image base 0x10000000):
//   10033ca2: push edx            ; edx = command text
//   10033ca3: call 0x100335a0     ; CGameConsoleDialog::Print(const char *)  (thiscall, ret 4)
//   10033ca8: push 0x100b0ae8     ; next statement prints the line break
// If any byte differs (another GameUI build) nothing is patched.

static ConVar con_arabic_echo("con_arabic_echo", "1", FCVAR_BHL_ARCHIVE,
	"Show Arabic commands echoed in the console history joined and in the right order (0 = off)");

namespace
{
constexpr uintptr_t PRINT_RVA = 0x335A0;
constexpr uintptr_t CALL_SITE_RVA = 0x33CA3;
constexpr uint8_t PRINT_PROLOGUE[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x28, 0x01, 0x00, 0x00 };

using PrintFn = void(__thiscall *)(void *, const char *);

PrintFn s_fnPrint = nullptr;
uint8_t *s_pCallSite = nullptr;
int32_t s_iOriginalRel = 0;
int32_t s_iHookedRel = 0;

void __fastcall HookedEchoPrint(void *thisptr, void * /*edx*/, const char *text)
{
	if (text && con_arabic_echo.GetBool())
	{
		// Command syntax is left to right, only Arabic runs are shaped and reordered
		const std::string visual = ArabicText::ToVisualUtf8(text, ArabicText::DIR_LTR);
		s_fnPrint(thisptr, visual.c_str());
		return;
	}

	s_fnPrint(thisptr, text);
}

bool WriteRel32(uint8_t *callSite, int32_t rel)
{
	DWORD oldProtect = 0;
	if (!VirtualProtect(callSite + 1, 4, PAGE_EXECUTE_READWRITE, &oldProtect))
		return false;

	memcpy(callSite + 1, &rel, sizeof(rel));
	FlushInstructionCache(GetCurrentProcess(), callSite, 5);

	DWORD ignored = 0;
	VirtualProtect(callSite + 1, 4, oldProtect, &ignored);
	return true;
}
}

bool ConsoleEchoHook_Install(void *hGameUI)
{
	if (s_pCallSite || !hGameUI)
		return false;

	uint8_t *base = reinterpret_cast<uint8_t *>(hGameUI);
	uint8_t *print = base + PRINT_RVA;
	uint8_t *site = base + CALL_SITE_RVA;

	if (IsBadReadPtr(site - 1, 11) || IsBadReadPtr(print, sizeof(PRINT_PROLOGUE)))
	{
		gEngfuncs.Con_DPrintf("Console echo: GameUI code not readable, Arabic echo fix disabled.\n");
		return false;
	}

	int32_t rel = 0;
	memcpy(&rel, site + 1, sizeof(rel));

	const bool matches = site[-1] == 0x52 && // push edx
	    site[0] == 0xE8 && // call rel32
	    site[5] == 0x68 && // push imm32
	    site + 5 + rel == print && // calls Print
	    memcmp(print, PRINT_PROLOGUE, sizeof(PRINT_PROLOGUE)) == 0;

	if (!matches)
	{
		gEngfuncs.Con_DPrintf("Console echo: unknown GameUI build, Arabic echo fix disabled.\n");
		return false;
	}

	s_fnPrint = reinterpret_cast<PrintFn>(print);
	s_iOriginalRel = rel;
	s_iHookedRel = static_cast<int32_t>(reinterpret_cast<uint8_t *>(&HookedEchoPrint) - (site + 5));

	if (!WriteRel32(site, s_iHookedRel))
	{
		s_fnPrint = nullptr;
		return false;
	}

	s_pCallSite = site;
	return true;
}

void ConsoleEchoHook_Remove()
{
	if (!s_pCallSite)
		return;

	int32_t current = 0;
	if (!IsBadReadPtr(s_pCallSite + 1, 4))
		memcpy(&current, s_pCallSite + 1, sizeof(current));

	// Restore only if it is still our redirect
	if (current == s_iHookedRel)
		WriteRel32(s_pCallSite, s_iOriginalRel);

	s_pCallSite = nullptr;
	s_fnPrint = nullptr;
}
