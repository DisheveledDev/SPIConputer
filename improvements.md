# HSTX / DVI output review (`system/core0/video_hw.c`)

The register-level HSTX setup (TMDS expander config, command lists, sync
symbols, CSR clock/shift settings, lane-to-pin mapping, ping/pong DMA
state machine) is a faithful port of the official
`pico-examples/hstx/dvi_out_hstx_encoder` and looks correct on its own.
The problems are in how it is integrated with the rest of the OS. Items
are ordered by severity; each has a "How to fix" section with concrete
pointers (SDK 2.3.1 API names, register fields, sizes and budgets).

---

## Blocking (will not work on hardware as written)

### 1. DMA channels 0/1 collide with the SD card driver

`video_hw.c` hard-codes `DMACH_PING = 0` / `DMACH_PONG = 1` and never
claims them. `fs_core0_mount()` runs first in `core0/main.c` and the
FatFs SPI driver (`FatFs_SPI/sd_driver/spi.c:187`) calls
`dma_claim_unused_channel(true)` twice, so the SD card already owns
channels 0 and 1 when `video_hw_init()` reprograms them. Both the SD
transfers and the video stream will be corrupted.

**How to fix**

- Replace the two `#define`s with `static uint s_dma_ping, s_dma_pong;`
  and claim them at the top of `video_hw_init()`:

  ```c
  s_dma_ping = dma_claim_unused_channel(true);
  s_dma_pong = dma_claim_unused_channel(true);
  ```

  Use these variables in `dma_channel_get_default_config`,
  `channel_config_set_chain_to`, `dma_channel_configure`, the `ints`/
  `inte` masks, `dma_channel_start` and in `hstx_dma_irq`
  (`dma_hw->ch[s_dma_ping]` etc.). The IRQ handler then reads two
  globals, which is cheap; if you want them constant-folded, claim
  fixed channels instead (`dma_channel_claim(0); dma_channel_claim(1);`)
  but do it *before* `fs_core0_mount()` so the SD driver picks 2/3.
- The recommended ordering in `core0/main.c` is: `video_hw_init()`
  (claims channels + IRQ, starts scanout with a black screen) →
  `fs_core0_mount()` → `input_hw_init()`. This also gives you a picture
  immediately at boot, before the SD card is probed.
- Add `hardware_dma` and `hardware_irq` to `target_link_libraries` in
  `system/CMakeLists.txt` (today they arrive only via the `FatFs_SPI`
  INTERFACE target).

### 2. `DMA_IRQ_0` is already taken as an exclusive handler

The SD driver installs `irq_set_exclusive_handler(DMA_IRQ_0, ...)`
(`irqShared` defaults to false in `hw_config.c`). The second
`irq_set_exclusive_handler(DMA_IRQ_0, hstx_dma_irq)` trips the SDK
hard-assert (or, with assertions off, silently replaces the SD handler
and SD reads hang forever).

**How to fix**

- Move the video onto its own DMA IRQ line. RP2350 has four
  (`DMA_IRQ_0..3`). Use `DMA_IRQ_1`:

  ```c
  dma_hw->ints1 = (1u << s_dma_ping) | (1u << s_dma_pong);   /* clear */
  dma_hw->inte1 = (1u << s_dma_ping) | (1u << s_dma_pong);   /* enable */
  irq_set_exclusive_handler(DMA_IRQ_1, hstx_dma_irq);
  irq_set_priority(DMA_IRQ_1, PICO_HIGHEST_IRQ_PRIORITY);
  irq_set_enabled(DMA_IRQ_1, true);
  ```

  and in the handler acknowledge via `dma_hw->ints1 = 1u << channel;`
  (not `dma_hw->intr`, which is the raw status shared by all lines).
  Alternatively use `dma_channel_set_irq1_enabled(ch, true)` and
  `dma_channel_acknowledge_irq1(ch)`.
- Keep the SD driver on `DMA_IRQ_0`; leave its `irqChannel1 = false`
  default. If you ever need to share a line, set `.irqShared = true` in
  `hw_config.c` so the driver uses `irq_add_shared_handler`.
