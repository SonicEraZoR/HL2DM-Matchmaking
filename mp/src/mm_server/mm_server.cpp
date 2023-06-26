//========= Copyright Buster Bunny, All rights reserved. ============//
//
// Purpose:		Matchmaking Server
//
//=============================================================================//

#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <random>
#include <chrono>
#include <thread>
#include <mutex>
#include <queue>
#include <map>
#include <cctype>
#include <sqlite3.h>
#include <srcon.h>
#include "mm_shared.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#ifndef STEAMNETWORKINGSOCKETS_OPENSOURCE
#include <steam/steam_api.h>
#endif

#ifdef _WIN32
	#include <windows.h> // Ug, for NukeProcess -- see below
#else
	#include <unistd.h>
	#include <signal.h>
#endif

/////////////////////////////////////////////////////////////////////////////
//
// Common stuff
//
/////////////////////////////////////////////////////////////////////////////

extern int iNumOfPlayersToStartGame;

bool g_bQuit = false;

SteamNetworkingMicroseconds g_logTimeZero;

// We do this because I won't want to figure out how to cleanly shut
// down the thread that is reading from stdin.
static void NukeProcess( int rc )
{
	#ifdef _WIN32
		ExitProcess( rc );
	#else
		(void)rc; // Unused formal parameter
		kill( getpid(), SIGKILL );
	#endif
}

static void DebugOutput( ESteamNetworkingSocketsDebugOutputType eType, const char *pszMsg )
{
	SteamNetworkingMicroseconds time = SteamNetworkingUtils()->GetLocalTimestamp() - g_logTimeZero;
	printf( "%10.6f %s\n", time*1e-6, pszMsg );
	fflush(stdout);
	if ( eType == k_ESteamNetworkingSocketsDebugOutputType_Bug )
	{
		fflush(stdout);
		fflush(stderr);
		NukeProcess(1);
	}
}

static void FatalError( const char *fmt, ... )
{
	char text[ 2048 ];
	va_list ap;
	va_start( ap, fmt );
	vsprintf( text, fmt, ap );
	va_end(ap);
	char *nl = strchr( text, '\0' ) - 1;
	if ( nl >= text && *nl == '\n' )
		*nl = '\0';
	DebugOutput( k_ESteamNetworkingSocketsDebugOutputType_Bug, text );
}

static void Printf( const char *fmt, ... )
{
	char text[ 2048 ];
	va_list ap;
	va_start( ap, fmt );
	vsprintf( text, fmt, ap );
	va_end(ap);
	char *nl = strchr( text, '\0' ) - 1;
	if ( nl >= text && *nl == '\n' )
		*nl = '\0';
	DebugOutput( k_ESteamNetworkingSocketsDebugOutputType_Msg, text );
}

static void InitSteamDatagramConnectionSockets()
{
	#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
		SteamDatagramErrMsg errMsg;
		if ( !GameNetworkingSockets_Init( nullptr, errMsg ) )
			FatalError( "GameNetworkingSockets_Init failed.  %s", errMsg );
	#else
		SteamDatagram_SetAppID( 570 ); // Just set something, doesn't matter what
		SteamDatagram_SetUniverse( false, k_EUniverseDev );

		SteamDatagramErrMsg errMsg;
		if ( !SteamDatagramClient_Init( errMsg ) )
			FatalError( "SteamDatagramClient_Init failed.  %s", errMsg );

		// Disable authentication when running with Steam, for this
		// example, since we're not a real app.
		//
		// Authentication is disabled automatically in the open-source
		// version since we don't have a trusted third party to issue
		// certs.
		SteamNetworkingUtils()->SetGlobalConfigValueInt32( k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1 );
	#endif

	g_logTimeZero = SteamNetworkingUtils()->GetLocalTimestamp();

	SteamNetworkingUtils()->SetDebugOutputFunction( k_ESteamNetworkingSocketsDebugOutputType_Msg, DebugOutput );
}

static void ShutdownSteamDatagramConnectionSockets()
{
	// Give connections time to finish up.  This is an application layer protocol
	// here, it's not TCP.  Note that if you have an application and you need to be
	// more sure about cleanup, you won't be able to do this.  You will need to send
	// a message and then either wait for the peer to close the connection, or
	// you can pool the connection to see if any reliable data is pending.
	std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );

	#ifdef STEAMNETWORKINGSOCKETS_OPENSOURCE
		GameNetworkingSockets_Kill();
	#else
		SteamDatagramClient_Kill();
	#endif
}

/////////////////////////////////////////////////////////////////////////////
//
// Non-blocking console user input.  Sort of.
// Why is this so hard?
//
/////////////////////////////////////////////////////////////////////////////

std::mutex mutexUserInputQueue;
std::queue< std::string > queueUserInput;

std::thread *s_pThreadUserInput = nullptr;

