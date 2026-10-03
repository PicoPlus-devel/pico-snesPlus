/* Pico-snes9x+ port: host-side stubs that snes9x's core expects the port
 * to provide. Input mapping (S9xReadJoypad) bridges the framework's unified
 * GamePadState to SNES button bits. Display init parks GFX.Screen on the
 * caller-provided HSTX framebuffer pointer. */

#include <stdio.h>

extern "C" {
#include "snes9x.h"
#include "memmap.h"
#include "gfx.h"
#include "display.h"
#include "ppu.h"
}
#include "port_alloc.h"

#include "gamepad.h"
#include "nespad.h"
#include "wiipad.h"
#include "settings.h"

/* Refreshed once per frame by host_tick() in main.cpp. */
extern uint16_t wiipad_raw_cached;
extern uint32_t g_rapid_fire_counter;

#if RENDER_TO_FB
/* Strip renderer. S9xUpdateScreen (gfx.c) renders every scanline block in
 * chunks of S9X_STRIP_ROWS rows into the four small SRAM staging strips
 * below, then calls s9x_port_strip_copyout() to memcpy the FINISHED rows
 * into the centered window of the 320x240 HSTX framebuffer. Two effects:
 *   - scan-out only ever sees fully-composited pixels (old frame or new),
 *     never the backdrop/partial-layer/pre-color-math intermediate states
 *     that a direct-to-framebuffer render exposes as rolling flicker;
 *   - screen, subscreen AND both Z buffers live in SRAM, so the per-pixel
 *     render traffic that used to hit PSRAM (screen writes, Z clears/RMW,
 *     subscreen composite, blit read-back) is gone entirely.
 * The strips keep the native 512-byte pitch: GFX.Delta / GFX.DepthDelta
 * stay constant because all four bases get the same per-chunk offset.
 * Works because the renderer already supports arbitrary [StartY,EndY]
 * blocks (mid-frame FLUSH_REDRAW blocks do this today) — chunking just
 * splits them finer; seam tiles use the existing clipped-tile paths. */
#include "hstx.h"
#include "pico/platform.h"

#define FB_WIDTH  320
#define FB_HEIGHT 240

/* Strip bases (row 0 = chunk start). +1 guard row: the force-lores guard
 * (gfx.c) contains mode 5/6, but its double-width writers still spill up
 * to one row past the last chunk row. */
#define STRIP_GUARD_ROWS (S9X_STRIP_ROWS + 1)

static uint8_t *strip_screen;
static uint8_t *strip_sub;
static uint8_t *strip_z;
static uint8_t *strip_subz;

extern "C" {
int s9x_port_max_endy = SNES_HEIGHT - 1;
/* Centered SNES window inside the framebuffer (copy-out target, also the
 * FPS overlay target in main.cpp). */
uint16_t *s9x_port_fb_window = nullptr;
/* 239x128 scratch for S9xSetupOBJ's FirstSprite+Y case — it used to abuse
 * GFX.SubScreen, which is now a strip far too small for it. PSRAM, same
 * tier SubScreen was in before. */
uint8_t *s9x_port_objonline = nullptr;
/* Optional hook fired just before the TOP strip (absolute row 0) is copied
 * to the single-buffered framebuffer. main.cpp registers it to stamp the FPS
 * overlay INTO the strip, so the digits are published together with the
 * frame's pixels in one copy-out. Stamping the overlay into the live FB
 * *after* the copy-out (as the port used to) left a sub-frame window where
 * the copy-out had already overwritten last frame's digits with game pixels
 * but the re-stamp had not run yet — scan-out caught that gap and the overlay
 * flickered. Passed the strip base (uint16_t), its stride in pixels, and the
 * chunk's absolute [block_start, block_end] row range so it can stamp only
 * the overlay rows this chunk publishes (strip physical row 0 == block_start;
 * a game may split the top of the frame into several short redraw chunks). */
void (*s9x_port_strip_top_hook)(uint16_t *strip, int stride,
                                int block_start, int block_end) = nullptr;
}

/* Framebuffer row of SNES row 0 (the window's top margin). */
static int fb_window_top;

