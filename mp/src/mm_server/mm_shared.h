//====== Copyright Buster Bunny, All rights reserved. ====================
//
// Purpose:		Matchmaking Client/Server Shared Code
//
//=============================================================================

#ifndef MM_SHARED_H
#define MM_SHARED_H
#ifdef _WIN32
#pragma once
#endif

#include <steam/steamnetworkingsockets.h>
#include <string>
#include <map>

/// Handle used to identify a lobby.
typedef uint32 HLobbyID;

#define invalid_lobby (HLobbyID)-1

enum MessageType
{
	string,
	request_lobby_list,
	lobby_list,
	request_create_lobby,
	request_join_lobby,
	request_leave_lobby,
	message_save_lobby_id,
	message_no_suitable_lobbies,
	message_start_game,
	request_echo,
	message_echo
};

enum HL2DM_Map
{
	dm_lockdown,
	dm_overwatch,
	dm_powerhouse,
	dm_resistance,
	dm_runoff,
	dm_steamlab,
	dm_underpass,
	halls3
};

struct Client_t
{
	std::string m_sNick;
};

struct Player
{
	Client_t m_Client;
	bool m_bEnemy;
};

struct Lobby
{
	std::map< HSteamNetConnection, Player > m_mapPlayers;
	HL2DM_Map m_map;
	bool m_bTeamDM;
};

EResult SendTypedMessage(HSteamNetConnection hConn, const void *pData, uint32 cbData, int nSendFlags, int64 *pOutMessageNumber, MessageType eType, ISteamNetworkingSockets* pInterface);
EResult SendOnlyMessageType(HSteamNetConnection hConn, int nSendFlags, int64 *pOutMessageNumber, MessageType eType, ISteamNetworkingSockets* pInterface);
MessageType DetermineMessageType(ISteamNetworkingMessage* pMessage);
void* RemoveFirstByte(void **Destination, const void *pData, uint32 cbData);

#endif