- Make the video IRQ the highest priority on core 0 and make sure the
  1 kHz input scan (`input_hw.c`) and the SPI DMA completion IRQ stay
  short (a few microseconds) so they cannot push a scanline reload past
  the blanking window. On RISC-V (Hazard3) priorities are honoured
  through the SDK's `irq_set_priority`, but the handler for a lower
  priority IRQ is *not* preempted once running, so latency = longest
  other handler.

### 3. Rendering a full scanline inside the DMA IRQ cannot meet timing

In the active region the handler that posts pixel data runs while the
other channel is streaming the 9-word `s_vactive_line` list, i.e. it has
roughly the horizontal blanking interval (160 pixel clocks, ~5 to 6 us,
plus the 8-word HSTX FIFO) to finish `render_line()` (640 px, 3-layer
compositing, per-pixel work) plus `pack_rgb332()` (a second 640-pixel
loop over an RGB888 intermediate). That is tens of thousands of cycles,
well beyond even the whole line period (~27 us at a 30 MHz pixel
clock). The DMA retriggers with stale `read_addr`/`transfer_count`,
producing an unstable picture, and the IRQ consumes essentially all of
core 0, starving the fs RPC loop and the watchdog feed (2 s timeout).

**How to fix (recommended design: line ring + cell renderer)**

Budget first: at 25.2 MHz the line period is 31.7 us = ~4000 cycles at
126 MHz (item 5). Leave at least half for SD/RPC work, so the target is
under ~2000 cycles per 640-pixel line, i.e. ~3 cycles per pixel. That
rules out any per-pixel loop; render per 8-pixel cell.