/* (Re-)anchor the framebuffer window for the current PPU.ScreenHeight
 * (224 or 239 — the overscan bit flips it at runtime; main.cpp calls this
 * again when it changes). Exports the last row the copy-out may write so
 * S9xUpdateScreen can clamp a mid-frame flip. */
extern "C" void s9x_port_anchor_screen(void)
{
    int h = PPU.ScreenHeight ? PPU.ScreenHeight
                             : (Settings.PAL ? SNES_HEIGHT_EXTENDED : SNES_HEIGHT);
    const int marginTop  = (FB_HEIGHT - h) / 2;
    const int marginLeft = (FB_WIDTH - SNES_WIDTH) / 2;
    s9x_port_fb_window = (uint16_t *)hstx_getframebuffer()
                       + marginTop * FB_WIDTH + marginLeft;
    s9x_port_max_endy = FB_HEIGHT - marginTop - 1;
    fb_window_top = marginTop;
}

/* -------------------------------------------------------------------------
 * Tear guard. The framebuffer is single-buffered: core1's scanline IRQ reads
 * it one row per output line (every row twice, 640x480 line-doubled) while
 * core0 copies finished strips into it. Rendering a strip takes about as
 * long as the beam needs to cross 16 rows, so without a guard the copy-out
 * and the beam leapfrog each other down the screen and a single refresh
 * shows old and new frame alternating at strip seams: the stair-stepped
 * edges in scrolling games. Two parts:
 *
 *   - strip_wait_for_beam() never publishes a strip the beam is inside or
 *     about to enter. Once the beam has overtaken the copy-out it stays
 *     ahead of it for the rest of that refresh, so a refresh shows at most
 *     one seam between two frames instead of one per strip.
 *   - The tear tracker below measures, per rendered frame, where each strip
 *     landed relative to the refresh that will first show it. main.cpp's
 *     vsync-phase pacer uses that to move the frame's start until all its
 *     strips land between the same two refreshes, which removes the
 *     remaining seam.
 *
 * The beam position comes from wrapping pico_shared's scanline callback
 * (hstx.c), which runs once per output line just before the row is read.
 * Wrapping it keeps this out of the shared driver. */
extern "C" void scanline_callbackfunc(uint32_t v_scanline, uint32_t active_line, uint32_t *buff);

/* Output lines per refresh, and from the vsync tick (video_frame_count++
 * at v_scanline == MODE_V_FRONT_PORCH) to active line 0. */
#define BEAM_LINES   MODE_V_TOTAL_LINES
#define BEAM_ACTIVE0 (MODE_V_TOTAL_LINES - MODE_V_ACTIVE_LINES - MODE_V_FRONT_PORCH)
/* Rows above a strip that already count as "inside": covers an interrupt
 * on core0 delaying the copy while the beam closes in (4 rows ~ 250 us). */
#define TEAR_GUARD_ROWS 4
/* Never wait longer than this for the beam: if scan-out stalls (HSTX
 * resync) the copy-out must not hang core0. A real wait is <= ~1.3 ms. */
#define TEAR_WAIT_MAX_US 3000u

/* Written by core1 per active line, in this order. */
static volatile uint32_t beam_line;   /* active line, 0..MODE_V_ACTIVE_LINES-1 */
static volatile uint32_t beam_cnt;    /* video_frame_count during that line */
static volatile uint32_t beam_time;   /* time_us_32() at that line */

static void __not_in_flash_func(beam_scanline_cb)(uint32_t v_scanline, uint32_t active_line,
                                                  uint32_t *buff)
{
    beam_line = active_line;
    beam_cnt  = video_frame_count;
    beam_time = time_us_32();
    scanline_callbackfunc(v_scanline, active_line, buff);
}

/* Call once after Frens::initAll (which installs the plain callback). */
extern "C" void s9x_port_beam_install(void)
{
    video_output_set_scanline_callback(beam_scanline_cb);
}

/* Beam position in output lines on a continuous scale: BEAM_LINES per
 * refresh, a multiple of BEAM_LINES at each vsync tick. Wraps, so only
 * differences mean anything. Extrapolated from the last active line at the
 * fixed 640x480 line rate (800 px at 25.2 MHz = 31.746 us = 2000/63 us), so
 * it keeps counting through vertical blanking. */
