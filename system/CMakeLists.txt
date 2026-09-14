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
set(SPICOMPUTER_BOARD_PROFILE "pico2" CACHE STRING "SPIComputer hardware profile")
set_property(CACHE SPICOMPUTER_BOARD_PROFILE PROPERTY STRINGS pico2 rp2354b)



if(SPICOMPUTER_BOARD_PROFILE STREQUAL "pico2")
    add_compile_definitions(SPICOMPUTER_PROFILE_PICO2=1)
elseif(SPICOMPUTER_BOARD_PROFILE STREQUAL "rp2354b")
    add_compile_definitions(SPICOMPUTER_PROFILE_RP2354B=1)
else()
    message(FATAL_ERROR "Unknown SPIComputer board profile: ${SPICOMPUTER_BOARD_PROFILE}")
endif()

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
        core0/video_hw.c
        core1/input_hw.c
        core1/lua_main.c
        hw_config.c
        fs_lua.c
        fs_core0.c
        rpc.c
        input.c
        program.c
        sys_lua.c
        screen_lua.c
        sound_lua.c
        audio.c
        video.c
        render.c
        render332.c
        scanout.c
        boot_signal.c
        os_time.c
        system_state.c
)

pico_set_program_name(SPIComputerOS "SPIComputerOS")
pico_set_program_version(SPIComputerOS "0.1")

# Stdio: the dev board (RP2040) prints over UART; the product board
# (RP2354B) has no UART stdio pins left (RS232 GP4/5 is the protocol
# stream, GP0/1 are the external SPI), so it prints over USB.
if (PICO_RP2350)
    pico_enable_stdio_uart(SPIComputerOS 0)
    pico_enable_stdio_usb(SPIComputerOS 1)
else()
    pico_enable_stdio_uart(SPIComputerOS 1)
    pico_enable_stdio_usb(SPIComputerOS 0)
endif()

# Add the standard library to the build
target_link_libraries(SPIComputerOS
        pico_stdlib
        lua_core
        FatFs_SPI)

# Add the standard include files to the build
target_include_directories(SPIComputerOS PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

# Core 0's stack. This is a compile-time define in the SDK (not a CMake
# cache variable) consumed by the CRT: the SDK default is 2 KB, and core
# 0 now runs the scanout IRQs, where the video DMA IRQ can preempt the
# render ISR. (SDK 2.x compiles its sources into the application target,
# so the define goes here rather than on an SDK library.)
#
# Core 1's stack is not set here: the SDK's stack lives in SCRATCH_X,
# which is only 4 KB, so core1_entry gets a stack in main SRAM instead
# (see core0/main.c).
# Diagnostic build: draw the test pattern on the scanout from the first
# frame and run no SD/Lua at all, so the video path can be judged on its
# own. Normal builds show the same pattern automatically when boot finds
# no program (no card, or no boot script): see core1/lua_main.c and
# core0/video_hw.c for the pattern layout.
if (SPICOMPUTER_VIDEO_TEST_PATTERN)
    target_compile_definitions(SPIComputerOS PRIVATE
            SPICOMPUTER_VIDEO_TEST_PATTERN=1)
endif()

# Bring-up logging period, in milliseconds (see core1/lua_main.c): a
# status line per period goes to USB stdio and spilog.txt on the card.
# Build with -DSPICOMPUTER_LOG_PERIOD_MS=5000 to space the writes out
# (tests whether the SD/stdio writes themselves disturb the display).
set(SPICOMPUTER_LOG_PERIOD_MS 1000 CACHE STRING
        "Bring-up status/log period in ms")
target_compile_definitions(SPIComputerOS PRIVATE
        SPICOMPUTER_LOG_PERIOD_MS=${SPICOMPUTER_LOG_PERIOD_MS})

# Optional CPU clock override for experiments:
#   cmake -DSPICOMPUTER_SYS_CLOCK_KHZ=126000 ...
# (default comes from board_config.h; see the overclock notes there).
if (SPICOMPUTER_SYS_CLOCK_KHZ)
    target_compile_definitions(SPIComputerOS PRIVATE
            SPICOMPUTER_SYS_CLOCK_KHZ=${SPICOMPUTER_SYS_CLOCK_KHZ})
endif()

target_compile_definitions(SPIComputerOS PRIVATE
        PICO_STACK_SIZE=4096
        # 0: no SDK core-1 stack in SCRATCH_X, we pass our own to
        # multicore_launch_core1_with_stack().
        PICO_CORE1_STACK_SIZE=0)

# Add any user requested libraries
target_link_libraries(SPIComputerOS
        hardware_spi
        hardware_timer
        hardware_watchdog
        hardware_clocks
        hardware_dma
        hardware_irq
        hardware_resets
        pico_multicore
        pico_sync
        )

pico_add_extra_outputs(SPIComputerOS)

