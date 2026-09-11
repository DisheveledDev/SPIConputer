cmake_minimum_required(VERSION 3.13)

project(spicomputer_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(OS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../os)
set(MOCK_DIR ${OS_DIR}/tests/host/mock)

# Real Lua 5.5 core (excluding the standalone interpreters)
file(GLOB LUA_SOURCES ${OS_DIR}/lua/*.c)
list(REMOVE_ITEM LUA_SOURCES
        ${OS_DIR}/lua/lua.c
        ${OS_DIR}/lua/luac.c)

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
        ${OS_DIR}/audio.c
        ${OS_DIR}/sound_lua.c
        ${OS_DIR}/screen_lua.c
        ${OS_DIR}/sys_lua.c
        ${OS_DIR}/program.c
        ${OS_DIR}/video.c
        ${OS_DIR}/render.c
        ${OS_DIR}/fs_lua.c
        ${OS_DIR}/fs_core0.c
        ${OS_DIR}/rpc.c
        ${OS_DIR}/system_state.c
        ${OS_DIR}/input.c
        ${OS_DIR}/protocol.c
        ${OS_DIR}/os_time.c
        ${LUA_SOURCES}
)

target_include_directories(spicomputer_sim PRIVATE
        ${OS_DIR}
        ${OS_DIR}/lua
        ${MOCK_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(spicomputer_sim PRIVATE
        SIM_OS_DIR="${OS_DIR}"
)

target_link_libraries(spicomputer_sim PRIVATE m)
if(TARGET SDL2::SDL2)
    target_link_libraries(spicomputer_sim PRIVATE SDL2::SDL2)
else()
    target_include_directories(spicomputer_sim PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(spicomputer_sim PRIVATE ${SDL2_LIBRARIES})
endif()