extern "C" uint32_t __not_in_flash_func(s9x_port_beam_pos)(void)
{
    uint32_t line, cnt, t;
    do {
        line = beam_line;
        cnt  = beam_cnt;
        t    = beam_time;
    } while (line != beam_line || cnt != beam_cnt);
    uint32_t dt = time_us_32() - t;
    if (dt > 100000u) dt = 100000u;   /* scan-out stalled; keep the math sane */
    return cnt * BEAM_LINES + BEAM_ACTIVE0 + line + dt * 63u / 2000u;
}

/* Per-frame tear tracking. psi = when a strip was published, in lines,
 * relative to the moment the reference refresh read the strip's first row:
 * psi in (-BEAM_LINES, 0) means it was published ahead of that refresh,
 * (0, BEAM_LINES) between it and the next one, and so on. A frame is whole
 * on screen exactly when all its strips share one such interval. */
static uint32_t tf_ref;               /* BEAM_LINES * refresh count at frame start */
static int32_t  tf_psi_min, tf_psi_max;
static bool     tf_any;

/* Counters for the optional once-per-second report (main.cpp, TEAR_STATS). */
static uint32_t ts_frames, ts_torn, ts_waits, ts_wait_us, ts_span_max;

static inline int32_t floor_div_lines(int32_t v)
{
    return v >= 0 ? v / BEAM_LINES : -((-v + BEAM_LINES - 1) / BEAM_LINES);
}

/* Bracket one rendered S9xMainLoop. end returns false when the frame
 * published nothing; otherwise *psi_mid is the midpoint of its strips' psi
 * range, which the pacer steers towards the middle of an interval, and
 * *torn says whether some refresh showed the frame only in part. */
extern "C" void s9x_port_tear_frame_begin(void)
{
    tf_ref = video_frame_count * BEAM_LINES;
    tf_any = false;
}

extern "C" bool s9x_port_tear_frame_end(int32_t *psi_mid, bool *torn)
{
    if (!tf_any) return false;
    ts_frames++;
    *torn = floor_div_lines(tf_psi_min) != floor_div_lines(tf_psi_max);
    if (*torn) ts_torn++;
    uint32_t span = (uint32_t)(tf_psi_max - tf_psi_min);
    if (span > ts_span_max) ts_span_max = span;
    *psi_mid = tf_psi_min + (tf_psi_max - tf_psi_min) / 2;
    return true;
}

/* Read and clear the counters: rendered frames, frames that reached the
 * screen torn, strips that had to wait for the beam, the total wait, and
 * the widest psi range of a frame (wider than a refresh, BEAM_LINES, can
 * not be placed tear-free by any start phase). */
extern "C" void s9x_port_tear_stats(uint32_t *frames, uint32_t *torn, uint32_t *waits,
                                    uint32_t *wait_us, uint32_t *span_max)
{
    *frames = ts_frames; *torn = ts_torn; *waits = ts_waits; *wait_us = ts_wait_us;
    *span_max = ts_span_max;
    ts_frames = ts_torn = ts_waits = ts_wait_us = ts_span_max = 0;
}

/* Block until publishing framebuffer rows [first, last] cannot meet the
 * beam: either the beam has not reached the picture in this refresh yet
 * (copy-out ahead) or it is past the strip (copy-out behind). */
static void __not_in_flash_func(strip_wait_for_beam)(int first, int last)
{
    uint32_t t0 = 0;
    bool waited = false;
    for (;;) {
        if (beam_cnt != video_frame_count)
            break;                       /* vblank: this refresh has not started the picture */
        int row = (int)(beam_line >> 1);
        if (row > last || row < first - TEAR_GUARD_ROWS)
            break;
        uint32_t now = time_us_32();
        if (!waited) {
            waited = true;
            t0 = now;
        } else if (now - t0 > TEAR_WAIT_MAX_US) {
            break;
        }
        tight_loop_contents();
    }
    if (waited) {
        ts_waits++;
        ts_wait_us += time_us_32() - t0;
    }
}

/* Point the GFX buffers at the strips so absolute rows [row, row+N-1]
 * land at strip rows [0, N-1]. The intermediate (base - row*pitch) points
 * below the allocation; only rows >= row are ever dereferenced.
 * Both per-chunk helpers run from RAM (__not_in_flash_func): they are
 * called 14+ times per rendered frame from the SRAM-resident gfx.c code
 * and must not fetch through the XIP/QMI bus they exist to relieve. */
