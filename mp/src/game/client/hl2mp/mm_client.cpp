//========= Copyright Buster Bunny, All rights reserved. ============//
//
// Purpose:		Matchmaking Client
//
//=============================================================================//

#include "cbase.h"
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <convar.h>
#include "tier0/valve_minmax_off.h"
#include <queue>
#include "tier0/valve_minmax_on.h"
#include <cctype>
#include <algorithm>
#include "../../mm_server/mm_shared.h"

#define STEAMNETWORKINGSOCKETS_OPENSOURCE

#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

const uint16 DEFAULT_SERVER_PORT = 27055;

ConVar mm_auto_start_game("mm_auto_start_game", "1", FCVAR_CLIENTDLL | FCVAR_ARCHIVE);

/////////////////////////////////////////////////////////////////////////////
//
// Common stuff
//
/////////////////////////////////////////////////////////////////////////////

SteamNetworkingMicroseconds g_logTimeZero;

/// Handle used to identify a lobby.
typedef uint32 HLobbyID;

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
// Non-blocking console user input.  Sort of.
// Why is this so hard?
//
/////////////////////////////////////////////////////////////////////////////

std::queue< std::string > queueUserInput;

// You really gotta wonder what kind of pedantic garbage was
// going through the minds of people who designed std::string
// that they decided not to include trim.
// https://stackoverflow.com/questions/216823/whats-the-best-way-to-trim-stdstring

// trim from start (in place)
static inline void ltrim(std::string &s) {
	s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](int ch) {
		return !std::isspace(ch);
	}));
}

// trim from end (in place)
static inline void rtrim(std::string &s) {
	s.erase(std::find_if(s.rbegin(), s.rend(), [](int ch) {
		return !std::isspace(ch);
	}).base(), s.end());
}


// Read the next line of input from stdin, if anything is available.
bool LocalUserInput_GetNext(std::string &result)
{
	bool got_input = false;
	while (!queueUserInput.empty() && !got_input)
	{
		result = queueUserInput.front();
		queueUserInput.pop();
		ltrim(result);
		rtrim(result);
		got_input = !result.empty(); // ignore blank lines
	}
	return got_input;
}

/////////////////////////////////////////////////////////////////////////////
//
// ChatClient
//
/////////////////////////////////////////////////////////////////////////////
void SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t *pInfo);

class ChatClientThread : public CThread
{
public:
	ChatClientThread()
	{
		SetName("ChatClientThread");
        m_hCurrentLobby = invalid_lobby;
        m_bQuit = false;
	}

	~ChatClientThread()
	{
	}

	bool CallThreadFunction(SteamNetworkingIPAddr serverAddr)
	{
		m_pServerAddr = serverAddr;

		return true;
	}

	int Run()
	{
		// Reset some variables
		SteamNetworkingIPAddr serverAddr = m_pServerAddr;

		//Reply(1);
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
		Msg("Connecting to matchmaking server at %s\n", szAddr);
		SteamNetworkingConfigValue_t opt;
		opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (FnSteamNetConnectionStatusChanged)SteamNetConnectionStatusChangedCallback);
		//opt.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
		//opt.SetInt32(k_ESteamNetworkingConfig_Unencrypted, 3);
		
		m_hConnection = SteamNetworkingSockets()->ConnectByIPAddress(serverAddr, 1, &opt);
		if (m_hConnection == k_HSteamNetConnection_Invalid)
		{
			Warning("Failed to create connection\n");
			return;
		}

		while (!m_bQuit)
		{
			// We renew the pointer to the interface at the start of every loop to catch
			// that the interface is not available anymore and prevent the game crash 
			// when we exit with active connection
			m_pInterface = SteamNetworkingSockets();
			if (!m_pInterface)
			{
				Warning("No interface to GameNetworkingSockets\n");
				m_bQuit = true;
				break;
			}
			ClientUpdate();
			PollIncomingMessages();
			RunCallBacks();
			PollLocalUserInput();
			Sleep(10);
		}
	}
