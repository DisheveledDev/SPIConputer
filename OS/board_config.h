/* board_config.h
 *
 * Board selection for the SPIComputer OS.
 *
 * The platform define (PICO_RP2350 / PICO_RP2040) is set automatically by
 * the pico-sdk from the selected PICO_BOARD / PICO_PLATFORM, and doubles as
 * the capability switch:
 *
 *   - RP2040 dev board (default): RS232 comms + SD card only.
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

/* SD card on SPI1. GP12-15 carry HSTX on RP2350, so the card uses the
 * other SPI1 function pins: RX=GP8, CSn=GP9, SCK=GP10, TX=GP11. */
#define SD_SPI_INSTANCE spi1
#define SD_PIN_SCK 10
#define SD_PIN_MOSI 11
#define SD_PIN_MISO 8
#define SD_PIN_CS 9

/* RS232 development link (TTL-level UART; see AGENTS.md) */
#define RS232_UART_INSTANCE uart1
#define RS232_PIN_TX 4
#define RS232_PIN_RX 5

/* C64 keyboard matrix: 8 rows out, 8 columns in (TBD, placeholder) */
#define KB_ROWS 8
#define KB_COLS 8

#else

/* ==================== RP2040 dev board (default) ==================== */

/* SD card on SPI1 (standard Pico SPI1 pins). */
#define SD_SPI_INSTANCE spi1
#define SD_PIN_SCK 10
#define SD_PIN_MOSI 11
#define SD_PIN_MISO 12
#define SD_PIN_CS 13

/* RS232 development link (TTL-level UART; see AGENTS.md) */
#define RS232_UART_INSTANCE uart1
#define RS232_PIN_TX 4
#define RS232_PIN_RX 5

#endif
