/* core0/serial_mirror.h — serial mirror (Phase 1, firmware side) */
#pragma once

void serial_mirror_init(void);

/* Called from the core 0 main loop. Streams the text frame protocol
 * over the RS232 UART: resolution/foreground/background headers on
 * connect and on change, `tile=` lines when tiles are redefined, and
 * one `data=` frame line per refresh (modes 0/1 only, ~3 fps).
 * Transmission is chunked so the main loop stays responsive. */
void serial_mirror_poll(void);
