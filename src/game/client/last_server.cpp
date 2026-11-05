// "Join last server": remembers the server you were connected to and reconnects with one command.
//
//   joinlast            - connect to the last remembered server
//   cl_lastserver       - the remembered address
//
// The address is saved at once in <gamedir>/lastserver.txt (so it survives closing the game from the
// task bar, which does not write config.cfg) and is loaded again when the game starts.
//
// The main menu entry "Join Last Server" is added to <gamedir>/resource/GameMenu.res as soon as a server
// is remembered, so it appears the next time the game starts, and not before the first server was joined.

#include <stdio.h>
#include <string.h>
#include <string>

#include "hud.h"
#include "cl_util.h"
#include "console.h"
#include "net_api.h"
#include "demo_api.h"
#include <convar.h>

static ConVar cl_lastserver("cl_lastserver", "", FCVAR_BHL_ARCHIVE, "Address of the last multiplayer server you joined (used by joinlast)");

namespace
{
const char MENU_LABEL[] = "Join Last Server";

std::string GamePath(const char *relative)
{
	const char *gameDir = gEngfuncs.pfnGetGameDirectory();
	return std::string(gameDir && gameDir[0] ? gameDir : "valve") + "/" + relative;
}

// Only digits, dots and a colon: nothing that could chain other commands
bool IsValidAddress(const char *address)
{
	if (!address || !address[0])
		return false;

	size_t length = 0;
	for (const char *c = address; *c; ++c, ++length)
	{
		if (!((*c >= '0' && *c <= '9') || *c == '.' || *c == ':'))
			return false;
	}

	return length >= 7 && length <= 21;
}

bool ReadFile(const std::string &path, std::string &out)
{
	FILE *f = fopen(path.c_str(), "rb");
	if (!f)
		return false;

	char buffer[4096];
	size_t read;
	out.clear();
	while ((read = fread(buffer, 1, sizeof(buffer), f)) > 0)
		out.append(buffer, read);

	fclose(f);
	return true;
}

bool WriteFile(const std::string &path, const std::string &text)
{
	FILE *f = fopen(path.c_str(), "wb");
	if (!f)
		return false;

	const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
	fclose(f);
	return ok;
}

// Adds or removes the "Join Last Server" entry of the main menu. The file is only touched when it has to
// change, and only if it exists (without a GameMenu.res in the game folder the default menu is in use).
void SyncMenuEntry(bool wanted)
{
	const std::string path = GamePath("resource/GameMenu.res");
	std::string text;
	if (!ReadFile(path, text))
		return;

	const size_t label = text.find(MENU_LABEL);
	const bool present = label != std::string::npos;
	if (present == wanted)
		return;

	const bool crlf = text.find("\r\n") != std::string::npos;
	const char *nl = crlf ? "\r\n" : "\n";

	if (wanted)
	{
		const size_t end = text.rfind('}');
		if (end == std::string::npos)
			return;

		std::string block;
		block += "\t\"joinlast\"";
		block += nl;
		block += "\t{";
		block += nl;
		block += "\t\t\"label\" \"Join Last Server\"";
		block += nl;
		block += "\t\t\"command\" \"engine joinlast\"";
		block += nl;
		block += "\t\t\"notsingle\" \"1\"";
		block += nl;
		block += "\t}";
		block += nl;
		text.insert(end, block);
	}
	else
	{
		// remove the whole block that contains the label: "key" { ... }
		const size_t open = text.rfind('{', label);
		const size_t close = text.find('}', label);
		if (open == std::string::npos || close == std::string::npos)
			return;

		const size_t keyEnd = text.rfind('"', open);
		if (keyEnd == std::string::npos || keyEnd == 0)
			return;
		const size_t keyStart = text.rfind('"', keyEnd - 1);
		if (keyStart == std::string::npos)
			return;

		size_t from = text.rfind('\n', keyStart);
		from = (from == std::string::npos) ? keyStart : from + 1;
		size_t to = close + 1;
		while (to < text.size() && (text[to] == '\r' || text[to] == '\n'))
		{
			++to;
			if (text[to - 1] == '\n')
				break;
		}
		text.erase(from, to - from);
	}

	WriteFile(path, text);
}
}

// True once a server has been remembered (menu entry visibility)
bool LastServer_Has()
{
	return IsValidAddress(cl_lastserver.GetString());
}

// Called when the client starts: the remembered address is read back from disk
void LastServer_Load()
{
	std::string text;
	if (ReadFile(GamePath("lastserver.txt"), text))
	{
		// first line only
		size_t eol = text.find_first_of("\r\n");
		if (eol != std::string::npos)
			text.erase(eol);

		if (IsValidAddress(text.c_str()))
			cl_lastserver.SetValue(text.c_str());
	}

	SyncMenuEntry(LastServer_Has());
}

// Called on every map load / connection (HUD_VidInit)
void LastServer_Update()
{
	if (gEngfuncs.pDemoAPI->IsPlayingback())
		return;

	// Make sure networking system has started.
	gEngfuncs.pNetAPI->InitNetworking();

	net_status_t status;
	gEngfuncs.pNetAPI->Status(&status);

	// Listen servers (loopback) and not connected states are not worth remembering
	if (!status.connected || status.remote_address.type != NA_IP)
		return;

	const unsigned char *ip = status.remote_address.ip;
	// The port is stored in network byte order
	const unsigned int port = ((status.remote_address.port & 0xFF) << 8) | ((status.remote_address.port >> 8) & 0xFF);

	if (ip[0] == 127 || port == 0)
		return;

	char address[32];
	snprintf(address, sizeof(address), "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], port);

	if (strcmp(cl_lastserver.GetString(), address) != 0)
	{
		cl_lastserver.SetValue(address);
		WriteFile(GamePath("lastserver.txt"), std::string(address) + "\n");
		SyncMenuEntry(true);
	}
}

CON_COMMAND(joinlast, "Connect to the last server you played on")
{
	const char *address = cl_lastserver.GetString();
	if (!IsValidAddress(address))
	{
		ConPrintf("joinlast: no previous server is remembered yet.\n");
		return;
	}

	ConPrintf("Connecting to %s ...\n", address);

	char cmd[64];
	snprintf(cmd, sizeof(cmd), "connect %s\n", address);
	EngineClientCmd(cmd);
}
