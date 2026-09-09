# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion RISCV_PICO_2_3_1_0)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
# The pico-vscode shim hardcodes the toolchain/version selected inside the
# VS Code extension (currently RISC-V), which is wrong for CLI builds of
# the other platform. Presets that build from the CLI set
# SPICOMPUTER_SKIP_VSCODE and provide PICO_SDK_PATH/PICO_TOOLCHAIN_PATH
# themselves.
if (NOT SPICOMPUTER_SKIP_VSCODE AND EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(SPIComputerOS C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Lua 5.5 interpreter core (static library, embedded use)
file(GLOB LUA_SOURCES ${CMAKE_CURRENT_LIST_DIR}/lua/*.c)
list(REMOVE_ITEM LUA_SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/lua/lua.c
        ${CMAKE_CURRENT_LIST_DIR}/lua/luac.c)
add_library(lua_core STATIC ${LUA_SOURCES})
target_include_directories(lua_core PUBLIC ${CMAKE_CURRENT_LIST_DIR}/lua)
target_link_libraries(lua_core pico_stdlib)

# FatFs + SD card SPI driver (vendored from carlk3/no-OS-FatFS-SD-SPI-RPi-Pico)
add_subdirectory(FatFs_SPI)

# Add executable. Default name is the project name, version 0.1

add_executable(SPIComputerOS
        core0/main.c
        core1/lua_main.c
        hw_config.c
        fatfs_lua.c
)

pico_set_program_name(SPIComputerOS "SPIComputerOS")
pico_set_program_version(SPIComputerOS "0.1")

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(SPIComputerOS 1)
pico_enable_stdio_usb(SPIComputerOS 0)

# Add the standard library to the build
target_link_libraries(SPIComputerOS
        pico_stdlib
        lua_core
        FatFs_SPI)

# Add the standard include files to the build
target_include_directories(SPIComputerOS PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# Add any user requested libraries
target_link_libraries(SPIComputerOS
        hardware_spi
        hardware_timer
        hardware_watchdog
        hardware_clocks
        pico_multicore
        )

pico_add_extra_outputs(SPIComputerOS)

