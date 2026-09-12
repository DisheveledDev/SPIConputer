cmake_minimum_required(VERSION 3.13)

project(spicomputer_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(SYSTEM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../system)
set(MOCK_DIR ${SYSTEM_DIR}/tests/host/mock)

# Real Lua 5.5 core (excluding the standalone interpreters)
file(GLOB LUA_SOURCES ${SYSTEM_DIR}/lua/*.c)
list(REMOVE_ITEM LUA_SOURCES
        ${SYSTEM_DIR}/lua/lua.c
        ${SYSTEM_DIR}/lua/luac.c)

# SDL2 (Homebrew on macOS: /opt/homebrew or /usr/local).
if(APPLE)
    list(APPEND CMAKE_PREFIX_PATH /opt/homebrew /usr/local)
endif()
find_package(SDL2 CONFIG REQUIRED)

# Platform-neutral OS sources; the hardware files under core0/ are
# replaced by sim_fs.c (SD) and sim_main.c (video/audio/input).
add_executable(spicomputer_sim
        sim_main.c
        sim_fs.c
        sim_dir.c
        ${SYSTEM_DIR}/audio.c
        ${SYSTEM_DIR}/sound_lua.c
        ${SYSTEM_DIR}/screen_lua.c
        ${SYSTEM_DIR}/sys_lua.c
        ${SYSTEM_DIR}/program.c
        ${SYSTEM_DIR}/video.c
        ${SYSTEM_DIR}/render.c
        ${SYSTEM_DIR}/fs_lua.c
        ${SYSTEM_DIR}/fs_core0.c
        ${SYSTEM_DIR}/rpc.c
        ${SYSTEM_DIR}/system_state.c
        ${SYSTEM_DIR}/input.c
        ${SYSTEM_DIR}/os_time.c
        ${LUA_SOURCES}
)

target_include_directories(spicomputer_sim PRIVATE
        ${SYSTEM_DIR}
        ${SYSTEM_DIR}/lua
        ${MOCK_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(spicomputer_sim PRIVATE m)
if(TARGET SDL2::SDL2)
    target_link_libraries(spicomputer_sim PRIVATE SDL2::SDL2)
else()
    target_include_directories(spicomputer_sim PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(spicomputer_sim PRIVATE ${SDL2_LIBRARIES})
endif()