void LocalUserInput_Init()
{
	s_pThreadUserInput = new std::thread( []()
	{
		while ( !g_bQuit )
		{
			char szLine[ 4000 ];
			if ( !fgets( szLine, sizeof(szLine), stdin ) )
			{
				// Well, you would hope that you could close the handle
				// from the other thread to trigger this.  Nope.
				if ( g_bQuit )
					return;
				g_bQuit = true;
				Printf( "Failed to read on stdin, quitting\n" );
				break;
			}

			mutexUserInputQueue.lock();
			queueUserInput.push( std::string( szLine ) );
			mutexUserInputQueue.unlock();
		}
	} );
}

void LocalUserInput_Kill()
{
// Does not work.  We won't clean up, we'll just nuke the process.
//	g_bQuit = true;
//	_close( fileno( stdin ) );
//
//	if ( s_pThreadUserInput )
//	{
//		s_pThreadUserInput->join();
//		delete s_pThreadUserInput;
//		s_pThreadUserInput = nullptr;
//	}
}

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
bool LocalUserInput_GetNext( std::string &result )
{
	bool got_input = false;
	mutexUserInputQueue.lock();
	while ( !queueUserInput.empty() && !got_input )
	{
		result = queueUserInput.front();
		queueUserInput.pop();
		ltrim(result);
		rtrim(result);
		got_input = !result.empty(); // ignore blank lines
	}
	mutexUserInputQueue.unlock();
	return got_input;
}

/////////////////////////////////////////////////////////////////////////////
//
// ChatServer
//
/////////////////////////////////////////////////////////////////////////////
class ChatServer
{
public:
	void Run( uint16 nPort )
	{
		// Select instance to use.  For now we'll always use the default.
		// But we could use SteamGameServerNetworkingSockets() on Steam.
		m_pInterface = SteamNetworkingSockets();

		// Start listening
		SteamNetworkingIPAddr serverLocalAddr;
		serverLocalAddr.Clear();
		serverLocalAddr.m_port = nPort;
		SteamNetworkingConfigValue_t opt;
		opt.SetPtr( k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)SteamNetConnectionStatusChangedCallback );
		//opt.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
		//opt.SetInt32(k_ESteamNetworkingConfig_Unencrypted, 3);
		m_hListenSock = m_pInterface->CreateListenSocketIP( serverLocalAddr, 1, &opt );
		if ( m_hListenSock == k_HSteamListenSocket_Invalid )
			FatalError( "Failed to listen on port %d", nPort );
		m_hPollGroup = m_pInterface->CreatePollGroup();
		if ( m_hPollGroup == k_HSteamNetPollGroup_Invalid )
			FatalError( "Failed to listen on port %d", nPort );
		Printf( "Server listening on port %d\n", nPort );

		while ( !g_bQuit )
		{
			ServerUpdate();
			PollIncomingMessages();
			RunCallBacks();
			PollLocalUserInput();
			std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
		}

		// Close all the connections
		Printf( "Closing connections...\n" );
		for ( auto it: m_mapClients )
		{
			// Send them one more goodbye message.  Note that we also have the
			// connection close reason as a place to send final data.  However,
			// that's usually best left for more diagnostic/debug text not actual
			// protocol strings.
			SendStringToClient( it.first, "Server is shutting down.  Goodbye." );

			// Close the connection.  We use "linger mode" to ask SteamNetworkingSockets
			// to flush this out and close gracefully.
			m_pInterface->CloseConnection( it.first, 0, "Server Shutdown", true );
		}
		for (std::map<HLobbyID, Lobby>::iterator it = m_mapLobbies.begin(); it != m_mapLobbies.end(); ++it)
		{
			it->second.m_mapPlayers.clear();
		}
		m_mapLobbies.clear();
		m_mapClients.clear();

		m_pInterface->CloseListenSocket( m_hListenSock );
		m_hListenSock = k_HSteamListenSocket_Invalid;

		m_pInterface->DestroyPollGroup( m_hPollGroup );
		m_hPollGroup = k_HSteamNetPollGroup_Invalid;
	}