a) **Decouple rendering from the DMA IRQ with a line ring.**

   ```c
   #define RING_LINES 8                      /* power of two */
   static uint32_t s_ring[RING_LINES][HSTX_WORDS_PER_LINE]; /* 5 KB */
   static volatile uint s_ring_rendered;    /* lines produced (monotonic) */
   static volatile uint s_ring_consumed;    /* lines handed to DMA */
   static volatile uint s_frame_count;
   ```

   The DMA IRQ, in the `else` (pixel) branch, only does:

   ```c
   uint idx = s_ring_consumed & (RING_LINES - 1);
   dma->read_addr = (uintptr_t)s_ring[idx];
   dma->transfer_count = HSTX_WORDS_PER_LINE;
   s_ring_consumed++;
   if (s_ring_rendered == s_ring_consumed) s_underruns++; /* diag only */
   ```

   Rendering happens in a lower-priority context that is woken by the
   IRQ. Two good options on RP2350:
   - a *software IRQ*: pick a spare IRQ number (`user_irq_claim_unused`,
     e.g. `SPARE_IRQ_0`), install `video_render_isr`, priority lower
     than the DMA IRQ, higher than SPI; the DMA IRQ does
     `irq_set_pending(spare_irq)`. The render ISR fills lines while
     `s_ring_rendered - s_ring_consumed < RING_LINES`. Because it is an
     IRQ it cannot be starved by the main loop's blocking FatFs calls.
   - or core 0's main loop (`while(1){ video_hw_render_pending();
     fs_core0_service(); ... }`), which is simpler but a long SD read
     (`f_read` of 4 KB at 12 MHz SPI is ~3 ms = ~95 lines) will
     underrun an 8-line ring, so the software-IRQ variant is the one to
     pick unless you make the ring a whole frame.

   Vertical blanking (45 lines) is free time: pre-render the first
   `RING_LINES` of the next frame during vblank so the ring is full when
   line 0 starts.

b) **Render directly to RGB332 words per cell, not per pixel.**
   Drop `s_rgb_line` and `pack_rgb332`. Keep a 256-entry
   `uint8_t s_palette332[256]` rebuilt whenever `video->version`
   changes (or hash the palette). Then:

   ```c
   /* 8 font bits -> 8 pixels = 2 words. Bit 0 is the leftmost pixel.
    * s_expand[bits] holds the 8 pixels as 0x00/0xFF byte masks. */
   static uint64_t s_expand[256];      /* built once, 2 KB */

   static inline void put_cell_1x(uint32_t *dst, uint8_t bits,
                                  uint32_t fg4, uint32_t bg4) {
       uint64_t m = s_expand[bits];
       uint32_t m0 = (uint32_t)m, m1 = (uint32_t)(m >> 32);
       dst[0] = (fg4 & m0) | (bg4 & ~m0);
       dst[1] = (fg4 & m1) | (bg4 & ~m1);
   }
   ```

   where `fg4 = fg332 * 0x01010101u`. For the 2x modes (0/1/10) use a
   second table `s_expand2x[256]` (16 pixels = 4 words) or double the
   mask with a bit trick. Per cell this is ~10 to 15 instructions
   instead of ~8 × 30, so an 80-column line is ~1000 cycles and a
   40-column line ~600. Text mode compositing (layers 2 → 0) is done
   once per cell, not per pixel, by picking the first non-transparent
   `attr_map[layer][cell]` exactly as `render_line` does today.

   Mode 10 (320 px, 2x): loop over 320 bytes with the palette LUT and
   write each pixel twice; ~4 cycles/pixel = ~1300 cycles. Or use HSTX
   pixel doubling (c) and write it once.

   Keep `render.c` as the reference implementation and add a host test
   that compares the new RGB332 fast path against
   `render_line` + a straightforward RGB888→RGB332 conversion for all
   modes (the golden-output harness in `tests/host/render_test.c`
   already has the fixtures).

c) **Optional: let HSTX double pixels for the 2x modes.**
   With `expand_shift` set to 8 encoder shifts of 4 bits... the simpler
   trick used by MCUME/PicoMite for RGB332 doubling is to shift by 8
   bits but only 2 shifts per word and feed 16-bit words that hold two
   *identical* pixels, or keep 4 shifts of 8 and DMA each 320-pixel
   half... In practice the cell renderer in (b) already writes 2x
   pixels for free (4 words per cell) and this is not needed for
   performance; only consider it if you move to a 320x240 framebuffer
   (d) and want to avoid the doubling copy.

d) **Alternative: a real framebuffer.**
   A full 640x480 RGB332 buffer is 300 KB and does not fit next to the
   4 × 64 KB Lua heaps in 520 KB SRAM. A 320x240 (77 KB) buffer works
   for modes 0/1/10 only and would force 80-column modes through path
   (a)/(b) anyway, so the ring is the better single design.

e) **Line repeat for the 2x modes.** In the ring design, lines 2k and
   2k+1 are identical for modes 0/1/10; render once and post the same
   buffer twice (track `s_ring_consumed` in *output* lines, render in
   *logical* lines). Halves the average render cost.

f) **Verify with a counter, not by eye.** Keep `s_underruns` and expose
   it via `video_hw_stats()`; print it once per second from the main
   loop during bring-up. Also toggle a spare GPIO around the render
   call and scope it against HSYNC to measure real per-line cost.

---

## Correctness / robustness

### 4. Use-after-free and NULL dereference races with core 1

The scanout on core 0 reads `g_current_video` while core 1 mutates it:
`program_pop()` (`program.c:347`) swaps the pointer and the caller then
frees the popped state (`program.c:322,371`); `video_set_mode()`
(`video.c:52`) sets `mode = PIXEL` before `framebuf` is `malloc`ed;
`video_state_free()` NULLs `framebuf` while `mode` may still read 10.

**How to fix**

- Snapshot once per *frame*, not per line: in the DMA IRQ when
  `s_v_scanline` wraps to 0 (or in the render context at the start of a
  frame) copy `g_current_video` into `s_frame_video`. All lines of a
  frame then come from one state, which also removes half-frame
  mode-switch glitches.
- Deferred free with a frame handshake. Add to `system_state.h`:

  ```c
  volatile uint32_t video_frame_count;      /* core 0 ++ per vsync */
  ```

  and in `program.c`, instead of `free(p->video)` immediately, push the
  pointer onto a small "graveyard" list tagged with
  `video_frame_count + 2`; free entries whose tag has passed (check in
  `program_service()`/scheduler tick on core 1). Two frames guarantees
  the scanout has taken a fresh snapshot. `program_test.c` can test the
  graveyard with a fake frame counter.
- Publish state atomically in `video_set_mode()`: allocate and clear
  `framebuf` *first*, then `__dmb(); v->mode = VIDEO_MODE_PIXEL;`. When
  leaving mode 10, set `v->mode` to the new text mode, `__dmb()`, and
  only then `video_state_free()`. Add `if (!v->framebuf) { black line;
  return; }` to the renderer as a belt-and-braces check.
- Alternatively (more work, but it also solves tearing): give each
  `video_state_t` two map sets and a `front` index; core 1 writes the
  back set and flips `front` after `WaitVSync`; the renderer only ever
  reads `maps[front]`. `video_state_t` is currently ~32 KB; a second
  copy of `char_map`/`attr_map` adds 29 KB per program, which is likely
  too much for four programs, so do this only for the top program's
  state or for the maps of the active mode's size.

### 5. Pixel clock is off-spec (30 MHz, ~71 Hz)

The SDK example comment assumes a 125 MHz `clk_hstx`; on RP2350 the SDK
default `clk_sys` is 150 MHz and `clk_hstx` follows it, so `CLKDIV = 5`
gives a 30 MHz pixel clock: 640x480 at ~71 Hz with a 37.5 kHz line
rate. Many monitors tolerate it, some TVs/capture devices will not lock.

**How to fix**

- `clk_hstx` has only a 2-bit integer divider (`CLOCKS_CLK_HSTX_DIV_INT`,
  values 1..3) and aux sources `clk_sys`, `pll_sys`, `pll_usb`, `gpin`.
  `pll_usb` is 48 MHz (needed for USB stdio) and 150/2 = 75 is wrong, so
  the practical choice is to run the *system* PLL at 126 MHz, which
  yields 25.2 MHz pixel / 252 MHz bit clock, exactly 640x480@60:

  ```c
  /* before stdio_init_all(), in core0/main.c */
  set_sys_clock_khz(126000, true);   /* VCO 1512 MHz, postdiv 6 x 2 */
  ```

  Then `clock_configure_undivided(clk_hstx, 0,
  CLOCKS_CLK_HSTX_CTRL_AUXSRC_VALUE_CLK_SYS, 126 * MHZ)` (the SDK
  already defaults clk_hstx to clk_sys, so this is documentation as
  much as code). `clk_peri` follows `clk_sys`, so re-check the SD SPI
  baud (`spi_set_baudrate` gives the nearest achievable divider; 126/10
  = 12.6 MHz is fine) and update the `System Clock Frequency` printout
  expectations. Lua throughput drops ~16 %.
- If 150 MHz for Lua matters more, keep it and accept 71 Hz, but then
  set `MODE_V_FRONT_PORCH`/porches to a VESA-compatible 640x480@72/75
  timing (e.g. 640x480@75: H 16/64/120, V 1/3/16, 31.5 MHz) so monitors
  recognise a standard mode rather than a stretched 60 Hz one.
- Whichever you choose, record the resulting refresh rate in
  `AGENTS.md` and in `lua.md` (it defines what `WaitVSync` means).

### 6. Handler is in scratch RAM but everything it calls is in flash

`hstx_dma_irq` is `__scratch_x`, yet `render_line`, `pack_rgb332`,
`memset`, `font8x8_basic` and the palette live in XIP flash. Cache
misses caused by the SD or Lua paths make IRQ latency unpredictable.

**How to fix**

- After item 3 the DMA IRQ no longer calls anything; keep it
  `__scratch_x("")` and keep `s_ring` in normal SRAM.
- Mark the render ISR and its helpers `__not_in_flash_func(name)` (or
  `__no_inline_not_in_flash_func`) and put lookup tables in RAM:
  `static uint64_t s_expand[256]` is built at init (RAM by default);
  copy `font8x8_basic` into a RAM array at init (1 KB) or declare it
  without `const` in a `__attribute__((section(".data")))`.
- Bigger hammer: `pico_set_binary_type(SPIComputerOS copy_to_ram)` puts
  the whole image in SRAM. The image plus Lua heaps probably exceeds
  520 KB, so prefer targeted placement.

### 7. No reset of the HSTX/DMA blocks on init

After a watchdog reboot or a future mode change the HSTX FIFO may hold
stale words and a previously running DMA chain may still be armed.

**How to fix**

```c
#include "hardware/resets.h"
reset_unreset_block_num_wait_blocking(RESET_HSTX);
dma_channel_abort(s_dma_ping);
dma_channel_abort(s_dma_pong);
dma_hw->ints1 = (1u << s_dma_ping) | (1u << s_dma_pong);
```

before writing `expand_tmds`. Reset state of `csr` is disabled, so the
`hstx_ctrl_hw->csr = 0;` line can then go (or stay with a comment).

### 8. `board_config.h` pin macros are not used

`HSTX_D0_P_PIN` etc. were added as the source of truth but
`video_hw.c` still hard-codes GPIO 12..19 and `lane_output_bit[] =
{0, 6, 4}`.

**How to fix**

```c
#define HSTX_BIT(pin) ((pin) - 12)
hstx_ctrl_hw->bit[HSTX_BIT(HSTX_CLK_P_PIN)] = HSTX_CTRL_BIT0_CLK_BITS;
hstx_ctrl_hw->bit[HSTX_BIT(HSTX_CLK_N_PIN)] = HSTX_CTRL_BIT0_CLK_BITS |
                                              HSTX_CTRL_BIT0_INV_BITS;
