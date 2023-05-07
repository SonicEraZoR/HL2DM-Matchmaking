//========= Copyright Buster Bunny, All rights reserved. ============//
//
// Purpose:		Matchmaking Client
//
//=============================================================================//

#include "cbase.h"
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <convar.h>
#include <thread>

#define STEAMNETWORKINGSOCKETS_OPENSOURCE

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

const uint16 DEFAULT_SERVER_PORT = 27055;

/////////////////////////////////////////////////////////////////////////////
//
// Common stuff
//
/////////////////////////////////////////////////////////////////////////////

bool g_bQuit = false;

SteamNetworkingMicroseconds g_logTimeZero;

class ChatClientThread;

static void DebugOutput(ESteamNetworkingSocketsDebugOutputType eType, const char *pszMsg)
{
	SteamNetworkingMicroseconds time = SteamNetworkingUtils()->GetLocalTimestamp() - g_logTimeZero;
	Msg("%10.6f %s\n", time*1e-6, pszMsg);
}

//static void FatalError(const char *fmt, ...)
//{
//	char text[2048];
//	va_list ap;
//	va_start(ap, fmt);
//	vsprintf(text, fmt, ap);
//	va_end(ap);
//	char *nl = strchr(text, '\0') - 1;
//	if (nl >= text && *nl == '\n')
//		*nl = '\0';
//	DebugOutput(k_ESteamNetworkingSocketsDebugOutputType_Bug, text);
//}
//
//static void Printf(const char *fmt, ...)
//{
//	char text[2048];
//	va_list ap;
//	va_start(ap, fmt);
//	vsprintf(text, fmt, ap);
//	va_end(ap);
//	char *nl = strchr(text, '\0') - 1;
//	if (nl >= text && *nl == '\n')
//		*nl = '\0';
//	DebugOutput(k_ESteamNetworkingSocketsDebugOutputType_Msg, text);
//}

static void InitSteamDatagramConnectionSockets()
{
	g_logTimeZero = SteamNetworkingUtils()->GetLocalTimestamp();

	SteamNetworkingUtils()->SetDebugOutputFunction(k_ESteamNetworkingSocketsDebugOutputType_Msg, DebugOutput);
}

/////////////////////////////////////////////////////////////////////////////
//
// ChatClient
//
/////////////////////////////////////////////////////////////////////////////
class ChatClientThread : public CWorkerThread
{
public:
	ChatClientThread()
	{
		SetName("ChatClientThread");
	}

	~ChatClientThread()
	{
	}

	enum
	{
		CALL_FUNC,
		EXIT,
	};

	bool CallThreadFunction(SteamNetworkingIPAddr serverAddr)
	{
		m_pServerAddr = serverAddr;
		//CallWorker(CALL_FUNC);

		return true;
	}

	int Run()
	{
		// Reset some variables
		SteamNetworkingIPAddr serverAddr = m_pServerAddr;

		Reply(1);
		RunClient(serverAddr);
		return 0;
	}

	void RunClient(const SteamNetworkingIPAddr &serverAddr)
	{
		// Select instance to use.  For now we'll always use the default.
		m_pInterface = SteamNetworkingSockets();

		// Start connecting
		char szAddr[SteamNetworkingIPAddr::k_cchMaxString];
		serverAddr.ToString(szAddr, sizeof(szAddr), true);
		Msg("Connecting to chat server at %s", szAddr);
		SteamNetworkingConfigValue_t opt;
		opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (FnSteamNetConnectionStatusChanged)SteamNetConnectionStatusChangedCallback);
		//opt.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
		//opt.SetInt32(k_ESteamNetworkingConfig_Unencrypted, 3);
		
		m_hConnection = m_pInterface->ConnectByIPAddress(serverAddr, 1, &opt);
		if (m_hConnection == k_HSteamNetConnection_Invalid)
			Warning("Failed to create connection");

		while (!g_bQuit)
		{
			PollIncomingMessages();
			RunCallBacks();
			//PollLocalUserInput();
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
	}
private:

	HSteamNetConnection m_hConnection;
	ISteamNetworkingSockets *m_pInterface;
	SteamNetworkingIPAddr m_pServerAddr;

	void PollIncomingMessages()
	{
		while (!g_bQuit)
		{
			ISteamNetworkingMessage *pIncomingMsg = nullptr;
			int numMsgs = m_pInterface->ReceiveMessagesOnConnection(m_hConnection, &pIncomingMsg, 1);
			if (numMsgs == 0 || !pIncomingMsg)
				break;
			if (numMsgs < 0)
				Warning("Error checking for messages");

			// Just echo anything we get from the server
			Msg((char*)pIncomingMsg->m_pData);
			Msg("\n");

			// We don't need this anymore.
			pIncomingMsg->Release();
		}
	}