private:

	HSteamListenSocket m_hListenSock;
	HSteamNetPollGroup m_hPollGroup;
	ISteamNetworkingSockets *m_pInterface;

	std::map< HSteamNetConnection, Client_t > m_mapClients;
	std::map< HLobbyID, Lobby > m_mapLobbies;
	std::vector< srcon_addr > m_vGameServers;

	void PrintLobbyList()
	{
		Printf("Current lobby list:\n");
		for (std::map<HLobbyID, Lobby>::iterator it = m_mapLobbies.begin(); it != m_mapLobbies.end(); ++it)
		{
			Printf("LobbyID: %u\n", it->first);
			for (std::map<HSteamNetConnection, Player>::iterator it2 = it->second.m_mapPlayers.begin(); it2 != it->second.m_mapPlayers.end(); ++it2)
			{
				Printf("Player: %u, %s\n", it2->first, it2->second.m_Client.m_sNick.c_str());
			}
			Printf("Current map: %s\n", ConvertMapToString(it->second.m_map).c_str());
			Printf("Team deathmatch: %i\n", (int)it->second.m_bTeamDM);
		}
	}

	void PrintLobby(HLobbyID lobbyID)
	{
		Printf("LobbyID: %u\n", lobbyID);
		for (std::map<HSteamNetConnection, Player>::iterator it2 = m_mapLobbies[lobbyID].m_mapPlayers.begin(); it2 != m_mapLobbies[lobbyID].m_mapPlayers.end(); ++it2)
		{
			Printf("Player: %u, %s\n", it2->first, it2->second.m_Client.m_sNick.c_str());
		}
		Printf("Current map: %s\n", ConvertMapToString(m_mapLobbies[lobbyID].m_map).c_str());
		Printf("Team deathmatch: %i\n", (int)m_mapLobbies[lobbyID].m_bTeamDM);
	}

	void RemovePlayerFromLobby(HSteamNetConnection conn)
	{
		for (std::map<HLobbyID, Lobby>::iterator it = m_mapLobbies.begin(); it != m_mapLobbies.end(); ++it)
		{
			std::string temp_nick = it->second.m_mapPlayers[conn].m_Client.m_sNick;
			if (it->second.m_mapPlayers.erase(conn))
			{
				Printf("PLAYER %s LEFT LOBBY: %u\n", temp_nick.c_str(), it->first);
				if (it->second.m_mapPlayers.empty())
				{
					Printf("DESTROYED LOBBY %u SINCE IT WAS EMPTY\n", it->first);
					m_mapLobbies.erase(it);
				}
				break;
			}
		}
		PrintLobbyList();
	}

	void SendStringToClient( HSteamNetConnection conn, const char *str )
	{
		SendTypedMessage(conn, str, (uint32)strlen(str), k_nSteamNetworkingSend_Reliable, nullptr, chat_message, m_pInterface);
	}

	void SendStringToAllClients( const char *str, HSteamNetConnection except = k_HSteamNetConnection_Invalid )
	{
		for ( auto &c: m_mapClients )
		{
			if ( c.first != except )
				SendStringToClient( c.first, str );
		}
	}

	void ServerUpdate()
	{
		for (std::map<HLobbyID, Lobby>::iterator it = m_mapLobbies.begin(); it != m_mapLobbies.end(); ++it)
		{
			if (it->second.m_mapPlayers.size() == iNumOfPlayersToStartGame)
			{
				Printf("ENOUTH PLAYERS TO START THE GAME IN A LOBBY: %u\n", it->first);
				
				srcon_addr addr_struct;
				addr_struct.addr = "127.0.1.1";
				addr_struct.pass = "123";
				addr_struct.port = 27015;
				srcon rcon_game_s = srcon(addr_struct);
				
				std::string set_tdm = "mp_teamplay ";
				set_tdm.append(std::to_string(it->second.m_bTeamDM));
				std::string tdm_re = rcon_game_s.send(set_tdm);
				Printf("SET TDM RESPONSE: %s\n", tdm_re.c_str());
				
				std::string change_level = "changelevel ";
				change_level.append(ConvertMapToString(it->second.m_map));
				std::string change_level_re = rcon_game_s.send(change_level);
				Printf("CHANGE LEVEL RESPONSE: %s\n", change_level_re.c_str());
				
				std::string game_sip = m_vGameServers.front().addr;
				game_sip.append(":");
				game_sip.append(std::to_string(m_vGameServers.front().port));
				
				HSteamNetConnection temp_id = it->second.m_mapPlayers.begin()->first;
				while (!it->second.m_mapPlayers.empty())
				{
					SendTypedMessage(temp_id, game_sip.c_str(), (uint32)strlen(game_sip.c_str()), k_nSteamNetworkingSend_Reliable, nullptr, message_start_game, m_pInterface);
					Printf("PLAYER %s LEFT LOBBY: %u\n", it->second.m_mapPlayers[temp_id].m_Client.m_sNick.c_str(), it->first);
					it->second.m_mapPlayers.erase(temp_id);
					temp_id = it->second.m_mapPlayers.begin()->first;
				}
				Printf("DESTROYED LOBBY %u SINCE IT WAS EMPTY\n", it->first);
				m_mapLobbies.erase(it->first);
				PrintLobbyList();
			}
			if (m_mapLobbies.empty())
				break;
		}
	}

	void PollIncomingMessages()
	{
		char temp[ 1024 ];

		while ( !g_bQuit )
		{
			ISteamNetworkingMessage *pIncomingMsg = nullptr;
			int numMsgs = m_pInterface->ReceiveMessagesOnPollGroup( m_hPollGroup, &pIncomingMsg, 1 );
			if ( numMsgs == 0 )
				break;
			if ( numMsgs < 0 )
				FatalError( "Error checking for messages" );
			assert( numMsgs == 1 && pIncomingMsg );
			auto itClient = m_mapClients.find( pIncomingMsg->m_conn );
			assert( itClient != m_mapClients.end() );

			std::string sCmd;
			const char *cmd;
			if (DetermineMessageType(pIncomingMsg) == chat_message)
			{
				// '\0'-terminate it to make it easier to parse
				void *temp_str = nullptr;
				RemoveFirstByte(&temp_str, pIncomingMsg->m_pData, pIncomingMsg->m_cbSize);
				sCmd.assign((char*)temp_str, pIncomingMsg->m_cbSize - 1);
				delete temp_str;
				cmd = sCmd.c_str();

				// Check for known commands.  None of this example code is secure or robust.
				// Don't write a real server like this, please.

				if (strncmp(cmd, "/nick", 5) == 0)
				{
					const char *nick = cmd + 5;
					while (isspace(*nick))
						++nick;

					// Let everybody else know they changed their name
					sprintf(temp, "%s shall henceforth be known as %s", itClient->second.m_sNick.c_str(), nick);
					SendStringToAllClients(temp, itClient->first);

					// Respond to client
					sprintf(temp, "Ye shall henceforth be known as %s", nick);
					SendStringToClient(itClient->first, temp);

					// Actually change their name
					SetClientNick(itClient->first, nick);
					continue;
				}

				// Assume it's just a ordinary chat message, dispatch to everybody else
				sprintf(temp, "%s: %s", itClient->second.m_sNick.c_str(), cmd);
				SendStringToAllClients(temp, itClient->first);
			}
			if (DetermineMessageType(pIncomingMsg) == request_lobby_list)
			{
				if (m_mapLobbies.empty())
				{
					SendOnlyMessageType(pIncomingMsg->m_conn, k_nSteamNetworkingSend_Reliable, nullptr, message_no_suitable_lobbies, m_pInterface);
				}
				else
				{
					HLobbyID* array_LobbyIDs = new HLobbyID[m_mapLobbies.size()];
					int i = 0;
					for (std::map<HLobbyID, Lobby>::iterator it = m_mapLobbies.begin(); it != m_mapLobbies.end(); ++it)
					{
						array_LobbyIDs[i] = it->first;
						i++;
					}
					SendTypedMessage(pIncomingMsg->m_conn, array_LobbyIDs, (uint32)(sizeof(HLobbyID)*m_mapLobbies.size()), k_nSteamNetworkingSend_Reliable, nullptr, lobby_list, m_pInterface);
					delete[] array_LobbyIDs;
				}
			}
			if (DetermineMessageType(pIncomingMsg) == request_create_lobby)
			{
				Lobby temp_lobby;
				Player temp_player;
				temp_player.m_Client = m_mapClients.find(pIncomingMsg->m_conn)->second;
				temp_lobby.m_mapPlayers.insert(std::pair<HSteamNetConnection, Player>(pIncomingMsg->m_conn, temp_player));
				srand((unsigned int)time(0));
				HLobbyID temp_id = rand();
				m_mapLobbies.insert(std::pair<HLobbyID, Lobby>(temp_id, temp_lobby));
				SendTypedMessage(pIncomingMsg->m_conn, &temp_id, sizeof(temp_id), k_nSteamNetworkingSend_Reliable, nullptr, message_save_lobby_id_on_create, m_pInterface);
				Printf("LOBBY %u CREATED\n", temp_id);
				Printf("HOST %s JOINED LOBBY\n", m_mapClients[pIncomingMsg->m_conn].m_sNick.c_str());
				PrintLobby(temp_id);
			}
			if (DetermineMessageType(pIncomingMsg) == request_join_lobby)
			{
				void* temp_lobbyid;
				RemoveFirstByte(&temp_lobbyid, pIncomingMsg->m_pData, pIncomingMsg->m_cbSize);
				HLobbyID lobby_to_join = *(HLobbyID*)temp_lobbyid;
				delete temp_lobbyid;
				Player temp_player;
				temp_player.m_Client = m_mapClients.find(pIncomingMsg->m_conn)->second;
				m_mapLobbies[lobby_to_join].m_mapPlayers.insert(std::pair<HSteamNetConnection, Player>(pIncomingMsg->m_conn, temp_player));
				SendTypedMessage(pIncomingMsg->m_conn, &lobby_to_join, sizeof(lobby_to_join), k_nSteamNetworkingSend_Reliable, nullptr, message_save_lobby_id, m_pInterface);
				Printf("PLAYER %s JOINED LOBBY\n", m_mapClients[pIncomingMsg->m_conn].m_sNick.c_str());
				PrintLobby(lobby_to_join);
			}
			if (DetermineMessageType(pIncomingMsg) == request_echo)
			{
				void* temp_lobbyid;
				RemoveFirstByte(&temp_lobbyid, pIncomingMsg->m_pData, pIncomingMsg->m_cbSize);
				HLobbyID lobby_to_join = *(HLobbyID*)temp_lobbyid;
				delete temp_lobbyid;
				SendTypedMessage(pIncomingMsg->m_conn, &lobby_to_join, sizeof(lobby_to_join), k_nSteamNetworkingSend_Reliable, nullptr, message_echo, m_pInterface);
				Printf("Echoed: %u\n", lobby_to_join);
			}
			if (DetermineMessageType(pIncomingMsg) == request_leave_lobby)
			{
				RemovePlayerFromLobby(pIncomingMsg->m_conn);
			}
			if (DetermineMessageType(pIncomingMsg) == request_lobby_data)
			{
				void* temp_lobbyid;
				RemoveFirstByte(&temp_lobbyid, pIncomingMsg->m_pData, pIncomingMsg->m_cbSize);
				HLobbyID lobby_id = *(HLobbyID*)temp_lobbyid;
				delete temp_lobbyid;
				LobbyData l_lobby_data;
				l_lobby_data.m_hLobbyID = lobby_id;
				l_lobby_data.m_bTeamDM = m_mapLobbies[lobby_id].m_bTeamDM;
				l_lobby_data.m_map = m_mapLobbies[lobby_id].m_map;
				SendTypedMessage(pIncomingMsg->m_conn, &l_lobby_data, sizeof(l_lobby_data), k_nSteamNetworkingSend_Reliable, nullptr, lobby_data, m_pInterface);
				Printf("LOBBY: %u METADATA WAS SENT\n", lobby_id);
			}
			if (DetermineMessageType(pIncomingMsg) == lobby_data)
			{
				void* temp_lobby_data;
				RemoveFirstByte(&temp_lobby_data, pIncomingMsg->m_pData, pIncomingMsg->m_cbSize);
				LobbyData l_lobby_data = *(LobbyData*)temp_lobby_data;
				delete temp_lobby_data;
				m_mapLobbies[l_lobby_data.m_hLobbyID].m_bTeamDM = l_lobby_data.m_bTeamDM;
				m_mapLobbies[l_lobby_data.m_hLobbyID].m_map = l_lobby_data.m_map;
				Printf("SET LOBBY: %u METADATA\n", l_lobby_data.m_hLobbyID);
				PrintLobby(l_lobby_data.m_hLobbyID);
			}
			
			// We don't need this anymore.
			pIncomingMsg->Release();
		}
	}

	void PollLocalUserInput()
	{
		std::string cmd;
		while ( !g_bQuit && LocalUserInput_GetNext( cmd ))
		{
			if ( strcmp( cmd.c_str(), "/quit" ) == 0 )
			{
				g_bQuit = true;
				Printf( "Shutting down server" );
				break;
			}
			if (strncmp(cmd.c_str(), "/num_s", 6) == 0)
			{
				const char *temp_num_s = cmd.c_str() + 6;
				iNumOfPlayersToStartGame = (int)strtol(temp_num_s, nullptr, 10);
				Printf("Number of players in a lobby requered for the game to start: %i\n", iNumOfPlayersToStartGame);
				break;
			}
			if (strncmp(cmd.c_str(), "/game_sip", 9) == 0)
			{
				const char *temp_game_sip = cmd.c_str() + 9;
				while (isspace(*temp_game_sip))
					++temp_game_sip;
				
				SteamNetworkingIPAddr addrServer; addrServer.Clear();
				
				if ( addrServer.IsIPv6AllZeros() )
				{
					if ( !addrServer.ParseString( temp_game_sip ) )
					{
						Printf( "Invalid GAME server address '%s'", temp_game_sip );
						break;
					}
					if ( addrServer.m_port == 0 )
						addrServer.m_port = 27015;
				}
				
				char szAddr[ SteamNetworkingIPAddr::k_cchMaxString ];
				addrServer.ToString( szAddr, sizeof(szAddr), false );
				srcon_addr addr_struct;
				addr_struct.addr = szAddr;
				addr_struct.pass = "123";
				addr_struct.port = addrServer.m_port;
				
				m_vGameServers.push_back(addr_struct);
				Printf("Game Server IP: %s:%i\n", m_vGameServers.front().addr.c_str(), m_vGameServers.front().port);
				break;
			}
			if (strncmp(cmd.c_str(), "/print_lobbies", 14) == 0)
			{
				PrintLobbyList();
				break;
			}

			// That's the only command we support
			Printf( "Possible commands:\n'/quit' (shutdown the server)\n'/num_s' (number of players in a lobby required to start the game)\n'/game_sip' (IP of the game server)\n'/print_lobbies' (print all of the lobbies)" );
		}
	}

	void SetClientNick( HSteamNetConnection hConn, const char *nick )
	{

		// Remember their nick
		m_mapClients[hConn].m_sNick = nick;

		// Set the connection name, too, which is useful for debugging
		m_pInterface->SetConnectionName( hConn, nick );
	}

	void OnSteamNetConnectionStatusChanged( SteamNetConnectionStatusChangedCallback_t *pInfo )
	{
		char temp[1024];

		// What's the state of the connection?
		switch ( pInfo->m_info.m_eState )
		{
			case k_ESteamNetworkingConnectionState_None:
				// NOTE: We will get callbacks here when we destroy connections.  You can ignore these.
				break;

			case k_ESteamNetworkingConnectionState_ClosedByPeer:
			case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			{
				// Ignore if they were not previously connected.  (If they disconnected
				// before we accepted the connection.)
				if ( pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connected )
				{

					// Locate the client.  Note that it should have been found, because this
					// is the only codepath where we remove clients (except on shutdown),
					// and connection change callbacks are dispatched in queue order.
					auto itClient = m_mapClients.find( pInfo->m_hConn );
					assert( itClient != m_mapClients.end() );

					// Select appropriate log messages
					const char *pszDebugLogAction;
					if ( pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally )
					{
						pszDebugLogAction = "problem detected locally";
						sprintf( temp, "Alas, %s hath fallen into shadow.  (%s)", itClient->second.m_sNick.c_str(), pInfo->m_info.m_szEndDebug );
					}
					else
					{
						// Note that here we could check the reason code to see if
						// it was a "usual" connection or an "unusual" one.
						pszDebugLogAction = "closed by peer";
						sprintf( temp, "%s hath departed", itClient->second.m_sNick.c_str() );
					}

					// Spew something to our own log.  Note that because we put their nick
					// as the connection description, it will show up, along with their
					// transport-specific data (e.g. their IP address)
					Printf( "Connection %s %s, reason %d: %s\n",
						pInfo->m_info.m_szConnectionDescription,
						pszDebugLogAction,
						pInfo->m_info.m_eEndReason,
						pInfo->m_info.m_szEndDebug
					);

					RemovePlayerFromLobby(pInfo->m_hConn);
					m_mapClients.erase( itClient );

					// Send a message so everybody else knows what happened
					SendStringToAllClients( temp );
				}
				else
				{
					assert( pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting );
				}

				// Clean up the connection.  This is important!
				// The connection is "closed" in the network sense, but
				// it has not been destroyed.  We must close it on our end, too
				// to finish up.  The reason information do not matter in this case,
				// and we cannot linger because it's already closed on the other end,
				// so we just pass 0's.
				m_pInterface->CloseConnection( pInfo->m_hConn, 0, nullptr, false );
				break;
			}

			case k_ESteamNetworkingConnectionState_Connecting:
			{
				// This must be a new connection
				assert( m_mapClients.find( pInfo->m_hConn ) == m_mapClients.end() );

				Printf( "Connection request from %s", pInfo->m_info.m_szConnectionDescription );

				// A client is attempting to connect
				// Try to accept the connection.
				if ( m_pInterface->AcceptConnection( pInfo->m_hConn ) != k_EResultOK )
				{
					// This could fail.  If the remote host tried to connect, but then
					// disconnected, the connection may already be half closed.  Just
					// destroy whatever we have on our side.
					m_pInterface->CloseConnection( pInfo->m_hConn, 0, nullptr, false );
					Printf( "Can't accept connection.  (It was already closed?)" );
					break;
				}

				// Assign the poll group
				if ( !m_pInterface->SetConnectionPollGroup( pInfo->m_hConn, m_hPollGroup ) )
				{
					m_pInterface->CloseConnection( pInfo->m_hConn, 0, nullptr, false );
					Printf( "Failed to set poll group?" );
					break;
				}

				// Generate a random nick.  A random temporary nick
				// is really dumb and not how you would write a real chat server.
				// You would want them to have some sort of signon message,
				// and you would keep their client in a state of limbo (connected,
				// but not logged on) until them.  I'm trying to keep this example
				// code really simple.
				char nick[ 64 ];
				sprintf( nick, "HL2_DM_Player%d", 10000 + ( rand() % 100000 ) );

				// Send them a welcome message
				sprintf( temp, "Welcome, stranger.  Thou art known to us for now as '%s'; upon thine command '/nick' we shall know thee otherwise.", nick ); 
				SendStringToClient( pInfo->m_hConn, temp ); 

				// Also send them a list of everybody who is already connected
				if ( m_mapClients.empty() )
				{
					SendStringToClient( pInfo->m_hConn, "Thou art utterly alone." ); 
				}
				else
				{
					sprintf( temp, "%d companions greet you:", (int)m_mapClients.size() ); 
					for ( auto &c: m_mapClients )
						SendStringToClient( pInfo->m_hConn, c.second.m_sNick.c_str() ); 
				}

				// Let everybody else know who they are for now
				sprintf( temp, "Hark!  A stranger hath joined this merry host.  For now we shall call them '%s'", nick ); 
				SendStringToAllClients( temp, pInfo->m_hConn ); 

				// Add them to the client list, using std::map wacky syntax
				m_mapClients[ pInfo->m_hConn ];
				SetClientNick( pInfo->m_hConn, nick );
				break;
			}

			case k_ESteamNetworkingConnectionState_Connected:
				// We will get a callback immediately after accepting the connection.
				// Since we are the server, we can ignore this, it's not news to us.
				break;

			default:
				// Silences -Wswitch
				break;
		}
	}

	static ChatServer *s_pCallbackInstance;
	static void SteamNetConnectionStatusChangedCallback( SteamNetConnectionStatusChangedCallback_t *pInfo )
	{
		s_pCallbackInstance->OnSteamNetConnectionStatusChanged( pInfo );
	}
	
	void RunCallBacks()
	{
		s_pCallbackInstance = this;
		m_pInterface->RunCallbacks();
	}
};

