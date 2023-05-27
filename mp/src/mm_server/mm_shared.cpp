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