static const uint8_t lane_p[3] = {HSTX_D0_P_PIN, HSTX_D1_P_PIN, HSTX_D2_P_PIN};
static const uint8_t lane_n[3] = {HSTX_D0_N_PIN, HSTX_D1_N_PIN, HSTX_D2_N_PIN};
for (uint lane = 0; lane < 3; lane++) {
    uint32_t sel = (lane * 10u) << HSTX_CTRL_BIT0_SEL_P_LSB |
                   (lane * 10u + 1u) << HSTX_CTRL_BIT0_SEL_N_LSB;
    hstx_ctrl_hw->bit[HSTX_BIT(lane_p[lane])] = sel;
    hstx_ctrl_hw->bit[HSTX_BIT(lane_n[lane])] = sel | HSTX_CTRL_BIT0_INV_BITS;
}
```

Add `_Static_assert`s that all eight pins are in 12..19 and distinct.
Confirm the product board follows the Pico DVI Sock pairing the current
table assumes (D0 on 12/13, CK on 14/15, D2 on 16/17, D1 on 18/19); if
the PCB routes differently, only `board_config.h` changes.

### 9. Colour channel order should be verified on real hardware

`pack_rgb332` produces `BBBGGGRR` (blue 3 bits, red 2 bits), identical
to the SDK example, and the expander routes byte bits 7:5 to lane 2 (D2)
and bits 1:0 to lane 0 (D0, which also carries the syncs and is
nominally the blue TMDS channel).

**How to fix / verify**

- Bring-up test: fill the screen with palette entry pure red
  (`0xFF0000`) and check the monitor. If it shows blue, swap the R and B
  masks in the RGB332 conversion (`(r & 0xe0) | (g & 0xe0) >> 3 |
  (b & 0xc0) >> 6`) and update the palette LUT builder. Do not touch
  `expand_tmds`; the ROT/NBITS values are what make 3+3+2 land on the
  lanes.
- If you would rather have 3 bits of red than blue (human eyes prefer
  it), swap the *lane* assignment instead: `lane_p/lane_n` order
  `{D2, D1, D0}` routes expander lane 0 (bits 1:0) to D2 (red). Either
  way, pin it down in a host test of the conversion function (item 14).

---

## Missing pieces for the rest of the OS

### 10. No vblank / frame signal

`AGENTS.md` promises double-buffered maps with vsync swaps and there is
currently no way for core 1 to know where the beam is.

**How to fix**

- In `hstx_dma_irq`, when `s_v_scanline` wraps to 0, do
  `g_system_state.video_frame_count++` (a plain `volatile uint32_t` in
  `system_state.h`, single writer, so no lock).
- Expose `WaitVSync()` in `sys_lua.c`: read the counter, then yield to
  the scheduler until it changes (do not spin; the scheduler loop on
  core 1 already polls input, so add a "wake at frame N" condition next
  to the timer wakeups). Document it in `lua.md`.
- Also expose `video_hw_scanline()` (returns `s_v_scanline`) for
  "raster" effects and for tests that want to prove a write landed in
  vblank.
- The simulator should implement the same counter from its SDL frame
  loop so programs behave identically.

### 11. HDMI audio data islands (still deferred)

**Pointers**

- Data islands need `HSTX_CMD_RAW` (not `RAW_REPEAT`) bursts of
  pre-encoded TERC4 symbols in the horizontal blanking of each line, so
  the three static command lists become per-line generated lists (about
  60 words each). Keep the list arrays in a ring like the pixel lines.
- Wren6991's PicoDVI `libdvi` (`dvi_serialiser`/`data_packet.c`) has the
  packet/BCH/TERC4 encoding that can be lifted; it is PIO based but the
  symbol generation is independent of the transport.
- Audio sample clock: 48 kHz needs 800 samples/frame at 60 Hz; N/CTS
  values for 25.2 MHz TMDS clock are N=6144, CTS=25200.

### 12. Watchdog interaction

Core 0 feeds the watchdog from its main loop; with the video IRQ taking
most of the core (item 3) the 2 s timeout will be hit.

**How to fix**

- Fix item 3 first; the DMA IRQ then costs ~50 cycles twice per line
  (~1 % of core 0).
- Move the feed out of the main loop anyway: feed from the render
  context once per frame *if* the core 1 heartbeat advanced in the last
  N frames. That keeps the "stuck Lua VM resets the system" guarantee
  even if the main loop is stuck inside a slow SD operation.

---

## Code quality / testing

### 13. Small cleanups

- Remove `s_started`, `HSTX_CMD_RAW` (unless item 11 lands),
  `MODE_H_TOTAL_PIXELS`; make `s_v_scanline`, `s_dma_pong`,
  `s_vactive_command_posted` `static volatile` or comment that they are
  IRQ-private.
- Add a comment on the double `csr` write (disable before reconfigure).
- `#if defined(PICO_RP2350)` is redundant with `SPICOMPUTER_HAS_HDMI`,
  which `board_config.h` only defines for RP2350 profiles; keep one.

