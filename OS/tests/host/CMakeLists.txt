# Host-side tests
#
# Standalone CMake project: builds the real Lua 5.5 core and the OS's
# fatfs_lua.c bridge against mock FatFs/SD headers, so the Lua<->SD glue
# can be tested on the developer machine without hardware.
#
# Build and run:
#   cmake -S tests/host -B build-host
#   cmake --build build-host
#   ./build-host/spicomputer_host_tests [path/to/os.lua]
#
# (Defaults to ../../os.lua relative to the current directory.)

cmake_minimum_required(VERSION 3.13)

project(spicomputer_host_tests C)

set(CMAKE_C_STANDARD 11)

# Real Lua 5.5 core (excluding the standalone interpreters)
file(GLOB LUA_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/../../lua/*.c)
list(REMOVE_ITEM LUA_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/../../lua/lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../lua/luac.c)

add_executable(spicomputer_host_tests
        test_main.c
        mock/mock_fatfs.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fatfs_lua.c
        ${LUA_SOURCES}
)

target_include_directories(spicomputer_host_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/mock
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
        ${CMAKE_CURRENT_SOURCE_DIR}/../../lua
)

target_link_libraries(spicomputer_host_tests m)
