/* board_config.h
 *
 * Board selection for the SPIComputer OS.
 *
 * The platform define (PICO_RP2350 / PICO_RP2040) is set automatically by
 * the pico-sdk from the selected PICO_BOARD / PICO_PLATFORM, and doubles as
 * the capability switch:
 *
 *   - RP2040 dev board (default): SD card only.
 *   - RP2354B product board: adds HDMI (HSTX), C64 key matrix, joysticks.
 *
 * Pin assignments are package-correct per platform.
 */
#pragma once

#if defined(PICO_RP2350)

/* ==================== RP2350 boards ==================== */

#define SPICOMPUTER_HAS_HDMI 1

/* Target CPU clock (core0/main.c applies it before anything else runs;
 * core 1 reports the result in the boot log).
 *
 * The value must divide (through the HSTX's 1..3 clock divider) to
 * 126 MHz so the pixel clock stays the exact 25.2 MHz: 252 MHz (/2) and
 * 378 MHz (/3) both do, and both keep the flash clock within its boot
 * range (core0/main.c rescales the QMI divider). The default is 378 MHz
 * (3x the 126 MHz baseline, core voltage 1.30 V): it is the highest
 * clock that keeps the DVI timing exact. 400 MHz and above run fine on
 * the silicon but cannot be divided to 25.2 MHz (400/3 = 133.3 MHz,
 * i.e. 26.7 MHz pixels and ~63.5 Hz refresh, off spec), and clk_hstx
 * cannot borrow pll_usb either, because USB stdio pins that PLL to
 * 48/96/144 MHz. A monitor that has trouble holding sync is the last
 * thing that wants an off-spec pixel clock, so stay at 378.
 *
 * Build with -DSPICOMPUTER_SYS_CLOCK_KHZ=<khz> to override; the clock is
 * applied by core0/main.c before anything else runs and reported in the
 * boot log. */
#ifndef SPICOMPUTER_SYS_CLOCK_KHZ
#define SPICOMPUTER_SYS_CLOCK_KHZ 378000
#endif

#define HSTX_D0_P_PIN 12
#define HSTX_D0_N_PIN 13
#define HSTX_CLK_P_PIN 14
#define HSTX_CLK_N_PIN 15
#define HSTX_D2_P_PIN 16
#define HSTX_D2_N_PIN 17
#define HSTX_D1_P_PIN 18
#define HSTX_D1_N_PIN 19

#if defined(SPICOMPUTER_PROFILE_RP2354B)
#define SPICOMPUTER_HAS_KEYMATRIX 1
#define SPICOMPUTER_HAS_JOYSTICKS 1
#define SPICOMPUTER_HAS_RESTORE 1
#endif

/* The pico2 dev board's onboard LED makes boot progress observable
 * without a console. Only for that profile: on the product board the
 * same pin is a keyboard column. */
#if defined(SPICOMPUTER_PROFILE_PICO2) && !defined(SPICOMPUTER_HAS_KEYMATRIX)
#define SPICOMPUTER_HAS_BOOT_LED 1
#define SPICOMPUTER_BOOT_LED_PIN PICO_DEFAULT_LED_PIN
#endif

/* SD card on SPI1. HSTX uses the explicit differential GPIO pairs above;
 * SPI1 uses RX=GP8, CSn=GP9, SCK=GP10, TX=GP11. */
#define SD_SPI_INSTANCE spi1
#define SD_PIN_SCK 10
#define SD_PIN_MOSI 11
#define SD_PIN_MISO 8
#define SD_PIN_CS 9

/* External SPI bus (SPI0) for expansion drives/peripherals.
 * Pins defined for the board layout; no driver yet. */
#define EXT_SPI_INSTANCE spi0
#define EXT_SPI_SCK_PIN 2
#define EXT_SPI_MOSI_PIN 3
#define EXT_SPI_MISO_PIN 0
#define EXT_SPI_CS_PIN 1

/* USB: the RP2350 USB PHY uses dedicated package pins (no GPIOs), so
 * the board can expose a USB connector for flashing without spending
 * any of the 48 GPIOs. */

/* C64 keyboard matrix: 8 columns read, 8 rows driven, plus RESTORE.
 * Active-low like the original C64 (row driven low, columns pulled up).
 * Wired in logical order per the keyboard connector: COL0-7 on GP20-27,
 * ROW0-7 on GP28-35, RESTORE on GP36. */
#define KB_ROWS 8
#define KB_COLS 8
#define KB_COL_PIN(col) (20 + (col)) /* read lines, C64 PB0-7 order */
#define KB_ROW_PIN(row) (28 + (row)) /* driven lines, C64 PA0-7 order */
#define RESTORE_PIN 36 /* active low, pulled up */

/* Two DSUB9 digital joysticks, active-low (switch to GND, pulled up):
 * up/down/left/right/fire. Dirs on GP37-40 (stick 1) and GP43-46
 * (stick 2); fires on GP6/GP7. GP47 (XIP_CS1n) stays free. */
#define JOY1_UP_PIN 37
#define JOY1_DOWN_PIN 38
#define JOY1_LEFT_PIN 39
#define JOY1_RIGHT_PIN 40
#define JOY1_FIRE_PIN 6
#define JOY2_UP_PIN 43
#define JOY2_DOWN_PIN 44
#define JOY2_LEFT_PIN 45
#define JOY2_RIGHT_PIN 46
#define JOY2_FIRE_PIN 7

#else

/* ==================== RP2040 dev board (default) ==================== */

/* SD card on SPI1 (standard Pico SPI1 pins). */
#define SD_SPI_INSTANCE spi1
#define SD_PIN_SCK 10
#define SD_PIN_MOSI 11
#define SD_PIN_MISO 12
#define SD_PIN_CS 13

#endif