extern "C" void __not_in_flash_func(s9x_port_strip_repoint)(uint32_t row)
{
    GFX.Screen     = strip_screen - (size_t)row * SNES_WIDTH * 2;
    GFX.SubScreen  = strip_sub    - (size_t)row * SNES_WIDTH * 2;
    GFX.ZBuffer    = strip_z      - (size_t)row * SNES_WIDTH;
    GFX.SubZBuffer = strip_subz   - (size_t)row * SNES_WIDTH;
}

/* Copy finished rows [start_row, end_row] from the screen strip into the
 * framebuffer window. SRAM->SRAM, ~8 KB per 16-row chunk. */
extern "C" void __not_in_flash_func(s9x_port_strip_copyout)(uint32_t start_row, uint32_t end_row)
{
    /* Stamp the overlay into any chunk overlapping the overlay band (rows
     * 0..7) before publishing it, so it rides out with the frame's own pixels
     * (no post-copy-out re-stamp gap) even when the top of the frame is split
     * across several short redraw chunks. 8 == overlay height in main.cpp. */
    if (start_row < 8 && s9x_port_strip_top_hook)
        s9x_port_strip_top_hook((uint16_t *)strip_screen, SNES_WIDTH,
                                (int)start_row, (int)end_row);

    const int first = fb_window_top + (int)start_row;
    strip_wait_for_beam(first, fb_window_top + (int)end_row);
    int32_t psi = (int32_t)(s9x_port_beam_pos() - (tf_ref + BEAM_ACTIVE0 + 2u * (uint32_t)first));
    if (!tf_any) {
        tf_any = true;
        tf_psi_min = tf_psi_max = psi;
    } else {
        if (psi < tf_psi_min) tf_psi_min = psi;
        if (psi > tf_psi_max) tf_psi_max = psi;
    }

    const uint8_t *src = strip_screen;
    uint16_t      *dst = s9x_port_fb_window + start_row * FB_WIDTH;
    for (uint32_t y = start_row; y <= end_row; y++) {
        memcpy(dst, src, SNES_WIDTH * 2);
        src += SNES_WIDTH * 2;
        dst += FB_WIDTH;
    }
}

/* SRAM-first with PSRAM fallback (needs PICO_MALLOC_PANIC=0), boot log
 * prints the tier — same pattern as FillRAM/MapInfo in memmap.c. A strip
 * in PSRAM still fixes the flicker but forfeits its share of the perf
 * win, so the log matters. FillRAM is forced to PSRAM under RENDER_TO_FB
 * (memmap.c) precisely so all four strips fit in SRAM. */
static uint8_t *strip_alloc(size_t bytes, const char *name)
{
    uint8_t *p = (uint8_t *)port_alloc_sram(bytes);
    if (p) {
        printf("%s (%u B) in SRAM\n", name, (unsigned)bytes);
        return p;
    }
    p = (uint8_t *)port_alloc_psram(bytes);
    printf("%s (%u B) in PSRAM (SRAM heap full)\n", name, (unsigned)bytes);
    return p;
}

extern "C" bool S9xInitDisplay(void)
{
    GFX.Pitch  = SNES_WIDTH * 2;   /* native 512 — strips decouple us from the fb stride */
    GFX.ZPitch = SNES_WIDTH;
    s9x_port_anchor_screen();

    /* Allocated hottest first, so that when the SRAM heap is short it is
     * the least used strip that ends up in PSRAM rather than whichever one
     * happens to be asked for when the heap runs out. The main Z buffer is
     * read for every candidate pixel of every layer and sprite; the screen
     * strip is written on every winning pixel and read in full by each
     * copy-out; the sub-screen pair is only touched when a game blends with
     * the sub-screen, and of those its Z buffer is read per pixel by both
     * passes while the sub-screen itself is read only where blending hits. */
    strip_screen = strip_alloc((size_t)STRIP_GUARD_ROWS * SNES_WIDTH * 2, "strip-screen");
    strip_z      = strip_alloc((size_t)STRIP_GUARD_ROWS * SNES_WIDTH,     "strip-z");
    strip_subz   = strip_alloc((size_t)STRIP_GUARD_ROWS * SNES_WIDTH,     "strip-subz");
    strip_sub    = strip_alloc((size_t)STRIP_GUARD_ROWS * SNES_WIDTH * 2, "strip-sub");
    s9x_port_objonline = (uint8_t *)port_alloc_psram((size_t)SNES_HEIGHT_EXTENDED * 128);

    GFX.Screen     = strip_screen;
    GFX.SubScreen  = strip_sub;
    GFX.ZBuffer    = strip_z;
    GFX.SubZBuffer = strip_subz;
    return strip_screen && strip_sub && strip_z && strip_subz && s9x_port_objonline;
}