### 14. Host-test the parts that do not need hardware

- Move the RGB332 packing / cell expansion into `render332.c` (platform
  neutral, no SDK includes) and add `tests/host/render332_test.c`
  comparing it against `render_line` + reference conversion for every
  mode, several palettes, overlay layers, inverted cells and custom
  tiles.
- Abstract the DMA register writes behind a tiny struct
  (`struct scanout_ops { void (*post)(const uint32_t *src, uint n); }`)
  so `prepare_channel` / the scanline state machine can be driven on
  the host for 525 × 2 steps and asserted to emit exactly 10 vsync-on
  lines, 33 back-porch lines, 480 (cmdlist, pixel) pairs and 2 front
  porch lines in the right order.
- Add a ring underrun test: simulate a slow renderer and assert the
  consumer repeats the last good line (better than showing garbage).

### 15. Link libraries explicitly

Add `hardware_dma hardware_irq hardware_resets hardware_clocks` (the
last is already there) to `target_link_libraries(SPIComputerOS ...)`.

### 16. Document the resource map

Add a table to `AGENTS.md`: DMA channels (video 2, SD 2), DMA IRQ lines
(video `DMA_IRQ_1`, SD `DMA_IRQ_0`), alarm/timer usage (input scan),
IRQ priorities (video > input > SPI), spare IRQ used for the render
ISR, and the `clk_sys`/pixel clock decision from item 5. Update the
"Display" row to describe the line-ring design once implemented.

