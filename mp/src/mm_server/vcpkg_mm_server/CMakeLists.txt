#
# Example porject that uses GameNetworkingSockets, obtaining
# dependencies, etc entirely through vcpkg.
#
cmake_minimum_required(VERSION 3.9)
project( mm_server_vcpkg )

# Ask vcpkg to fetch gamenetworkingsockets and all
# its dependencies and build it
find_package(GameNetworkingSockets CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)

file(GLOB SOURCES
    ../mm_shared.cpp
	../mm_server.cpp
	../SourceRCON/src/srcon.cpp
)

# Our program only has one cpp file
add_executable(
	mm_server
	${SOURCES}
	)

target_include_directories(mm_server PUBLIC
	../SourceRCON/include/
)
	
# This example links GameNetworkingSockets as a shared lib.
target_link_libraries(mm_server PRIVATE GameNetworkingSockets::shared SQLite::SQLite3)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS DEBUG)
