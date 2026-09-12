# Host-side tests
#
# Standalone CMake project: builds the real Lua 5.5 core, the real RPC
# transport and the real fs_lua.c/fs_core0.c stack against mock FatFs/SD
# headers, so the Lua<->SD bridge and the RPC protocol can be tested on
# the developer machine without hardware.
#
# Build and run:
#   cmake -S tests/host -B build-host
#   cmake --build build-host
#   ./build-host/spicomputer_host_tests
#
# All card programs are fixtures inside the tests: the Lua programs
# that run on the device are developed outside this repo.

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
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fs_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fs_core0.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../rpc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../system_state.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../program.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sys_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../screen_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sound_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../audio.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../video.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../os_time.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../input.c
        ${LUA_SOURCES}
)

target_include_directories(spicomputer_host_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/mock
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
        ${CMAKE_CURRENT_SOURCE_DIR}/../../lua
)

target_link_libraries(spicomputer_host_tests m)

# Audio engine + score scheduler + PCM playback + sound Lua module (Phase 8).
add_executable(spicomputer_audio_tests
        audio_test.c
        mock/mock_fatfs.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../audio.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sound_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../program.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sys_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../screen_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../video.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../os_time.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fs_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fs_core0.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../rpc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../input.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../system_state.c
        ${LUA_SOURCES}
)

target_include_directories(spicomputer_audio_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/mock
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
        ${CMAKE_CURRENT_SOURCE_DIR}/../../lua
)

target_link_libraries(spicomputer_audio_tests m)

# Input engine (input.c + the shared protocol parser), tested with
# simulated GPIO patterns and byte streams.
add_executable(spicomputer_input_tests
        input_test.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../input.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../system_state.c
)

target_include_directories(spicomputer_input_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
)

# Process model (program.c + sys_lua.c) over the RPC + mock SD.
add_executable(spicomputer_program_tests
        program_test.c
        mock/mock_fatfs.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../program.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sys_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../screen_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sound_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../audio.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../video.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../os_time.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fs_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fs_core0.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../rpc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../input.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../system_state.c
        ${LUA_SOURCES}
)

target_include_directories(spicomputer_program_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/mock
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
        ${CMAKE_CURRENT_SOURCE_DIR}/../../lua
)

target_link_libraries(spicomputer_program_tests m)

# Renderer + serial mirror encoder + screen Lua module (Phase 1).
add_executable(spicomputer_render_tests
        render_test.c
        mock/mock_fatfs.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../render.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../video.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../screen_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../program.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sys_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sound_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../audio.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../os_time.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fs_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fs_core0.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../rpc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../input.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../system_state.c
        ${LUA_SOURCES}
)

target_include_directories(spicomputer_render_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/mock
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
        ${CMAKE_CURRENT_SOURCE_DIR}/../../lua
)

target_link_libraries(spicomputer_render_tests m)


# Editor (Phase 6): boots an editor.lua supplied on the command line
# and simulates typing. The editor lives in its own project, so this
# harness skips when no path is given.
add_executable(spicomputer_editor_tests
        editor_test.c
        mock/mock_fatfs.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../render.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../video.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../screen_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../program.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sys_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sound_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../audio.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../os_time.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fs_lua.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../fs_core0.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../rpc.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../input.c
        ${CMAKE_CURRENT_SOURCE_DIR}/../../system_state.c
        ${LUA_SOURCES}
)

target_include_directories(spicomputer_editor_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/mock
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
        ${CMAKE_CURRENT_SOURCE_DIR}/../../lua
)

target_link_libraries(spicomputer_editor_tests m)