extern "C" void S9xDeinitDisplay(void)
{
    port_alloc_free(strip_screen); strip_screen = nullptr;
    port_alloc_free(strip_sub);    strip_sub    = nullptr;
    port_alloc_free(strip_z);      strip_z      = nullptr;
    port_alloc_free(strip_subz);   strip_subz   = nullptr;
    port_alloc_free(s9x_port_objonline); s9x_port_objonline = nullptr;
    GFX.Screen = GFX.SubScreen = GFX.ZBuffer = GFX.SubZBuffer = nullptr;
}
#else
/* Private PSRAM screen buffer. snes9x renders here at native 256-wide
 * pitch; main.cpp blits it into the HSTX framebuffer after S9xMainLoop
 * returns. Decoupling render from scan-out eliminates the lower-third
 * tearing caused by HSTX reading the framebuffer mid-PPU-write. Cost is
 * one PSRAM-to-SRAM memcpy per visible frame (~240 KB/s) which fits
 * comfortably in the vblank-to-bottom-of-content window. */
uint16_t *g_snes_private_screen = nullptr;

extern "C" bool S9xInitDisplay(void)
{
    const size_t pitch_bytes = (size_t)SNES_WIDTH * 2;                  /* 512 */
    const size_t z_stride    = pitch_bytes >> 1;                        /* 256, matches S9xInitGFX */

    g_snes_private_screen = (uint16_t *)port_alloc_psram(pitch_bytes * SNES_HEIGHT_EXTENDED);

    GFX.Pitch  = pitch_bytes;
    GFX.ZPitch = z_stride;
    GFX.Screen = (uint8_t *)g_snes_private_screen;
    GFX.SubScreen  = (uint8_t *)port_alloc_psram(pitch_bytes * SNES_HEIGHT_EXTENDED);
    GFX.ZBuffer    = (uint8_t *)port_alloc_psram(z_stride    * SNES_HEIGHT_EXTENDED);
    GFX.SubZBuffer = (uint8_t *)port_alloc_psram(z_stride    * SNES_HEIGHT_EXTENDED);
    return GFX.Screen && GFX.SubScreen && GFX.ZBuffer && GFX.SubZBuffer;
}

extern "C" void S9xDeinitDisplay(void)
{
    port_alloc_free(g_snes_private_screen); g_snes_private_screen = nullptr;
    port_alloc_free(GFX.SubScreen);  GFX.SubScreen  = nullptr;
    port_alloc_free(GFX.ZBuffer);    GFX.ZBuffer    = nullptr;
    port_alloc_free(GFX.SubZBuffer); GFX.SubZBuffer = nullptr;
    GFX.Screen = nullptr;
}
#endif /* RENDER_TO_FB */

/* USB HID/XInput pads (io::GamePadState) -> SNES joypad bits. */
static uint32_t pad_to_snes(const io::GamePadState &pad)
{
    uint32_t out = 0;   /* snes9x's S9xUpdateJoypads sets the high bits itself. */

    if (pad.buttons & io::GamePadState::Button::UP)     out |= SNES_UP_MASK;
    if (pad.buttons & io::GamePadState::Button::DOWN)   out |= SNES_DOWN_MASK;
    if (pad.buttons & io::GamePadState::Button::LEFT)   out |= SNES_LEFT_MASK;
    if (pad.buttons & io::GamePadState::Button::RIGHT)  out |= SNES_RIGHT_MASK;

    if (pad.buttons & io::GamePadState::Button::A)      out |= SNES_A_MASK;
    if (pad.buttons & io::GamePadState::Button::B)      out |= SNES_B_MASK;
    if (pad.buttons & io::GamePadState::Button::X)      out |= SNES_X_MASK;
    if (pad.buttons & io::GamePadState::Button::Y)      out |= SNES_Y_MASK;
    if (pad.buttons & io::GamePadState::Button::L)      out |= SNES_TL_MASK;
    if (pad.buttons & io::GamePadState::Button::R)      out |= SNES_TR_MASK;
    if (pad.buttons & io::GamePadState::Button::START)  out |= SNES_START_MASK;
    if (pad.buttons & io::GamePadState::Button::SELECT) out |= SNES_SELECT_MASK;

    return out;
}