ChatServer *ChatServer::s_pCallbackInstance = nullptr;

/////////////////////////////////////////////////////////////////////////////
//
// ChatClient
//
/////////////////////////////////////////////////////////////////////////////

class ChatClient
{
public:
	void Run( const SteamNetworkingIPAddr &serverAddr )
	{
		// Select instance to use.  For now we'll always use the default.
		m_pInterface = SteamNetworkingSockets();

		// Start connecting
		char szAddr[ SteamNetworkingIPAddr::k_cchMaxString ];
		serverAddr.ToString( szAddr, sizeof(szAddr), true );
		Printf( "Connecting to chat server at %s", szAddr );
		SteamNetworkingConfigValue_t opt;
		opt.SetPtr( k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, (void*)SteamNetConnectionStatusChangedCallback );
		//opt.SetInt32(k_ESteamNetworkingConfig_IP_AllowWithoutAuth, 1);
		//opt.SetInt32(k_ESteamNetworkingConfig_Unencrypted, 3);
		m_hConnection = m_pInterface->ConnectByIPAddress( serverAddr, 1, &opt );
		if ( m_hConnection == k_HSteamNetConnection_Invalid )
			FatalError( "Failed to create connection" );

		while ( !g_bQuit )
		{
			PollIncomingMessages();
			RunCallBacks();
			PollLocalUserInput();
			std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
		}
	}
private:

	HSteamNetConnection m_hConnection;
	ISteamNetworkingSockets *m_pInterface;

	void PollIncomingMessages()
	{
		while ( !g_bQuit )
		{
			ISteamNetworkingMessage *pIncomingMsg = nullptr;
			int numMsgs = m_pInterface->ReceiveMessagesOnConnection( m_hConnection, &pIncomingMsg, 1 );
			if ( numMsgs == 0 )
				break;
			if ( numMsgs < 0 )
				FatalError( "Error checking for messages" );

			if (DetermineMessageType(pIncomingMsg) == chat_message)
			{
				void* temp_str;
				RemoveFirstByte(&temp_str, pIncomingMsg->m_pData, pIncomingMsg->m_cbSize);
				// Just echo anything we get from the server
				fwrite(temp_str, 1, pIncomingMsg->m_cbSize - 1, stdout);
				fputc('\n', stdout);
				delete temp_str;
			}

			// We don't need this anymore.
			pIncomingMsg->Release();
		}
	}

	void PollLocalUserInput()
	{
		std::string cmd;
		while ( !g_bQuit && LocalUserInput_GetNext( cmd ))
		{

			// Check for known commands
			if ( strcmp( cmd.c_str(), "/quit" ) == 0 )
			{
				g_bQuit = true;
				Printf( "Disconnecting from chat server" );

				// Close the connection gracefully.
				// We use linger mode to ask for any remaining reliable data
				// to be flushed out.  But remember this is an application
				// protocol on UDP.  See ShutdownSteamDatagramConnectionSockets
				m_pInterface->CloseConnection( m_hConnection, 0, "Goodbye", true );
				break;
			}

			// Anything else, just send it to the server and let them parse it
			SendTypedMessage(m_hConnection, cmd.c_str(), (uint32)cmd.length(), k_nSteamNetworkingSend_Reliable, nullptr, chat_message, m_pInterface);
		}
	}