	//void PollLocalUserInput()
	//{
	//	std::string cmd;
	//	while (!g_bQuit)
	//	{

	//		// Check for known commands
	//		if (strcmp(cmd.c_str(), "/quit") == 0)
	//		{
	//			g_bQuit = true;
	//			Msg("Disconnecting from chat server");

	//			// Close the connection gracefully.
	//			// We use linger mode to ask for any remaining reliable data
	//			// to be flushed out.  But remember this is an application
	//			// protocol on UDP.  See ShutdownSteamDatagramConnectionSockets
	//			m_pInterface->CloseConnection(m_hConnection, 0, "Goodbye", true);
	//			break;
	//		}

	//		// Anything else, just send it to the server and let them parse it
	//		m_pInterface->SendMessageToConnection(m_hConnection, cmd.c_str(), (uint32)cmd.length(), k_nSteamNetworkingSend_Reliable, nullptr);
	//	}
	//}

	void OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t *pInfo)
	{
		assert(pInfo->m_hConn == m_hConnection || m_hConnection == k_HSteamNetConnection_Invalid);

		// What's the state of the connection?
		switch (pInfo->m_info.m_eState)
		{
		case k_ESteamNetworkingConnectionState_None:
			// NOTE: We will get callbacks here when we destroy connections.  You can ignore these.
			break;

		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		{
			m_pInterface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
			break;
		}
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		{
			g_bQuit = true;

			// Print an appropriate message
			if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting)
			{
				// Note: we could distinguish between a timeout, a rejected connection,
				// or some other transport problem.
				Msg("We sought the remote host, yet our efforts were met with defeat.  (%s)", pInfo->m_info.m_szEndDebug);
			}
			else if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
			{
				Msg("Alas, troubles beset us; we have lost contact with the host.  (%s)", pInfo->m_info.m_szEndDebug);
			}
			else
			{
				// NOTE: We could check the reason code for a normal disconnection
				Msg("The host hath bidden us farewell.  (%s)", pInfo->m_info.m_szEndDebug);
			}

			// Clean up the connection.  This is important!
			// The connection is "closed" in the network sense, but
			// it has not been destroyed.  We must close it on our end, too
			// to finish up.  The reason information do not matter in this case,
			// and we cannot linger because it's already closed on the other end,
			// so we just pass 0's.
			m_pInterface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
			m_hConnection = k_HSteamNetConnection_Invalid;
			break;
		}

		case k_ESteamNetworkingConnectionState_Connecting:
			// We will get this callback when we start connecting.
			// We can ignore this.
			break;

		case k_ESteamNetworkingConnectionState_Connected:
			Msg("Connected to server OK");
			break;

		default:
			// Silences -Wswitch
			break;
		}
	}

	void RunCallBacks()
	{
		m_pInterface->RunCallbacks();
	}

	friend void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t *pInfo);
};

static ChatClientThread g_ChatClientThread;

void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t *pInfo)
{
	g_ChatClientThread.OnSteamNetConnectionStatusChanged(pInfo);
}

void MM_Connect(const CCommand &args)
{
	if (args.ArgC() < 1 || !args.Arg(1))
	{
		Msg("Usage: mm_connect SERVER_ADDR\n");
		return;
	}
	
	SteamNetworkingIPAddr addrServer; addrServer.Clear();

	if (addrServer.IsIPv6AllZeros())
	{
		if (!addrServer.ParseString(args.Arg(1)))
			Warning("Invalid server address '%s'\n", args.Arg(1));
		if (!V_stricmp(args.Arg(3), ""))
			addrServer.m_port = DEFAULT_SERVER_PORT;
		else
			addrServer.m_port = strtol(args.Arg(3), NULL, 0);
	}

	if (addrServer.IsIPv6AllZeros())
	{
		Msg("Usage: mm_connect SERVER_ADDR\n");
		return;
	}

	// Create client and server sockets
	InitSteamDatagramConnectionSockets();

	g_ChatClientThread.CallThreadFunction(addrServer);

	if (!g_ChatClientThread.IsAlive())
		g_ChatClientThread.Start();

	if (!g_ChatClientThread.IsAlive())
		Warning("MM_Connect() failed to start the thread!\n");

	//g_ChatClientThread.CallWorker(ChatClientThread::EXIT);
}

void MM_ThreadStop()
{
	if (g_ChatClientThread.IsAlive())
	{
		g_ChatClientThread.Stop();
		Msg("MM_ThreadStop() attemting to stop the thread....\n");
	}


	if (g_ChatClientThread.IsAlive())
		Warning("MM_ThreadStop() failed to stop the thread!\n");
	else
		Msg("MM_ThreadStop() thread stopped successfully\n");
}

ConCommand mm_connect("mm_connect", MM_Connect, "Connect to a matchmaking server");
ConCommand mm_threadstop("mm_threadstop", MM_ThreadStop);