private:

	HSteamNetConnection m_hConnection;
	ISteamNetworkingSockets *m_pInterface;
	SteamNetworkingIPAddr m_pServerAddr;
	HLobbyID m_hCurrentLobby;
	std::string s_GameServerIP;
	bool m_bQuit;

	void LeaveLobby(HSteamNetConnection hConnection)
	{
		if (m_hCurrentLobby != invalid_lobby)
		{
			SendOnlyMessageType(hConnection, k_nSteamNetworkingSend_Reliable, nullptr, request_leave_lobby, m_pInterface);
			Msg("Left lobby: %u\n", m_hCurrentLobby);
			m_hCurrentLobby = invalid_lobby;
		}
		else
		{
			Msg("Can't leave lobby, currently not in one!\n");
		}
	}

	void ClientUpdate()
	{
		while (!m_bQuit)
		{
			if (mm_auto_start_game.GetBool() && !s_GameServerIP.empty())
			{
				std::string connect_commad = "connect ";
				connect_commad.append(s_GameServerIP);
				engine->ClientCmd(connect_commad.c_str());
				LeaveLobby(m_hConnection);
				s_GameServerIP.clear();
			}
			else
			{
				break;
			}
		}
	}

	void PollIncomingMessages()
	{
		while (!m_bQuit)
		{
			ISteamNetworkingMessage *pIncomingMsg = nullptr;
			int numMsgs = m_pInterface->ReceiveMessagesOnConnection(m_hConnection, &pIncomingMsg, 1);
			if (numMsgs == 0 || !pIncomingMsg)
				break;
			if (numMsgs < 0)
			{
				Warning("Error checking for messages\n");
				m_bQuit = true;
				break;
			}

			if (DetermineMessageType(pIncomingMsg) == chat_message)
			{
				void* temp_str;
				RemoveFirstByte(&temp_str, pIncomingMsg->m_pData, pIncomingMsg->m_cbSize);
				// Just echo anything we get from the server
				Msg("%s", (char*)temp_str);
				delete temp_str;
				Msg("\n");
			}
			if (DetermineMessageType(pIncomingMsg) == lobby_list)
			{
				void* temp_array_LobbyIDs;
				RemoveFirstByte(&temp_array_LobbyIDs, pIncomingMsg->m_pData, pIncomingMsg->m_cbSize);
				HLobbyID* array_LobbyIDs = (HLobbyID*)temp_array_LobbyIDs;
				SendTypedMessage(m_hConnection, &array_LobbyIDs[0], sizeof(HLobbyID), k_nSteamNetworkingSend_Reliable, nullptr, request_join_lobby, m_pInterface);
				delete temp_array_LobbyIDs;
			}
			if (DetermineMessageType(pIncomingMsg) == message_no_suitable_lobbies)
			{
				SendOnlyMessageType(m_hConnection, k_nSteamNetworkingSend_Reliable, nullptr, request_create_lobby, m_pInterface);
			}
			if (DetermineMessageType(pIncomingMsg) == message_save_lobby_id)
			{
				void* temp_lobbyid;
				RemoveFirstByte(&temp_lobbyid, pIncomingMsg->m_pData, pIncomingMsg->m_cbSize);
				m_hCurrentLobby = *(HLobbyID*)temp_lobbyid;
				delete temp_lobbyid;
				Msg("Joined lobby: %u\n", m_hCurrentLobby);
			}
			if (DetermineMessageType(pIncomingMsg) == message_echo)
			{
				void* temp_lobbyid;
				RemoveFirstByte(&temp_lobbyid, pIncomingMsg->m_pData, pIncomingMsg->m_cbSize);
				HLobbyID lobby_to_echo = *(HLobbyID*)temp_lobbyid;
				delete temp_lobbyid;
				Msg("Echoed: %u\n", lobby_to_echo);
			}
			if (DetermineMessageType(pIncomingMsg) == message_start_game)
			{
				void* temp_game_sip;
				RemoveFirstByte(&temp_game_sip, pIncomingMsg->m_pData, pIncomingMsg->m_cbSize);
				s_GameServerIP = (char*)temp_game_sip;
				Msg("Ready to start the match!\n");
                delete temp_game_sip;
			}

			// We don't need this anymore.
			pIncomingMsg->Release();
		}
	}

	void PollLocalUserInput()
	{
		std::string cmd;
		while (!m_bQuit && LocalUserInput_GetNext(cmd))
		{

			// Check for known commands
			if (strcmp(cmd.c_str(), "/quit") == 0)
			{
				m_bQuit = true;
				Msg("Disconnecting from matchmaking server\n");

				LeaveLobby(m_hConnection);
				// Close the connection gracefully.
				// We use linger mode to ask for any remaining reliable data
				// to be flushed out.  But remember this is an application
				// protocol on UDP.  See ShutdownSteamDatagramConnectionSockets
				m_pInterface->CloseConnection(m_hConnection, 0, "Goodbye\n", true);
				break;
			}
			if (strcmp(cmd.c_str(), "/find_game") == 0)
			{
				if (m_hCurrentLobby == invalid_lobby)
					SendOnlyMessageType(m_hConnection, k_nSteamNetworkingSend_Reliable, nullptr, request_lobby_list, m_pInterface);
				else
					Warning("Already in a lobby! LobbyID: %u\n", m_hCurrentLobby);
				break;
			}
			if (strcmp(cmd.c_str(), "/echo") == 0)
			{
				HLobbyID test = 432000;
				SendTypedMessage(m_hConnection, &test, sizeof(test), k_nSteamNetworkingSend_Reliable, nullptr, request_echo, m_pInterface);
				break;
			}
			if (strcmp(cmd.c_str(), "/leave_lobby") == 0)
			{
				LeaveLobby(m_hConnection);
				break;
			}
			if (strcmp(cmd.c_str(), "/start_game") == 0)
			{
				if (!s_GameServerIP.empty())
				{
					std::string connect_commad = "connect ";
					connect_commad.append(s_GameServerIP);
					engine->ClientCmd(connect_commad.c_str());
					LeaveLobby(m_hConnection);
					s_GameServerIP.clear();
				}
				else
				{
					Warning("Can't start the game! Haven't received game server IP from mm server\n");
				}
				break;
			}
			

			// Anything else, just send it to the server and let them parse it
			SendTypedMessage(m_hConnection, cmd.c_str(), (uint32)cmd.length(), k_nSteamNetworkingSend_Reliable, nullptr, chat_message, m_pInterface);
		}
	}

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
			LeaveLobby(pInfo->m_hConn);
			m_pInterface->CloseConnection(pInfo->m_hConn, 0, nullptr, false);
			break;
		}
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		{
			m_bQuit = true;

			// Print an appropriate message
			if (pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting)
			{
				// Note: we could distinguish between a timeout, a rejected connection,
				// or some other transport problem.
				Msg("We sought the remote host, yet our efforts were met with defeat.  (%s)\n", pInfo->m_info.m_szEndDebug);
			}
			else if (pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally)
			{
				Msg("Alas, troubles beset us; we have lost contact with the host.  (%s)\n", pInfo->m_info.m_szEndDebug);
			}
			else
			{
				// NOTE: We could check the reason code for a normal disconnection
				Msg("The host hath bidden us farewell.  (%s)\n", pInfo->m_info.m_szEndDebug);
			}

			LeaveLobby(pInfo->m_hConn);
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
			Msg("Connected to server OK\n");
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

	void OnExit() //reset all our globals and member variables
	{
		m_bQuit = false;
		while (!queueUserInput.empty())
		{
			queueUserInput.pop();
		}

		m_hConnection = k_HSteamNetConnection_Invalid;
		m_pInterface = nullptr;
		m_pServerAddr.Clear();

		CThread::OnExit();
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
	if (!SteamNetworkingSockets())
	{
		Warning("GameNetworkingSockets appears to not be initialised\n");
		return;
	}

	if (args.ArgC() < 1 || !args.Arg(1))
	{
		Msg("Usage: mm_connect SERVER_ADDR\n");
		return;
	}
	
	SteamNetworkingIPAddr addrServer; addrServer.Clear();

	if (addrServer.IsIPv6AllZeros())
	{
		if (!addrServer.ParseString(args.Arg(1)))
		{
			Warning("Invalid server address '%s'\n", args.Arg(1));
			return;
		}
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

void MM_ChatSay(const CCommand &args)
{
	queueUserInput.push(args.ArgS());
}

void MM_FindGame()
{
	queueUserInput.push("/find_game");
}

void MM_Echo()
{
	queueUserInput.push("/echo");
}

void MM_LeaveLobby()
{
	queueUserInput.push("/leave_lobby");
}

void MM_StartGame()
{
	queueUserInput.push("/start_game");
}

void MM_Disconnect()
{
	queueUserInput.push("/quit");
}

ConCommand mm_connect("mm_connect", MM_Connect, "Connect to a matchmaking server");
ConCommand mm_threadstop("mm_threadstop", MM_ThreadStop);
ConCommand mm_chatsay("mm_chatsay", MM_ChatSay, "Say something to a matchmaking chat");
ConCommand mm_find_game("mm_find_game", MM_FindGame, "Start searching for a match");
ConCommand mm_echo("mm_echo", MM_Echo, "For testing, echoes message back from matchmaking server");
ConCommand mm_leave_lobby("mm_leave_lobby", MM_LeaveLobby, "Leave matchmaking lobby");
ConCommand mm_start_game("mm_start_game", MM_StartGame, "Begin the match if it was found");
ConCommand mm_disconnect("mm_disconnect", MM_Disconnect, "Disconnect from a matchmaking server");