#if NES_PIN_CLK != -1
/* GPIO NES/SNES pad. nespad_states_ext is in SNES serial order (bit0=B ...
 * bit11=R), which is the SNES joypad register order mirrored: serial bit i
 * == register bit 15-i. A NES pad only delivers bits 0-7 (A,B,Select,
 * Start,dpad), so its A lands on SNES B and its B on SNES Y — the natural
 * positional feel (jump/run in most games). */
static uint32_t nespad_to_snes(uint16_t raw)
{
    uint32_t out = 0;
    for (int i = 0; i < 12; i++)
    {
        if (raw & (1u << i))
            out |= 1u << (15 - i);
    }
    return out;
}
#endif

#if WII_PIN_SDA >= 0 && WII_PIN_SCL >= 0
/* Wii Classic / SNES-Classic-mini pad; encoding documented in wiipad.h.
 * Labels equal SNES positions on these pads, so this is a 1:1 map. */
static uint32_t wiipad_to_snes(uint16_t v)
{
    uint32_t out = 0;
    if (v & (1 << 0))  out |= SNES_A_MASK;
    if (v & (1 << 1))  out |= SNES_B_MASK;
    if (v & (1 << 2))  out |= SNES_SELECT_MASK;
    if (v & (1 << 3))  out |= SNES_START_MASK;
    if (v & (1 << 4))  out |= SNES_UP_MASK;
    if (v & (1 << 5))  out |= SNES_DOWN_MASK;
    if (v & (1 << 6))  out |= SNES_LEFT_MASK;
    if (v & (1 << 7))  out |= SNES_RIGHT_MASK;
    if (v & (1 << 8))  out |= SNES_X_MASK;
    if (v & (1 << 9))  out |= SNES_Y_MASK;
    if (v & (1 << 10)) out |= SNES_TL_MASK;
    if (v & (1 << 11)) out |= SNES_TR_MASK;
    return out;
}
#endif

/* Set by main.cpp when the in-game menu hands control back. The button the
 * player used to confirm a menu item is normally still physically down, and
 * without this the game sees it on its first frames. Harmless on most carts,
 * not on the SPC7110 ones: their power-on check program reads A at startup as
 * "run MODE 1 again", so an in-game Reset between diagnostic stages restarts
 * the diagnostic instead of advancing, and Super Power League 4 can never be
 * reached. Suppress each port until it reads clear -- per port, so a second
 * controller that is not being held keeps working immediately. */
extern "C" { volatile bool g_pad_ignore_request = false; }

extern "C" uint32_t S9xReadJoypad(int32_t port)
{
    if (port < 0 || port > 1) return 0;

    static bool ignore[2] = { false, false };
    if (g_pad_ignore_request) {
        ignore[0] = ignore[1] = true;
        g_pad_ignore_request = false;
    }

    uint32_t out = pad_to_snes(io::getCurrentGamePadState(port));

    /* Sibling convention (pico-infonesPlus): with a USB pad connected the
     * GPIO NES/SNES and Wii Classic pads act as player 2, otherwise they
     * are player 1 (resp. players 1 and 2 for two GPIO pads). */
    const bool usb = io::getCurrentGamePadState(0).isConnected();
#if NES_PIN_CLK != -1
    if (usb)
    {
        if (port == 1)
            out |= nespad_to_snes(nespad_states_ext[0] | nespad_states_ext[1]);
    }
    else
    {
        out |= nespad_to_snes(nespad_states_ext[port]);
    }
#endif
#if WII_PIN_SDA >= 0 && WII_PIN_SCL >= 0
    if (port == (usb ? 1 : 0))
        out |= wiipad_to_snes(wiipad_raw_cached);
#endif

    /* Still holding whatever closed the menu: feed the game nothing until it
     * is let go. Checked after every source is merged in, so a button held on
     * the GPIO or Wii pad counts too. */
    if (ignore[port])
    {
        if (out) return 0;
        ignore[port] = false;
    }

    /* Rapid fire on A/B (menu setting) — 15 presses/sec, same cadence as
     * the sibling emulators. */
    if (g_rapid_fire_counter & 2)
    {
        if (settings.flags.rapidFireOnA) out &= ~SNES_A_MASK;
        if (settings.flags.rapidFireOnB) out &= ~SNES_B_MASK;
    }

    return out;
}

