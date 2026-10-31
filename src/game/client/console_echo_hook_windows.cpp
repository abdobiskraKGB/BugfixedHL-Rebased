#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

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
//
// Second redirect: the same handler sends the typed command to the engine right before the echo:
//   10033c85: lea eax,[esp+8] ; push eax ; call [ecx+0x50] (engine->pfnClientCmd) ; add esp,4
// "name <arabic>" and "say <arabic>" are converted to the shaped form BEFORE they reach the engine, so
// the server announces one name change in the shaped form instead of a raw one followed by a fixed one.

extern ConVar hud_chat_arabic_name;
extern ConVar hud_chat_arabic_send;
void ChatTranslate_SendLine(const char *text, bool team); // hud/chat.cpp

static ConVar con_arabic_echo("con_arabic_echo", "1", FCVAR_BHL_ARCHIVE,
	"Show Arabic commands echoed in the console history joined and in the right order (0 = off)");

namespace
{
constexpr uintptr_t PRINT_RVA = 0x335A0;
constexpr uintptr_t CALL_SITE_RVA = 0x33CA3;
constexpr uint8_t PRINT_PROLOGUE[] = { 0x56, 0x8B, 0xF1, 0x8B, 0x86, 0x28, 0x01, 0x00, 0x00 };

constexpr uintptr_t SUBMIT_CALL_RVA = 0x33C8A;
constexpr uint8_t SUBMIT_CODE_BEFORE[] = { 0x8D, 0x44, 0x24, 0x08, 0x50 }; // lea eax,[esp+8] ; push eax
constexpr uint8_t SUBMIT_CODE[] = { 0xFF, 0x51, 0x50, 0x83, 0xC4, 0x04 }; // call [ecx+0x50] ; add esp,4
constexpr uint8_t SUBMIT_CODE_AFTER[] = { 0x8B, 0xCE }; // mov ecx,esi

using PrintFn = void(__thiscall *)(void *, const char *);

PrintFn s_fnPrint = nullptr;
uint8_t *s_pCallSite = nullptr;
int32_t s_iOriginalRel = 0;
int32_t s_iHookedRel = 0;

uint8_t *s_pSubmitSite = nullptr;
uint8_t s_SubmitOriginal[sizeof(SUBMIT_CODE)] = {};
int32_t s_iSubmitHookedRel = 0;

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

bool IsCommand(const char *&cursor, const char *word)
{
	const size_t length = strlen(word);
	if (_strnicmp(cursor, word, length) != 0)
		return false;
	const char next = cursor[length];
	if (next != ' ' && next != '\t')
		return false;
	cursor += length;
	return true;
}

// Returns true when the command was Arabic and has been handled here (sent shaped)
bool SendShapedCommand(const char *command)
{
	const char *cursor = command;
	while (*cursor == ' ' || *cursor == '\t')
		++cursor;

	enum { NONE, NAME, SAY, SAY_TEAM } kind = NONE;
	if (IsCommand(cursor, "name"))
		kind = NAME;
	else if (IsCommand(cursor, "say_team"))
		kind = SAY_TEAM;
	else if (IsCommand(cursor, "say"))
		kind = SAY;
	if (kind == NONE)
		return false;

	std::string text = cursor;
	const size_t first = text.find_first_not_of(" \t");
	if (first == std::string::npos)
		return false;
	text.erase(0, first);
	while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == ';'))
		text.pop_back();
	// A single pair of quotes around the whole argument is part of the syntax, not of the text
	if (text.size() >= 2 && text.front() == '"' && text.back() == '"' && text.find('"', 1) == text.size() - 1)
		text = text.substr(1, text.size() - 2);
	// Several commands on one line (name x; say y) are left to the engine
	if (text.empty() || text.find(';') != std::string::npos || text.find('"') != std::string::npos)
		return false;

	if (kind == NAME)
	{
		if (!hud_chat_arabic_name.GetBool())
			return false;
		const std::string visual = ArabicText::ToVisualNameUtf8(text, 31);
		if (visual == text)
			return false;
		std::string cmd = "name \"" + visual + "\"\n";
		gEngfuncs.pfnClientCmd(const_cast<char *>(cmd.c_str()));
		return true;
	}

	if (!hud_chat_arabic_send.GetBool() || ArabicText::ToVisualUtf8(text) == text)
		return false;

	// Same path as the chat box: shaped, and split into several messages when it is long
	ChatTranslate_SendLine(text.c_str(), kind == SAY_TEAM);
	return true;
}

