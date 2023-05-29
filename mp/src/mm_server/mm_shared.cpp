//====== Copyright Buster Bunny, All rights reserved. ====================
//
// Purpose:		Matchmaking Client/Server Shared Code
//
//=============================================================================

#include "cbase.h"
#include "mm_shared.h"
#include <steam/steamnetworkingsockets.h>

int iNumOfPlayersToStartGame = 2;

EResult SendTypedMessage(HSteamNetConnection hConn, const void *pData, uint32 cbData, int nSendFlags, int64 *pOutMessageNumber, MessageType eType, ISteamNetworkingSockets* pInterface)
{
	void* typed_message = (void*) ::operator new (cbData + 1);
	memcpy(typed_message, &eType, 1);
	if (cbData != 0)
		memcpy((uint8*)typed_message + 1, pData, cbData);
	EResult res = pInterface->SendMessageToConnection(hConn, typed_message, cbData + 1, nSendFlags, pOutMessageNumber);
	delete typed_message;
	return res;
}

EResult SendOnlyMessageType(HSteamNetConnection hConn, int nSendFlags, int64 *pOutMessageNumber, MessageType eType, ISteamNetworkingSockets* pInterface)
{
	return SendTypedMessage(hConn, nullptr, 0, nSendFlags, pOutMessageNumber, eType, pInterface);
}

MessageType DetermineMessageType(ISteamNetworkingMessage* pMessage)
{
	const char* message_data = (const char *)pMessage->m_pData;
	MessageType type = (MessageType)message_data[0];
	return type;
}

void* RemoveFirstByte(void **Destination, const void *pData, uint32 cbData)
{
	*Destination = (void*) ::operator new (cbData - 1);
	memcpy(*Destination, (uint8*)pData + 1, cbData - 1);
	return *Destination;
}

std::string ReceiveString(const void *pData, uint32 cbData)
{
	std::string res;
	void *temp_str = nullptr;
	RemoveFirstByte(&temp_str, pData, cbData);
	res.assign((char*)temp_str, cbData - 1);
	delete temp_str;
	return res;
}

std::string ConvertMapToString(HL2DM_Map map)
{
	switch (map)
	{
	case dm_lockdown:
		return "dm_lockdown";
	case dm_overwatch:
		return "dm_overwatch";
	case dm_powerhouse:
		return "dm_powerhouse";
	case dm_resistance:
		return "dm_resistance";
	case dm_runoff:
		return "dm_runoff";
	case dm_steamlab:
		return "dm_steamlab";
	case dm_underpass:
		return "dm_underpass";
	case halls3:
		return "halls3";
	case invalid_map:
		return "<no map set>";
	default:
		return "";
	}
}