---

## Suggested order of work

1. Items 1, 2, 7, 15 (claim channels, `DMA_IRQ_1`, reset, link libs):
   half a day, gets a stable black/solid-colour screen on the bench.
2. Item 5 (126 MHz) and item 9 (colour bar test) while the display is
   simple.
3. Item 3 (ring + cell renderer) with the host test from item 14.
4. Items 4 and 10 (frame counter, deferred free, `WaitVSync`).
5. Items 6, 8, 12, 13, 16.

---

## Implementation status (updated)

All of items 1-10, 13-16 plus the item 3 design are implemented; the
firmware builds clean for the product board profile and the host suite
(including new tests) passes.

| Item | Status | Where |
|---|---|---|
| 1 DMA channels claimed | done | `core0/video_hw.c` (`dma_claim_unused_channel`), init order in `core0/main.c` |
| 2 own IRQ line | done | video on `DMA_IRQ_2` (`ints2`/`inte2`), SD keeps `DMA_IRQ_0` |
| 3 line ring + cell renderer | done | `scanout.c` kernel, ring in `video_hw.c`, `render332.c` writes two words per cell |
| 4 races / deferred free | done | `program_retire_reap()` (two frames), release fences in `video_set_mode`, NULL `framebuf` guard in both renderers |
| 5 pixel clock | done | `apply_sys_clock()` (378 MHz, vreg 1.30 V) in `core0/main.c`; `configure_hstx_clock()` divides `clk_hstx` down to 126 MHz from whatever `clk_sys` is |
| 6 flash vs scratch | done | DMA IRQ and post path `__not_in_flash_func`; ring/tables in RAM (BSS) |
| 7 HSTX reset | done | `reset_unreset_block_num_wait_blocking(RESET_HSTX)` |
| 8 pin macros | done | `HSTX_*_PIN` from `board_config.h`, `_Static_assert`s |
| 9 colour order | done + host test | `render332_rgb` test asserts R=7:5, G=4:2, B=1:0 (still worth a scope check on the board) |
| 10 vblank signal | done | `g_system_state.video_frame_count`, `WaitVSync([ms])` in `sys_lua.c`, documented in `lua.md` |
| 14 host tests | done | `tests/host/scanout_test.c` (frame structure, cadence, underruns, RGB332 vs reference); `program_test.c` covers the retire queue and WaitVSync |
| 15 link libs | done | `hardware_dma`/`hardware_irq`/`hardware_resets` |
| 16 resource map | done | "Implemented (Phase 7, video)" in `AGENTS.md` |
| 11 audio data islands | open | still deferred; the blanking command lists can grow per-line lists |
| 12 watchdog | mitigated | the render ISR runs at 0x40 and returns between lines; main loop still feeds on progress |
| 13 cleanups | done | dead defines removed; tables sized from the mode geometry |