/* SNES Mouse (Mario Paint) — fed from the USB HID mouse state pico_shared
 * accumulates (hid_app.cpp). The real peripheral is a pure delta device, so
 * no absolute cursor is kept here: the core only diffs what we return
 * against its own IPPU.PrevMouseX/Y (which it then sets to our value), so
 * returning PrevMouse plus the pending motion hands it exact deltas and
 * stays in sync with the game's cursor by construction — across the game's
 * own edge clamping, its speed scaling, and S9xReset re-centering. (A
 * screen-clamped mirror cursor drifts against the game's cursor at every
 * edge rub and makes screen borders intermittently unreachable.)
 * The divisor tames modern 800-1600 dpi mice; the queue keeps sub-divisor
 * remainders so slow movements aren't truncated away, and saturates at one
 * max-rate frame (+-63 SNES counts) like the real mouse's motion counter.
 * Runs once per frame from S9xMainLoop on core0 — same core as tuh_task(),
 * so no locking needed. */
#define SNES_MOUSE_SENS_DIV 2

extern "C" bool S9xReadMousePosition(int32_t which1, int32_t *x, int32_t *y, uint32_t *buttons)
{
    if (which1 != 0)
        return false;
    io::MouseState &m = io::getCurrentMouseState();
    if (!m.connected)
        return false;

    static int32_t qx, qy; /* pending motion, raw HID counts */
    qx += m.dx;
    qy += m.dy;
    m.dx = m.dy = m.wheel = 0;

    const int32_t qcap = 63 * SNES_MOUSE_SENS_DIV;
    if (qx > qcap) qx = qcap; else if (qx < -qcap) qx = -qcap;
    if (qy > qcap) qy = qcap; else if (qy < -qcap) qy = -qcap;

    const int32_t step_x = qx / SNES_MOUSE_SENS_DIV;
    const int32_t step_y = qy / SNES_MOUSE_SENS_DIV;
    qx -= step_x * SNES_MOUSE_SENS_DIV;
    qy -= step_y * SNES_MOUSE_SENS_DIV;

    *x = IPPU.PrevMouseX[0] + step_x;
    *y = IPPU.PrevMouseY[0] + step_y;
    *buttons = m.buttons & 3; /* TinyUSB bit0=left, bit1=right — same layout the core packs */
    return true;
}

extern "C" bool S9xReadSuperScopePosition(int32_t *, int32_t *, uint32_t *)    { return false; }
extern "C" bool JustifierOffscreen(void)                                       { return true; }
extern "C" void JustifierButtons(uint32_t *)                                   { }
extern "C" void S9xToggleSoundChannel(int32_t)                                 { }
/* S9xNextController is defined by snes9x's own ppu.c. */

#if MIX_ON_CORE1
/* Cross-core sound-state lock — see the comment at S9xSetAPUDSP (apu.c).
 * spin_lock_unsafe: neither holder takes it from an IRQ, so no irq-save
 * variant is needed. Held ~µs on core0 (one DSP register write) and up
 * to ~200 µs on core1 (one 64-sample mix chunk). */
#include "pico/sync.h"
static spin_lock_t *snd_lock;

extern "C" void port_sound_lock_init(void)
{
    if (!snd_lock)
        snd_lock = spin_lock_instance(spin_lock_claim_unused(true));
}
extern "C" void __not_in_flash_func(port_sound_lock)(void)
{
    spin_lock_unsafe_blocking(snd_lock);
}
extern "C" void __not_in_flash_func(port_sound_unlock)(void)
{
    spin_unlock_unsafe(snd_lock);
}
#endif