// Replaces "call [ecx+0x50]; add esp,4" (cdecl, caller cleans) -> stdcall callee cleans the one argument
void __stdcall HookedSubmitCommand(const char *command)
{
	if (command && con_arabic_echo.GetBool() && SendShapedCommand(command))
		return;

	gEngfuncs.pfnClientCmd(const_cast<char *>(command));
}

bool WriteBytes(uint8_t *at, const uint8_t *bytes, size_t count)
{
	DWORD oldProtect = 0;
	if (!VirtualProtect(at, count, PAGE_EXECUTE_READWRITE, &oldProtect))
		return false;

	memcpy(at, bytes, count);
	FlushInstructionCache(GetCurrentProcess(), at, count);

	DWORD ignored = 0;
	VirtualProtect(at, count, oldProtect, &ignored);
	return true;
}

bool InstallSubmitHook(uint8_t *base)
{
	uint8_t *site = base + SUBMIT_CALL_RVA;
	const size_t before = sizeof(SUBMIT_CODE_BEFORE);
	const size_t after = sizeof(SUBMIT_CODE_AFTER);

	if (IsBadReadPtr(site - before, before + sizeof(SUBMIT_CODE) + after))
		return false;

	if (memcmp(site - before, SUBMIT_CODE_BEFORE, before) != 0 ||
	    memcmp(site, SUBMIT_CODE, sizeof(SUBMIT_CODE)) != 0 ||
	    memcmp(site + sizeof(SUBMIT_CODE), SUBMIT_CODE_AFTER, after) != 0)
		return false;

	memcpy(s_SubmitOriginal, site, sizeof(SUBMIT_CODE));
	s_iSubmitHookedRel = static_cast<int32_t>(reinterpret_cast<uint8_t *>(&HookedSubmitCommand) - (site + 5));

	uint8_t patch[sizeof(SUBMIT_CODE)];
	patch[0] = 0xE8; // call rel32
	memcpy(patch + 1, &s_iSubmitHookedRel, sizeof(s_iSubmitHookedRel));
	patch[5] = 0x90; // nop (the old "add esp,4" is gone: our stdcall function cleans the argument)

	if (!WriteBytes(site, patch, sizeof(patch)))
		return false;

	s_pSubmitSite = site;
	return true;
}

void RemoveSubmitHook()
{
	if (!s_pSubmitSite)
		return;

	// Restore only if it is still our redirect
	if (!IsBadReadPtr(s_pSubmitSite, sizeof(SUBMIT_CODE)) && s_pSubmitSite[0] == 0xE8)
	{
		int32_t current = 0;
		memcpy(&current, s_pSubmitSite + 1, sizeof(current));
		if (current == s_iSubmitHookedRel)
			WriteBytes(s_pSubmitSite, s_SubmitOriginal, sizeof(SUBMIT_CODE));
	}

	s_pSubmitSite = nullptr;
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
	if (s_pCallSite || s_pSubmitSite || !hGameUI)
		return false;

	uint8_t *base = reinterpret_cast<uint8_t *>(hGameUI);

	// Independent of the echo fix below; a different GameUI build simply skips it
	if (!InstallSubmitHook(base))
		gEngfuncs.Con_DPrintf("Console submit: unknown GameUI build, Arabic name/say fix disabled.\n");
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
	RemoveSubmitHook();

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