Remaining work is hardware bring-up itself: scope the colour order on a
monitor, confirm the 378 MHz overclock is stable on the board at 1.30 V
(watch the `HSTX:` and `System Clock Frequency` lines in the boot log),
and watch the underrun diagnostic (`video: ... underruns` once per
second on the USB console) under load. Item 3c (HSTX pixel doubling) and item 3d (framebuffer) were
left out deliberately: the cell renderer meets the budget without them.

---

## Follow-up review (ring budget and overclock)

A second review found bugs the first round of tests could not see, because
they checked the *post order* rather than the data:

1. **Ring rebase underflowed the producer budget.** At the end of an
   active region the code set `slots_produced = rows_produced` while
   resetting `rows_consumed = 0`, so `scanout_ring_space()` (an unsigned
   difference) wrapped to a huge value and the producer rendered the
   *entire next frame* inside one vblank, overwriting ring slots as it
   went. The ring ended up holding only the last 8 rows, which the
   consumer then scanned out for the whole screen.
2. **The producer budget ignored the DMA pipeline.** A buffer posted at
   completion IRQ S is transferred between IRQ S+1 and S+2 (the other
   channel's command list runs first) and the buffer posted at S-1 is
   being read while the render ISR runs, so the two most recent rows are
   always in flight. Allowing the producer to run `RING - 1` rows ahead
   let it overwrite a slot mid-transfer.
3. **Frame geometry was refreshed too late.** `scanout_frame_begin()` ran
   on the first active line, after the vblank prefetch had already filled
   the ring with the previous frame's mode.
4. **The sequencer ran from flash.** `scanout_step()` is called every
   scanline; an XIP cache miss there could miss a line.

Fixes: the ring is now row-based with a single `rows_published` counter
that (with `rows_consumed`) resets at the rebase; the producer budget is
`SCANOUT_RING_AHEAD = RING - 2`; `scanout_frame_begin()` is called from
the rebase itself; and `scanout_step()`/`scanout_frame_begin()` are
`__not_in_flash_func`. The host tests now tag every slot with its row and
check the data of every post plus an in-flight invariant (a posted buffer
must not change while armed); widening the budget back to `RING` makes
those tests fail, so they do cover the bug.

**Overclock.** `core0/main.c` now runs `clk_sys` at 378 MHz (1.30 V) to
give the render ISR and the RPC loop 3x the cycles per scanline. 378 MHz
is the highest clock whose HSTX divider (1..3) still yields the exact
25.2 MHz pixel clock; 400 MHz is available via
`-DSPICOMPUTER_SYS_CLOCK_KHZ=400000` at the cost of a ~63.5 Hz refresh.
The QSPI flash divider and RX sample delay are scaled with the clock (from
SRAM, before the jump) so the flash never runs faster than at boot and
stays sampled mid-eye; an unattainable target falls back to 126 MHz.

---

## Core split: video on its own core

With the CPU at 378 MHz the scanout still had to share core 0 with the
SD/FatFs service, the input tick, stdio and the watchdog, so a scanline
could be delayed by the XIP cache and the bus traffic those generate (and
every cross-core service needed a queue or a semaphore). The split is now:

- **Core 0 = video only**: clock setup, `video_hw_init()` (HSTX, DMA
  claim, DMA_IRQ_2 + the render ISR), `multicore_launch_core1_with_stack()`,
  then WFI. No stdio, no timers, no SD.
- **Core 1 = everything else**: stdio, FatFs + SD SPI (DMA_IRQ_0), the
  1 kHz input tick, the watchdog and the Lua scheduler.

What that bought and what it cost:

- The filesystem went from a blocking cross-core RPC (request slot +
  response slot + staging buffer + multicore semaphore + atomics) to a
  direct call: `rpc_set_local_handler(fs_core0_execute)` and
  `rpc_call()` dispatches inline. The op codes and the 4 KB staging
  buffer stay (they are the contract between `fs_lua.c` and
  `fs_core0.c`), and the slot transport is retained for builds that
  still split the two (host harness, desktop simulator), so nothing
  external breaks.
- The input ring lost its atomics: producer (1 kHz IRQ) and consumer
  (scheduler) are the same core, and only the index words are touched
  from the IRQ.
- `system_state` shrank to the input ring (now core-1-local, kept in
  place for source compatibility) plus the one genuinely shared object:
  `video_frame_count`.
- Affinity rules now matter and are documented in `AGENTS.md`: IRQs are
  enabled on the core that should take them; the alarm pool belongs to
  whichever core uses it first (so core 0 uses `busy_wait_us`, never
  `sleep_ms`); stdio is core 1's; the watchdog is fed by core 1 while its
  loop steps and core 0 keeps producing frames.
- Stacks: the SDK's core-1 stack lives in the 4 KB SCRATCH_X, which is
  far too small now that FatFs shares that core, so core 1 gets a 16 KB
  stack in main SRAM (`multicore_launch_core1_with_stack`, with
  `PICO_CORE1_STACK_SIZE=0`), and core 0's SCRATCH_Y stack is 4 KB.
- Tests: the host harness gained a direct-dispatch test (framebuffer-free
  filesystem round trip with no service pumping) alongside the existing
  slot-transport tests, and the full suite still passes.

Residual risk to watch on hardware: core 1 now runs IRQs (SD DMA, input
timer, USB) alongside the Lua VM, which is a behaviour change for the VM
(previously IRQ-free); the input ISR is short and the SD completion IRQ
only releases the driver's semaphore, but a stalled USB or SD IRQ is now
the OS core's problem rather than the video core's.
