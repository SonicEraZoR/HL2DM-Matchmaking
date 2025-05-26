Half-Life 2 Deathmatch with matchmaking and matchmaking server itself
=====

This is a mod that brings matchmaking queue to Half-Life 2 Deathmatch with a fully open source matchmaking server implementation written in C++ using Valve's [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) networking library. It's based on [example_chat](https://github.com/ValveSoftware/GameNetworkingSockets/tree/master/examples/vcpkg_example_chat) program from that library's repo. It currently has basic functionality of searching for a game based on map and gamemode and talking in chat with other players connected to the matchmaking server through console commands. However the matchmaking server doesn't currently properly pick game server to play on from a list of servers and just uses one server every time. Still this code might be useful to someone who tries to understand how matchmaking in multiplayer games works.

## Usage

### Setting up the matchmaking server

The server can be run with "mm_server server" from the directory of the executable (by default it's "(SDK Dir)/mp/src/mm_server/vcpkg_mm_server/build". From here there are a few commands it accepts:

* /quit - shutdown the server)
* /num_s - number of players in a lobby required to start the game
* /game_sip IP of the game server
* /print_lobbies - print all of the lobbies

By default it runs on port 27055 but this can be changed with "--port" argument like so "mm_server server --port 1111".

The server can also run as a chat client with "mm_server client (server address)".

### Setting up the game server

Technically setting up the game server is not required but without it clients won't have anywhere to connect after finding match. I won't provide instructions on how to setup Half-Life 2 Deathmatch server, since there are plenty of resources online that describe that process. The only difference is that when running the server, "-game" argument needs to point to a build of the matchmaking mod (hl2mp_mm folder). Also it needs to be 32 bit server specifically, since the mod hasn't been updated to the new MP SDK as of now.

### Setting up the game client

Same as Source SDK 2013 mod: https://developer.valvesoftware.com/wiki/Setup_mod_on_Steam. Although the mod needs to run on 32 bit version of Half-Life 2 Deathmatch, since the mod hasn't been updated to the new MP SDK as of now. You can force this on 64 bit version of Windows by going into the game directory and creating shortcut for "hl2mp.exe" and specifying "-game" argument that points to the build of the matchmaking mod (hl2mp_mm folder).

### Using matchmaking capabilities in-game

The mod doesn't currently have any graphical user interface, everything is done with console commands:

* mm_connect (ip) - connect to a matchmaking server, this command can be put in autoexec.cfg to connect automatically on booting the mod.
* mm_find_game - start searching for a match.
* mm_auto_start_game 1|0 - automatically start a match if it was found, enabled by default.
* mm_start_game - start a match if mm_auto_start_game is disabled.
* mm_set_map 1-8 - set the map to search for, prints possible maps when no map is specified.
* mm_set_tdm 1|0 - set if you want to search for team deathmatch or not.
* mm_cancel_search - cancel search and leave a matchmaking lobby.
* mm_chatsay - say something to a matchmaking chat.
* mm_disconnect - disconnect from a matchmaking server.

## Dependencies

### Windows
* [Visual Studio 2013 with Update 5](https://visualstudio.microsoft.com/vs/older-downloads/)

### macOS
* [Xcode 5.0.2](https://developer.apple.com/downloads/more)

### Linux
* GCC 4.8
* [Steam Client Runtime](http://media.steampowered.com/client/runtime/steam-runtime-sdk_latest.tar.xz)

### All platforms

* [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets)

## Building

### Matchmaking Client

Compiling process for the client is the same as for Source SDK 2013. Instructions for building Source SDK 2013 can be found here: https://developer.valvesoftware.com/wiki/Source_SDK_2013. The client doesn't currently compile on Linux, but the server portion of the mod does, so if you want to run dedicated server on Linux - you can. The command is "make -f games.mak server_hl2mp_mm".

One additional thing you need to do though is compile [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) library itself for Windows since it's not compiled automatically with the mod itself. Then after it's compiled you need to copy GameNetworkingSockets.dll, libcrypto-3.dll and libprotobuf.dll to the Half-Life 2 Deathmatch bin folder in the root of the game.

### Matchmaking Server

The server is fully functional only on Unix systems since it uses [SourceRCON](https://github.com/Phil25/SourceRCON) library to communicate with a game server and it only works on Unix systems. Technically you can use preprocessor definitions to remove all calls to SourceRCON and compile the server on Windows but when it won't be able to control map and gamemode on the game server.

The building process is similar to [example_chat](https://github.com/ValveSoftware/GameNetworkingSockets/blob/master/examples/vcpkg_example_chat/README.md) program from [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets) repo that the server is based on:

First, we bootstrap a project-specific installation of vcpkg ("manifest mode")
in the default location, `<project root>/vcpkg`.  From the project root (mp/src/mm_server/vcpkg_mm_server), run these
commands:

```
git clone https://github.com/microsoft/vcpkg
./vcpkg/bootstrap-vcpkg.sh
```

Now we ask vcpkg to install of the dependencies for our project, which are described by
the file `<project root>/vcpkg.json`.  Note that this step is optional, as cmake will
automatically do this.  But here we are doing it in a separate step so that we can isolate
any problems, because if problems happen here don't have anything to do with your
cmake files.
This command might ask you to install additional dependencies, you should just do it using the suggested way.

```
./vcpkg/vcpkg install --triplet=x64-linux
```

Next build the project files.  There are different options for telling cmake how
to integrate with vcpkg; here we use `CMAKE_TOOLCHAIN_FILE` on the command line.
Also we select Ninja project generator.

```
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE="vcpkg/scripts/buildsystems/vcpkg.cmake" -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=1
```

Finally, build the project:

```
> cd build
> ninja
```
