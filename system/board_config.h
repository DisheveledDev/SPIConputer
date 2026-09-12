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

/* ==================== RP2354B product board ==================== */

#define SPICOMPUTER_HAS_HDMI 1
#define SPICOMPUTER_HAS_KEYMATRIX 1
#define SPICOMPUTER_HAS_JOYSTICKS 1
#define SPICOMPUTER_HAS_RESTORE 1

/* SD card on SPI1. GP12-15 carry HSTX on RP2350, so the card uses the
 * other SPI1 function pins: RX=GP8, CSn=GP9, SCK=GP10, TX=GP11. */
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
