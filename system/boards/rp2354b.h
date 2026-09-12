/*
 * SPIComputer OS product board: RP2354B (RP2350B die + 2 MB stacked
 * in-package flash). Custom board header until the SDK ships an
 * official RP2354 definition; SDK 2.3.1 predates it.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------

#ifndef _BOARDS_RP2354B_H
#define _BOARDS_RP2354B_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// For board detection
#define SPICOMPUTER_RP2354B

// --- RP2350 VARIANT ---
#define PICO_RP2350B 1

// --- FLASH: 2 MB stacked in-package flash (W25Q16-class, generic 03h
// commands) ---
#define PICO_BOOT_STAGE2_CHOOSE_GENERIC_03H 1

#ifndef PICO_FLASH_SPI_CLKDIV
#define PICO_FLASH_SPI_CLKDIV 2
#endif

pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (2 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

// RP2354 is the A2 silicon stepping.
pico_board_cmake_set_default(PICO_RP2350_A2_SUPPORTED, 1)
#ifndef PICO_RP2350_A2_SUPPORTED
#define PICO_RP2350_A2_SUPPORTED 1
#endif

// --- No stdio UART on this board (RS232 UART1 GP4/5 carries the
// protocol stream; GP0/1 are the external SPI0 bus). Stdio goes over
// USB (dedicated package pins, see board_config.h). ---
// --- No LED, no VSYS/VBUS sense pins: all 48 GPIOs are allocated by
// the OS pin map (AGENTS.md). ---

#endif
