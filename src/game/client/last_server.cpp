// "Join last server": remembers the server you were connected to and reconnects with one command.
//
//   joinlast            - connect to the last remembered server
//   cl_lastserver       - the remembered address (archived in config.cfg, survives restarts)
//
// For the main menu add the entry from resource/GameMenu.res ("engine joinlast").

#include <stdio.h>

#include "hud.h"
#include "cl_util.h"
#include "console.h"
#include "net_api.h"
#include "demo_api.h"
#include <convar.h>

static ConVar cl_lastserver("cl_lastserver", "", FCVAR_BHL_ARCHIVE, "Address of the last multiplayer server you joined (used by joinlast)");

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
		cl_lastserver.SetValue(address);
}

CON_COMMAND(joinlast, "Connect to the last server you played on")
{
	const char *address = cl_lastserver.GetString();
	if (!address || !address[0])
	{
		ConPrintf("joinlast: no previous server is remembered yet.\n");
		return;
	}

	// Only digits, dots and a colon are expected; refuse anything that could chain other commands
	for (const char *c = address; *c; ++c)
	{
		if (!((*c >= '0' && *c <= '9') || *c == '.' || *c == ':'))
		{
			ConPrintf("joinlast: the remembered address is not valid.\n");
			return;
		}
	}

	ConPrintf("Connecting to %s ...\n", address);

	char cmd[64];
	snprintf(cmd, sizeof(cmd), "connect %s\n", address);
	EngineClientCmd(cmd);
}