	void OnSteamNetConnectionStatusChanged( SteamNetConnectionStatusChangedCallback_t *pInfo )
	{
		assert( pInfo->m_hConn == m_hConnection || m_hConnection == k_HSteamNetConnection_Invalid );

		// What's the state of the connection?
		switch ( pInfo->m_info.m_eState )
		{
			case k_ESteamNetworkingConnectionState_None:
				// NOTE: We will get callbacks here when we destroy connections.  You can ignore these.
				break;

			case k_ESteamNetworkingConnectionState_ClosedByPeer:
			case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			{
				g_bQuit = true;

				// Print an appropriate message
				if ( pInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connecting )
				{
					// Note: we could distinguish between a timeout, a rejected connection,
					// or some other transport problem.
					Printf( "We sought the remote host, yet our efforts were met with defeat.  (%s)", pInfo->m_info.m_szEndDebug );
				}
				else if ( pInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ProblemDetectedLocally )
				{
					Printf( "Alas, troubles beset us; we have lost contact with the host.  (%s)", pInfo->m_info.m_szEndDebug );
				}
				else
				{
					// NOTE: We could check the reason code for a normal disconnection
					Printf( "The host hath bidden us farewell.  (%s)", pInfo->m_info.m_szEndDebug );
				}

				// Clean up the connection.  This is important!
				// The connection is "closed" in the network sense, but
				// it has not been destroyed.  We must close it on our end, too
				// to finish up.  The reason information do not matter in this case,
				// and we cannot linger because it's already closed on the other end,
				// so we just pass 0's.
				m_pInterface->CloseConnection( pInfo->m_hConn, 0, nullptr, false );
				m_hConnection = k_HSteamNetConnection_Invalid;
				break;
			}

			case k_ESteamNetworkingConnectionState_Connecting:
				// We will get this callback when we start connecting.
				// We can ignore this.
				break;

			case k_ESteamNetworkingConnectionState_Connected:
				Printf( "Connected to server OK" );
				break;

			default:
				// Silences -Wswitch
				break;
		}
	}

