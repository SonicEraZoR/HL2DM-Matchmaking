#
# Example porject that uses GameNetworkingSockets, obtaining
# dependencies, etc entirely through vcpkg.
#
cmake_minimum_required(VERSION 3.9)
project( mm_server_vcpkg )

# Ask vcpkg to fetch gamenetworkingsockets and all
# its dependencies and build it
find_package(GameNetworkingSockets CONFIG REQUIRED)

# Our program only has one cpp file
add_executable(
	mm_server
	../mm_server.cpp)

# This example links GameNetworkingSockets as a shared lib.
target_link_libraries(mm_server GameNetworkingSockets::shared)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
