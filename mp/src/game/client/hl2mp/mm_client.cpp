//========= Copyright Buster Bunny, All rights reserved. ============//
//
// Purpose:		Player for HL2.
//
//=============================================================================//

#include "cbase.h"
#include "steam/steam_api.h"
#include <convar.h>

void TestSteamAPI()
{
	if (!SteamAPI_Init())
	{
		Warning("Fatal Error - Steam must be running to play this game (SteamAPI_Init() failed).\n");
		return;
	}
	SteamAPI_Shutdown();
}

ConCommand test_steam_api("test_steam_api", TestSteamAPI);