	static ChatClient *s_pCallbackInstance;
	static void SteamNetConnectionStatusChangedCallback( SteamNetConnectionStatusChangedCallback_t *pInfo )
	{
		s_pCallbackInstance->OnSteamNetConnectionStatusChanged( pInfo );
	}

	void RunCallBacks()
	{
		s_pCallbackInstance = this;
		m_pInterface->RunCallbacks();
	}
};

ChatClient *ChatClient::s_pCallbackInstance = nullptr;

const uint16 DEFAULT_SERVER_PORT = 27055;

void PrintUsageAndExit( int rc = 1 )
{
	fflush(stderr);
	printf(
R"usage(Usage:
    mm_server client SERVER_ADDR
    mm_server server [--port PORT]
)usage"
	);
	fflush(stdout);
	exit(rc);
}

int main( int argc, const char *argv[] )
{
// 	srcon_addr addr_struct;
// 	addr_struct.addr = "127.0.1.1";
// 	addr_struct.pass = "123";
// 	addr_struct.port = 27015;
// 	srcon client = srcon(addr_struct);
// 	std::string response = client.send("echo test");
// 	printf("RCON: %s\n", response.c_str());
	
	bool bServer = false;
	bool bClient = false;
	int nPort = DEFAULT_SERVER_PORT;
	SteamNetworkingIPAddr addrServer; addrServer.Clear();

	for ( int i = 1 ; i < argc ; ++i )
	{
		if ( !bClient && !bServer )
		{
			if ( !strcmp( argv[i], "client" ) )
			{
				bClient = true;
				continue;
			}
			if ( !strcmp( argv[i], "server" ) )
			{
				bServer = true;
				continue;
			}
		}
		if ( !strcmp( argv[i], "--port" ) )
		{
			++i;
			if ( i >= argc )
				PrintUsageAndExit();
			nPort = atoi( argv[i] );
			if ( nPort <= 0 || nPort > 65535 )
				FatalError( "Invalid port %d", nPort );
			continue;
		}

		// Anything else, must be server address to connect to
		if ( bClient && addrServer.IsIPv6AllZeros() )
		{
			if ( !addrServer.ParseString( argv[i] ) )
				FatalError( "Invalid server address '%s'", argv[i] );
			if ( addrServer.m_port == 0 )
				addrServer.m_port = DEFAULT_SERVER_PORT;
			continue;
		}

		PrintUsageAndExit();
	}

	if ( bClient == bServer || ( bClient && addrServer.IsIPv6AllZeros() ) )
		PrintUsageAndExit();

	// Create client and server sockets
	InitSteamDatagramConnectionSockets();
	LocalUserInput_Init();

	if ( bClient )
	{
		ChatClient client;
		client.Run( addrServer );
	}
	else
	{
		ChatServer server;
		server.Run( (uint16)nPort );
	}

	ShutdownSteamDatagramConnectionSockets();

	// Ug, why is there no simple solution for portable, non-blocking console user input?
	// Just nuke the process
	LocalUserInput_Kill();
	NukeProcess(0);
}
