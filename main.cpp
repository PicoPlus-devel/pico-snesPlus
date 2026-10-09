/* pico_snesPlus — snes9x port for RP2350 + PSRAM, HSTX HDMI out.
 *
 * Frame loop is markedly different from pico-infonesPlus's NES:
 *   - InfoNES drives the host via per-scanline callbacks
 *     (InfoNES_PreDrawLine / InfoNES_PostDrawLine) and host-supplied audio
 *     waveform mixer (InfoNES_SoundOutput).
 *   - snes9x runs as a one-shot S9xMainLoop() that emulates a full SNES
 *     frame and returns at V-blank, writing pixels directly into GFX.Screen
 *     and queuing samples via S9xMixSamples (pulled by us after the frame).
 *
 * SNES native resolution 256x224 (NTSC) or 256x239 (PAL) is centered in
 * the 320x240 HSTX framebuffer with a 32-px L/R margin. Pixel format is
 * RGB555 (HSTX scan-out format — snes9x's PIXEL_FORMAT is set to RGB555
 * via PICO_SNESPLUS_HSTX in port.h).
 *
 * With RENDER_TO_FB (default) snes9x renders in S9X_STRIP_ROWS-row chunks
 * into small SRAM staging strips and copies each finished chunk into that
 * centered framebuffer window (strip renderer, see port_glue.cpp) — no
 * private PSRAM screen, no blit, and scan-out never sees mid-composite
 * pixels. Without it, the legacy path renders into a 256-wide PSRAM
 * buffer that is blitted here after S9xMainLoop (optionally on core1,
 * BLIT_ON_CORE1). */

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/divider.h"
#include "hardware/watchdog.h"
#include "tusb.h"
#include "ff.h"

#include "FrensHelpers.h"
#include "FrensFonts.h"
#include "romflash.h"
#include "progress_bar.h"
#include "settings.h"
#include "menu.h"
#include "menu_settings.h"
#include "vumeter.h"
#include "gamepad.h"
#include "nespad.h"
#include "wiipad.h"

#if HSTX
#include "hstx.h"
#endif

#include "hardware/structs/scb.h"

/* snes9x core */
extern "C" {
#include "snes9x.h"
#include "memmap.h"
#include "gfx.h"
#include "apu.h"
#include "soundux.h"
#include "ppu.h"
#include "cpuexec.h"
#include "display.h"
}

#if ENABLE_MSU1
/* MSU-1 expansion audio. Registers live in the snes9x core (ppu.c/dma.c);
 * the streaming half is split between core0 (msu1_pump — all SD I/O) and
 * core1 (msu1_mix — sums PCM into the DSP mix). See snes9x/src/msu1.h. */
#include "msu1.h"
#endif

#if ENABLE_SPC7110
/* SPC7110: Hudson's graphics decompressor + memory mapper, plus the
 * RTC-4513 on ROMType $F9. Registers and mapping live in the core; this
 * file supplies the RTC its clock and persists it. See snes9x/src/spc7110.h. */
#include "spc7110.h"
#endif

#if ENABLE_SDD1
/* S-DD1: decompressor + 1 MB bank mapper (Street Fighter Alpha 2, Star
 * Ocean). Wired entirely inside the core; main.cpp only frees its PSRAM
 * block when the session ends and shows its cost in the FPS overlay. See
 * snes9x/src/sdd1.h. */
#include "sdd1.h"
#endif

#if RENDER_TO_FB
/* port glue — strip renderer. Re-anchors the framebuffer window for the
 * current PPU.ScreenHeight (the overscan bit flips 224<->239 at runtime)
 * and exposes the window pointer for the FPS overlay. */
extern "C" void s9x_port_anchor_screen(void);
extern "C" uint16_t *s9x_port_fb_window;
/* Hook called by the strip copy-out just before it publishes a chunk whose
 * rows overlap the overlay band, so the FPS overlay can be stamped into the
 * frame's own pixels instead of into the live single-buffered FB after the
 * fact (which flickered). Args: strip base, stride, and the chunk's absolute
 * [block_start, block_end] row range (strip physical row 0 == block_start). */
extern "C" void (*s9x_port_strip_top_hook)(uint16_t *strip, int stride,
                                           int block_start, int block_end);
/* Tear guard (port_glue.cpp): scan-out beam position and per-frame tracking
 * of where the strips landed relative to the refreshes, used by paceFrame. */
extern "C" void     s9x_port_beam_install(void);
extern "C" uint32_t s9x_port_beam_pos(void);
extern "C" void     s9x_port_tear_frame_begin(void);
extern "C" bool     s9x_port_tear_frame_end(int32_t *psi_mid, bool *torn);
extern "C" void     s9x_port_tear_stats(uint32_t *frames, uint32_t *torn, uint32_t *waits,
                                        uint32_t *wait_us, uint32_t *span_max);
#else
/* port glue — snes9x's render target, 256-wide RGB555 in PSRAM. */
extern uint16_t *g_snes_private_screen;
#endif

#define EMULATOR_CLOCKFREQ_KHZ 378000      /* RP2350 default clock for this emulator */
#define EMULATOR_MAX_CLOCKFREQ_KHZ 504000  /* RP2350 max overclock — 504 MHz (experiment, may be unstable) */
#define AUDIOBUFFERSIZE 1024
/* 44100, matching everything downstream: the TLV320 DAC's register
 * script is written for 44.1 kHz (its BCLK-fed PLL runs below spec at
 * 32 kHz), the I2S setup in Frens::initAll uses the 44100 default, and
 * hstx_init's HDMI ACR default is 44100 too. An earlier 32000 setting
 * (to cut SPC700 sample-synthesis cost on core0) became pointless once
 * S9xMixSamples moved to core1 (MIX_ON_CORE1) — the synthesis cost now
 * lands on core1's idle time. */
#define SNES_AUDIO_HZ 44100

bool isFatalError = false;
char *romName = nullptr;
char selectedRom[FF_MAX_LFN] = {0};
#if AUDIO_WATCHDOG
/* Loudest sample core1 has mixed since core0 last looked. See
 * audio_watchdog_tick(). */
volatile uint32_t g_mix_peak = 0;
#endif
/* One-shot "resume this cart after the flash write" handshake across the
 * reboot romflash needs. See the loop entry in main(). */
static constexpr int      SNES_RESUME_SCRATCH = 5;
static constexpr uint32_t SNES_RESUME_MAGIC   = 0x5E5F1A54u;

/* Frames rendered in the last ~1 s window, updated by the per-second block in
 * run_emulator() and read by the on-screen FPS overlay. */
static uint32_t g_fps = 60;

/* -------------------------------------------------------------------------
 * Hardfault reporter — overrides the SDK's weak isr_hardfault (a bare
 * breakpoint) on BOTH cores. At 378 MHz this board lives near its silicon
 * margin, so when a fault fires we want the stacked frame and fault status
 * on serial without needing a debug probe attached. RAM-resident so it
 * still runs if XIP is unavailable (printf itself is flash-resident; if
 * the fault happened mid-flash-write the prints are lost but the
 * breakpoint below still lands). */
extern "C" void __not_in_flash_func(hardfault_report)(uint32_t *sp, uint32_t exc_lr)
{
    armv8m_scb_hw_t *scb = scb_hw;
    printf("\n*** HARDFAULT core%u ***\n", (unsigned)get_core_num());
    printf("  r0=%08lx r1=%08lx r2=%08lx  r3=%08lx\n",
           (unsigned long)sp[0], (unsigned long)sp[1],
           (unsigned long)sp[2], (unsigned long)sp[3]);
    printf(" r12=%08lx lr=%08lx pc=%08lx psr=%08lx\n",
           (unsigned long)sp[4], (unsigned long)sp[5],
           (unsigned long)sp[6], (unsigned long)sp[7]);
    printf("  sp=%08lx exc_lr=%08lx\n", (unsigned long)(uintptr_t)sp,
           (unsigned long)exc_lr);
    printf("  CFSR=%08lx HFSR=%08lx MMFAR=%08lx BFAR=%08lx\n",
           (unsigned long)scb->cfsr, (unsigned long)scb->hfsr,
           (unsigned long)scb->mmfar, (unsigned long)scb->bfar);
    while (true)
        __breakpoint();
}

extern "C" __attribute__((naked)) void __not_in_flash_func(isr_hardfault)(void)
{
    __asm volatile(
        "tst lr, #4          \n" /* which stack holds the exception frame? */
        "ite eq              \n"
        "mrseq r0, msp       \n"
        "mrsne r0, psp       \n"
        "mov r1, lr          \n"
        "b hardfault_report  \n");
}

/* GPIO/I2C pad state refreshed by host_tick(), consumed by S9xReadJoypad
 * (port_glue.cpp) and wantsMenu(). Encoding: see wiipad.h. */
uint16_t wiipad_raw_cached = 0;
/* Frame counter for the rapid-fire A/B menu setting (port_glue.cpp gates
 * the A/B bits on bit 1, giving a 15 Hz autofire). */
uint32_t g_rapid_fire_counter = 0;
/* Raised when the in-game menu closes so the confirm press does not reach the
 * game; cleared per port by S9xReadJoypad once the pad reads clear. */
extern "C" volatile bool g_pad_ignore_request;
/* ErrorMessage[] is owned by the framework (FrensHelpers.cpp); declared
 * extern in FrensHelpers.h. */

static uint32_t CPUFreqKHz = EMULATOR_CLOCKFREQ_KHZ;

/* SNES menu visibility — same shape as NES, drop FDS/DMG/border options
 * that don't apply. Non-const because settings might toggle entries later. */
int8_t g_settings_visibility_snes[MOPT_COUNT] = {
    [MOPT_EXIT_GAME]                = 0,                  /* set 1 at runtime when in-game */
    [MOPT_RESET_GAME]               = 0,                  /* set 1 at runtime when in-game */
    [MOPT_REBOOT_TO_LOADER]         = BOOTLOADER_BUILD,   /* Return to emuLoader picker (only when built for the loader) */
    [MOPT_SAVE_RESTORE_STATE]       = -1,                 /* Save / Restore State — deferred */
    [MOPT_SCREENMODE]               = 1,                  /* Screen Mode */
    [MOPT_SCANLINES]                = 0,                  /* Scanlines toggle (superseded by Screen Mode) */
    [MOPT_SCANLINE_TYPE]            = HSTX,               /* Scanline Type (HSTX only) */
    [MOPT_FPS_OVERLAY]              = 1,                  /* FPS Overlay */
    [MOPT_AUDIO_ENABLE]             = 1,                  /* Audio Enable */
    [MOPT_FRAMESKIP]                = 1,                  /* Frame Skip */
    [MOPT_DISPLAY_MODE]             = HSTX && ENABLEDVI,  /* Display Mode (HDMI or DVI, HSTX builds only) */
    [MOPT_EXTERNAL_AUDIO]           = EXT_AUDIO_IS_ENABLED, /* External Audio */
    [MOPT_FONT_COLOR]               = 1,                  /* Font Color */
    [MOPT_FONT_BACK_COLOR]          = 1,                  /* Font Back Color */
    [MOPT_FRUITJAM_VUMETER]         = ENABLE_VU_METER,    /* VU Meter */
    [MOPT_FRUITJAM_VOLUME_CONTROL]  = (HW_CONFIG == 8),   /* Fruit Jam Volume Control */
    [MOPT_DMG_PALETTE]              = 0,                  /* DMG Palette — NES/GB only */
    [MOPT_BORDER_MODE]              = 0,                  /* Border Mode — GB only */
    [MOPT_RAPID_FIRE_ON_A]          = 1,                  /* Rapid Fire on A */
    [MOPT_RAPID_FIRE_ON_B]          = 1,                  /* Rapid Fire on B */
    [MOPT_AUTO_INSERT_FDS_DISK_A]   = 0,                  /* NES/FDS only */
    [MOPT_AUTO_SWAP_FDS_DISK]       = 0,                  /* NES/FDS only */
    [MOPT_FDS_DISK_SWAP]            = 0,                  /* NES/FDS only */
    [MOPT_OVERCLOCK]                = (HSTX && (HW_CONFIG == 2 || HW_CONFIG == 8)), /* Overclock (CPU high clock toggle) */
    [MOPT_FM_AUDIO]                 = 0,                  /* YM Audio — SMS only */
    [MOPT_ENTER_BOOTSEL_MODE]       = 1,                  /* Enter bootsel mode */
    [MOPT_CONTROLLER_TEST]          = 1,                  /* Controller Test */
    [MOPT_RECENT_GAMES]             = 0,                  /* Recently played (menu.cpp force-shows this in the rom browser) */
    [MOPT_USB_DRIVE_MODE]           = 0,                  /* USB drive mode (menu.cpp force-shows this in the rom browser) */
    [MOPT_CASSETTE]                 = 0,                  /* TI-99/4A only */
    [MOPT_DISK]                     = 0,                  /* TI-99/4A only */
    [MOPT_SERIAL_KEYBOARD]          = 0,                  /* TI-99/4A only */
    [MOPT_SPRITE_LIMIT]             = 0,                  /* NES only */
    [MOPT_MENU_OVERSCAN]            = 0,                  /* Overscan in menu (menu.cpp force-shows this below the menu colors) */
    [MOPT_GENESIS_PAD]              = 0,                  /* Genesis only */
    [MOPT_NES_PALETTE]              = 0,                  /* NES only */
    [MOPT_HSTX_CLOCK_FIX]           = HSTX && !CFG_TUH_RPI_PIO_USB, /* Video Clock Fix (PIO-USB builds always have it, see SNES_OVERCLOCK_FIX) */
};

static const uint8_t g_available_screen_modes_snes[] = {
    1,  /* SCANLINE_8_7 */
    1,  /* NOSCANLINE_8_7 */
    1,  /* SCANLINE_1_1 */
    1,  /* NOSCANLINE_1_1 */
};

/* -------------------------------------------------------------------------
 * Audio pump — pull stereo samples from snes9x and push into the
 * framework's HDMI Data Island queue (or external I2S if enabled).
 *
 * audio_route_to_ext() is the single source of truth for where samples go
 * (setting on, OR headphone jack plugged in). Pacing and routing MUST use
 * the same answer: pacing against one queue while enqueueing into the
 * other lets the mixer run unthrottled — the unfed queue never fills, so
 * the real sink overflows and drops samples, garbling the audio. */
static inline bool audio_route_to_ext(void)
{
#if EXT_AUDIO_IS_ENABLED
    return settings.flags.useExtAudio || Frens::isHeadPhoneJackConnected();
#else
    return false;
#endif
}

static int audio_free_samples(bool toExtAudio)
{
#if EXT_AUDIO_IS_ENABLED
    if (toExtAudio) {
        return audio_i2s_get_freebuffer_size();
    }
#else
    (void)toExtAudio;
#endif
#if HSTX
    int level = hstx_di_queue_get_level();
    int free_packets = HSTX_AUDIO_DI_HIGH_WATERMARK - level;
    if (free_packets <= 0) return 0;
    return free_packets << 2;  /* 4 samples per DI packet */
#else
    return 0;
#endif
}

#if !(HSTX && MIX_ON_CORE1)
static int16_t mix_buf[512];  /* up to 256 stereo frames per pump call */
#endif

#if HSTX && BLIT_ON_CORE1
/* -------------------------------------------------------------------------
 * Off-load the GFX.Screen (PSRAM) → HSTX framebuffer (SRAM) blit to core1.
 * core1's video work all happens in dma_irq_handler; its thread context
 * (video_output_core1_run) is an idle watchdog loop with a background-task
 * hook, so the ~2.5 ms blit runs there for free while core0 emulates the
 * next (frameskipped) SNES frame.
 *
 * Safety: GFX.Screen is only written by S9xMainLoop on rendered frames.
 * Core0 submits the job right after a rendered frame and never touches
 * GFX.Screen until the next rendered frame, so the only sync needed is
 * "wait for pending==0 before starting a rendered S9xMainLoop" (and before
 * the menu repaints the framebuffer). With frameskip on, the blit has two
 * whole skip-frames (~33 ms) to finish; the wait never actually spins. */
static struct {
    const uint16_t *src;
    uint16_t       *dst;
    int             rows;
    volatile bool   pending;
} blit_job;

static void __not_in_flash_func(core1_blit_task)(void)
{
    if (!blit_job.pending) return;
    __dmb();  /* order: job fields were written before pending was set */
    const uint16_t * __restrict src = blit_job.src;
    uint16_t       * __restrict dst = blit_job.dst;
    for (int y = 0; y < blit_job.rows; y++) {
        memcpy(dst, src, SNES_WIDTH * sizeof(uint16_t));
        src += SNES_WIDTH;
        dst += 320;
    }
    __dmb();
    blit_job.pending = false;
}

static inline void blit_wait_done(void)
{
    while (blit_job.pending) tight_loop_contents();
    __dmb();
}

static inline void blit_submit(const uint16_t *src, uint16_t *dst, int rows)
{
    blit_job.src  = src;
    blit_job.dst  = dst;
    blit_job.rows = rows;
    __dmb();
    blit_job.pending = true;
}
#endif

#if HSTX && MIX_ON_CORE1
/* -------------------------------------------------------------------------
 * Audio mixing + HDMI data-island encode on core1. Two wins:
 *   - core0 gets ~850 us/frame back (the entire pump_audio cost);
 *   - the DI queue is topped up continuously instead of once per frame
 *     capped at 256 samples — which could never keep up with the 533
 *     samples/frame the scan-out consumes at 32 kHz, so audio was
 *     chronically interleaved with silence-fallback packets.
 * Exclusion vs the SPC700's DSP writes (core0): port_sound_lock, taken
 * per 64-sample chunk here and around S9xSetAPUDSP there. The park
 * protocol makes the mixer quiescent for the menu (whose wavplayer is
 * the other DI-queue producer) and during sound init/teardown. */
extern "C" {
    void port_sound_lock_init(void);
    void port_sound_lock(void);
    void port_sound_unlock(void);
}
#if PROFILE_BUCKETS
extern "C" bool g_prof_bypass_apu;   /* defined in the PROFILE block below */
#endif

static volatile bool mix_c1_enable = false;
static volatile bool mix_c1_parked = true;
static int16_t mix_buf_c1[128];   /* one 64-stereo-frame chunk */

static void __not_in_flash_func(core1_mix_task)(void)
{
    if (!mix_c1_enable) {
        mix_c1_parked = true;
        return;
    }
    mix_c1_parked = false;
#if ENABLE_VU_METER
    /* All NeoPixel writes stay on this core: turnOffAllLeds() from core0
     * could interleave with a 5-pixel update here and leave garbage lit.
     * Clear on the on->off transition instead (menu entry/apply clears
     * separately, while the mixer is parked). */
    static bool vu_was_on;
    bool vu_on = settings.flags.enableVUMeter != 0;
    if (vu_was_on && !vu_on) turnOffAllLeds();
    vu_was_on = vu_on;
#endif
    if (!settings.flags.audioEnabled) return;
#if PROFILE_BUCKETS
    if (g_prof_bypass_apu) return;
#endif
    bool toExtAudio = audio_route_to_ext();
    int free_slots = audio_free_samples(toExtAudio);
    while (free_slots > 0 && mix_c1_enable) {
        int n = free_slots > 64 ? 64 : free_slots;
        port_sound_lock();
        S9xMixSamples(mix_buf_c1, n * 2);
        port_sound_unlock();
#if AUDIO_WATCHDOG
        {
            int pk = 0;
            for (int i = 0; i < n * 2; i++) {
                int v = mix_buf_c1[i] < 0 ? -mix_buf_c1[i] : mix_buf_c1[i];
                if (v > pk) pk = v;
            }
            if ((uint32_t)pk > g_mix_peak) g_mix_peak = (uint32_t)pk;
        }
#endif
#if ENABLE_MSU1
        /* MSU-1 PCM sums on top of the SNES DSP mix — before the VU meter so
         * the meter shows what actually leaves the box, and before both sink
         * loops so one call covers HDMI and I2S. Deliberately outside the
         * sound lock: it shares nothing with the SPC700/DSP, only the PSRAM
         * ring that core0 fills in msu1_pump. S9xMixSamples has already
         * clipped its output to full scale, so the sum really can overflow —
         * msu1_mix saturates. */
        msu1_mix(mix_buf_c1, n);
#endif
#if ENABLE_VU_METER
        if (vu_on) {
            for (int i = 0; i < n; i++) {
                addSampleToVUMeter(mix_buf_c1[i*2]);
            }
        }
#endif
#if EXT_AUDIO_IS_ENABLED
        if (toExtAudio) {
            for (int i = 0; i < n; i++) {
                EXT_AUDIO_ENQUEUE_SAMPLE(mix_buf_c1[i*2], mix_buf_c1[i*2+1]);
            }
        } else
#endif
        {
            for (int i = 0; i < n; i++) {
                hstx_push_audio_sample(mix_buf_c1[i*2], mix_buf_c1[i*2+1]);
            }
        }
        free_slots -= n;
    }
}

static void mix_c1_park(void)
{
    mix_c1_enable = false;
    while (!mix_c1_parked) tight_loop_contents();
    __dmb();
}

static inline void mix_c1_resume(void)
{
    __dmb();
    mix_c1_enable = true;
}
#endif

#if HSTX && (BLIT_ON_CORE1 || MIX_ON_CORE1)
/* Single background task for core1's idle loop. Blit first — it has a
 * (soft) deadline against the next rendered frame; audio has ~25 ms of
 * queue headroom. */
static void __not_in_flash_func(core1_background_task)(void)
{
#if BLIT_ON_CORE1
    core1_blit_task();
#endif
#if MIX_ON_CORE1
    core1_mix_task();
#endif
}
#endif

#if PROFILE_BUCKETS
/* Frame-time accumulators, us. Reset once per second alongside the fps
 * printf. All updates happen on core0 in run_emulator() so no locking.
 * main is split by rendered vs skipped so we can see render cost
 * separately from CPU+APU cost. */
static uint32_t prof_us_host_tick;
static uint32_t prof_us_main_r;      /* S9xMainLoop on rendered frames */
static uint32_t prof_us_main_s;      /* S9xMainLoop on skipped frames  */
static uint32_t prof_frames_r;
static uint32_t prof_frames_s;
static uint32_t prof_us_blit;
static uint32_t prof_us_pump;
static uint32_t prof_us_pace;
/* SuperFX GSU cost, accumulated inside S9xSuperFXExec (fxemu.c). The GSU
 * runs inside S9xMainLoop, so this is a *subset* of mainR — sfx/mainR is
 * the share of emulation spent in the GSU. */
extern "C" uint32_t g_prof_us_sfx;
extern "C" uint32_t g_prof_sfx_runs;

/* Phase 0 A/B toggles. Cycles every ~5 s through a 4-state pattern so a
 * single UART capture gives all combinations. When bypass_apu is on we
 * toggle BOTH IAPU.APUExecuting and Settings.APUEnabled — the latter is
 * necessary because dma.c:381 and apu.c:71/104/136 re-derive APUExecuting
 * from Settings.APUEnabled on every DMA and every port r/w, so leaving
 * Settings.APUEnabled=true means the bypass is defeated within µs.
 * When bypass_pace is on we skip paceFrame() so raw emulator fps shows. */
extern "C" bool g_prof_bypass_apu;
extern "C" bool g_prof_bypass_pace;
bool g_prof_bypass_apu = false;
bool g_prof_bypass_pace = false;

static bool s_prof_apu_bypass_active = false;
static bool s_prof_saved_apuenabled  = true;
static void dbg_apply_apu_bypass(bool on)
{
    if (on && !s_prof_apu_bypass_active) {
        s_prof_saved_apuenabled = Settings.APUEnabled;
        s_prof_apu_bypass_active = true;
    } else if (!on && s_prof_apu_bypass_active) {
        Settings.APUEnabled = s_prof_saved_apuenabled;
        IAPU.APUExecuting   = s_prof_saved_apuenabled;
        s_prof_apu_bypass_active = false;
        return;
    }
    if (on) {
        Settings.APUEnabled = false;
        IAPU.APUExecuting   = false;
    }
}
#endif

/* Core0 audio pump — only compiled when mixing has NOT moved to core1
 * (core1_mix_task replaces it under MIX_ON_CORE1; keeping this compiled
 * would waste mix_buf's 1 KB of .bss, which counts against the SRAM
 * heap budget that decides whether FillRAM fits in SRAM). */
#if !(HSTX && MIX_ON_CORE1)
static void __not_in_flash_func(pump_audio)(void)
{
#if PROFILE_BUCKETS
    if (g_prof_bypass_apu) return;
#endif
#if ENABLE_VU_METER
    /* Same on->off LED clear as core1_mix_task, for the core0-pump build. */
    static bool vu_was_on;
    bool vu_on = settings.flags.enableVUMeter != 0;
    if (vu_was_on && !vu_on) turnOffAllLeds();
    vu_was_on = vu_on;
#endif
    if (!settings.flags.audioEnabled) return;

    bool toExtAudio = audio_route_to_ext();
    int free_slots = audio_free_samples(toExtAudio);
    if (free_slots <= 0) return;
    if (free_slots > 256) free_slots = 256;  /* cap to mix_buf */

    /* S9xMixSamples expects "count" = stereo*2 (number of int16 slots). */
    S9xMixSamples(mix_buf, free_slots * 2);
#if ENABLE_MSU1
    msu1_mix(mix_buf, free_slots);   /* see the core1 mixer for the rationale */
#endif
#if ENABLE_VU_METER
    if (vu_on) {
        for (int i = 0; i < free_slots; i++) {
            addSampleToVUMeter(mix_buf[i*2]);
        }
    }
#endif

#if EXT_AUDIO_IS_ENABLED
    if (toExtAudio) {
        for (int i = 0; i < free_slots; i++) {
            EXT_AUDIO_ENQUEUE_SAMPLE(mix_buf[i*2], mix_buf[i*2+1]);
        }
        return;
    }
#endif
#if HSTX
    for (int i = 0; i < free_slots; i++) {
        hstx_push_audio_sample(mix_buf[i*2], mix_buf[i*2+1]);
    }
#endif
}
#endif /* !(HSTX && MIX_ON_CORE1) */

/* -------------------------------------------------------------------------
 * Frame pacing — 60 Hz NTSC via PaceFrames60fps, 50 Hz PAL via sleep_until.
 * Same model as pico-infonesPlus's paceFrame().
 *
 * PACE_SOFT_60FPS replaces the vsync-locked NTSC pacer with a time-based
 * budget (target += 16716 us per frame; sleep only if ahead). The
 * pico_shared vsync pacer aligns every SNES frame to a display vsync
 * tick — great for tearing, but when frameskip is on and work per
 * SNES-frame is uneven (render 42 ms, skip 4 ms, skip 4 ms) it aligns
 * each frame to its own tick, forcing 3 SNES frames into 4 vsync
 * intervals = 45 fps even when the emulator could produce 60. Soft
 * pacing lets the emulator free-run at its true rate; may introduce
 * tearing on the blit if it lands mid-scan.
 *
 * PACE_VSYNC_PHASE (strip renderer only) supersedes both for NTSC. It paces
 * whole frameskip groups (one rendered frame plus the skipped ones after
 * it) instead of single frames, so it has the soft pacer's throughput, but
 * starts every group on the display's refresh grid: a group of N frames
 * gets exactly N refreshes. Free-running against the display is what let
 * the strip copy-out and the beam leapfrog each other into stair-stepped
 * tears; on the grid, pace_note_frame() can then move the start phase until
 * a rendered frame's strips all reach the screen in the same refresh. A
 * group that overruns starts late, without waiting, as with the soft pacer.
 * groupEnd: the next frame starts a new group (it will be rendered). */
static absolute_time_t pal_next_frame;
#if PACE_SOFT_60FPS && !PACE_VSYNC_PHASE
static absolute_time_t soft_next_frame;
#endif
#if PACE_VSYNC_PHASE
static constexpr int32_t PACE_LINES = MODE_V_TOTAL_LINES;  /* output lines per refresh */
static uint32_t pace_slot;      /* beam position (s9x_port_beam_pos) the next group starts at */
static uint32_t pace_frames;    /* frames run since the current group started */
static int32_t  pace_nudge;     /* phase correction, lines, applied at the next group start */
static bool     pace_on_time;   /* the current group started on its slot, not late */
static bool     pace_have_frame;/* the current group's rendered frame reported below */
static int32_t  pace_last_m;    /* ... its psi midpoint, mod PACE_LINES */
static bool     pace_last_torn; /* ... and whether it reached the screen torn */
static uint32_t pace_bad_run;   /* consecutive groups that started late AND tore */
static uint32_t pace_since_jump;/* groups since the last phase jump (saturating) */

/* A rendered frame's tear report (s9x_port_tear_frame_end). Steers the
 * group start phase so the frame's psi midpoint sits in the middle of a
 * refresh interval, as far as possible from both refreshes that could catch
 * a strip. A quarter of the error per frame, capped at ~1 ms, so a game's
 * changing render load moves the phase smoothly. Only from groups that
 * started on their slot: a late group's start was not the phase the pacer
 * asked for, and steering on it winds the slot away from reality. */
static void pace_note_frame(int32_t psi_mid, bool torn)
{
    int32_t m = psi_mid % PACE_LINES;
    if (m < 0) m += PACE_LINES;
    pace_last_m     = m;
    pace_last_torn  = torn;
    pace_have_frame = true;
    if (!pace_on_time)
        return;
    int32_t step = (PACE_LINES / 2 - m) / 4;
    if (step > 32) step = 32;
    if (step < -32) step = -32;
    pace_nudge += step;
}
#endif
static void paceFrame(bool init, bool groupEnd = true)
{
    if (Settings.PAL) {
        if (init) {
            pal_next_frame = make_timeout_time_us(20000);
            return;
        }
        sleep_until(pal_next_frame);
        pal_next_frame = delayed_by_us(pal_next_frame, 20000);
        /* Resync if we drift more than two frames behind. */
        if (absolute_time_diff_us(get_absolute_time(), pal_next_frame) < -40000) {
            pal_next_frame = make_timeout_time_us(20000);
        }
    } else {
#if PACE_VSYNC_PHASE
        if (init) {
            pace_slot       = s9x_port_beam_pos();
            pace_frames     = 0;
            pace_nudge      = 0;
            pace_on_time    = false;
            pace_have_frame = false;
            pace_bad_run    = 0;
            pace_since_jump = 0;
            return;
        }
        pace_frames++;
        if (!groupEnd)
            return;   /* skipped frames run back to back inside their group */
        pace_slot  += pace_frames * PACE_LINES + pace_nudge;
        pace_frames = 0;
        pace_nudge  = 0;
        if (pace_since_jump < 0xFFFF)
            pace_since_jump++;
        int32_t late = (int32_t)(s9x_port_beam_pos() - pace_slot);
        bool on_time = late < 0;
        pace_bad_run = (!on_time && pace_have_frame && pace_last_torn) ? pace_bad_run + 1 : 0;
        if (on_time) {
            while ((int32_t)(s9x_port_beam_pos() - pace_slot) < 0)
                tight_loop_contents();
        } else if (late >= PACE_LINES) {
            /* A refresh or more behind (a heavy scene): give up the missed
             * refreshes rather than racing to catch up, but keep the phase.
             * A smaller lateness keeps the slot, so the next groups' slack
             * absorbs it. */
            pace_slot += (uint32_t)(late / PACE_LINES) * PACE_LINES;
        }
        if (pace_bad_run >= 4 && pace_since_jump >= 60) {
            /* Stuck: with almost no slack per group a late start can hold
             * itself in place (the torn frame makes strips wait for the
             * beam, the waits eat the slack), and steering cannot move a
             * start that is already late any earlier. Jump forward to the
             * phase the last frame asked for instead: one delay of under a
             * refresh. At most every ~60 groups (2 s with frameskip), which
             * bounds the cost when the scene simply cannot hold 60 fps. */
            pace_slot = s9x_port_beam_pos() +
                        (uint32_t)((PACE_LINES / 2 - pace_last_m + PACE_LINES) % PACE_LINES);
            while ((int32_t)(s9x_port_beam_pos() - pace_slot) < 0)
                tight_loop_contents();
            on_time         = true;
            pace_bad_run    = 0;
            pace_since_jump = 0;
        }
        pace_on_time    = on_time;
        pace_have_frame = false;
#elif PACE_SOFT_60FPS
        if (init) {
            soft_next_frame = make_timeout_time_us(16716);
            return;
        }
        if (absolute_time_diff_us(get_absolute_time(), soft_next_frame) > 0) {
            sleep_until(soft_next_frame);
        }
        soft_next_frame = delayed_by_us(soft_next_frame, 16716);
        /* Resync when persistently far behind schedule (a heavy scene the
         * emulator can't sustain 60fps in). Two things matter here:
         *   - Threshold big enough that resyncs are rare (200 ms).
         *   - Set target = now - 16716, NOT now + 16716: the latter puts
         *     target ahead of now and triggers a burst of 12+ ms sleeps
         *     on the next skip frames, offsetting the real emulator rate.
         *     Starting one frame in the PAST means the immediate next
         *     frame ends up naturally behind schedule — no catch-up sleeps. */
        if (absolute_time_diff_us(get_absolute_time(), soft_next_frame) < -200000) {
            soft_next_frame = delayed_by_us(get_absolute_time(), -16716);
        }
#else
        Frens::PaceFrames60fps(init);
#endif
    }
}

/* -------------------------------------------------------------------------
 * Host tick — must run once per frame. Pumps the TinyUSB host stack
 * (otherwise USB HID gamepad state never updates) and refreshes the
 * GPIO/Wii pad latches that the framework's getCurrentGamePadState()
 * reads. Mirrors the pump done in pico-infonesPlus's InfoNES_LoadFrame. */
static void host_tick(void)
{
#if NES_PIN_CLK != -1
    nespad_read_start();
#endif
    auto count =
#if !HSTX
        dvi_->getFrameCounter();
#else
        hstx_getframecounter();
#endif
    auto onOff = hw_divider_s32_quotient_inlined(count, 60) & 1;
    Frens::blinkLed(onOff);
    Frens::pollHeadPhoneJack();
#if NES_PIN_CLK != -1
    nespad_read_finish();
#endif
    tuh_task();
    /* SNES Mouse auto hot-plug: with a USB mouse present the core's mouse
     * path takes over SNES port 1 — the port Mario Paint requires — like
     * plugging the real peripheral in, so pad 1 is suspended until the
     * mouse is unplugged (pad 2 and the SELECT+START menu combo keep
     * working). MouseMaster tracks the live connection so without a mouse
     * the core executes the exact same instruction path as before this
     * feature. */
    {
        io::MouseState &m = io::getCurrentMouseState();
        Settings.MouseMaster = m.connected;
        if (m.connected && IPPU.Controller == SNES_JOYPAD)
        {
            m.dx = m.dy = m.wheel = 0; /* drop deltas piled up while inactive */
            IPPU.Controller = SNES_MOUSE;
        }
        else if (!m.connected && IPPU.Controller == SNES_MOUSE)
            IPPU.Controller = SNES_JOYPAD;
    }
#if WII_PIN_SDA >= 0 && WII_PIN_SCL >= 0
    wiipad_raw_cached = wiipad_read();
#endif
#if ENABLE_VU_METER
    /* Physical toggle button (see vumeter.h). Only flips the setting —
     * the audio pump owning the NeoPixel PIO clears the LEDs on the
     * on->off transition. */
    if (isVUMeterToggleButtonPressed()) {
        settings.flags.enableVUMeter = !settings.flags.enableVUMeter;
        FrensSettings::savesettings();
    }
#endif
    g_rapid_fire_counter++;
}

/* Input — check for menu trigger (SELECT+START combo) on any connected
 * pad: both USB players plus the GPIO NES/SNES and Wii Classic pads
 * (without which NESPAD/Wii-only boards could never open the menu).
 * snes9x's joypad read goes through S9xReadJoypad (in port_glue.cpp),
 * called by S9xUpdateJoypads from inside S9xMainLoop. */
static bool wantsMenu(void)
{
    constexpr uint32_t combo = io::GamePadState::Button::SELECT |
                               io::GamePadState::Button::START;
    for (int i = 0; i < 2; i++)
    {
        if ((io::getCurrentGamePadState(i).buttons & combo) == combo)
            return true;
    }
    /* nespad raw and wiipad share the low-bit layout: Select=bit2, Start=bit3. */
    uint32_t aux = 0;
#if NES_PIN_CLK != -1
    aux |= nespad_states[0];
#endif
#if NES_PIN_CLK_1 != -1
    aux |= nespad_states[1];
#endif
#if WII_PIN_SDA >= 0 && WII_PIN_SCL >= 0
    aux |= wiipad_raw_cached;
#endif
    return (aux & 0x0C) == 0x0C;
}

/* -------------------------------------------------------------------------
 * snes9x bring-up: configure Settings, allocate buffers, hand the
 * PSRAM-resident ROM to LoadROM(NULL). */
static bool snes9x_setup_settings(void)
{
    memset(&Settings, 0, sizeof(Settings));
    Settings.CyclesPercentage  = 100;
    Settings.H_Max             = SNES_CYCLES_PER_SCANLINE;
    Settings.HBlankStart       = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.FrameTimePAL      = 20000;
    Settings.FrameTimeNTSC     = 16667;
    Settings.ControllerOption  = SNES_JOYPAD;
    Settings.SoundPlaybackRate = SNES_AUDIO_HZ;
    Settings.SoundInputRate    = SNES_AUDIO_HZ;
    Settings.SoundBufferSize   = 1024;
    Settings.SoundMixInterval  = 0;
    /* Linear interpolation in the mixer (~25-30% of MixStereo's cost).
     * Flip to true when CPU headroom recovers — mostly affects chip-music
     * tracks with high-freq detail (F-Zero, Castlevania IV). */
    Settings.InterpolatedSound = false;
    Settings.DisableSoundEcho  = false;
    Settings.Mute              = false;
    Settings.APUEnabled        = true;
    Settings.Shutdown          = true;
    return true;
}

/* rom_ptr is either the framework's PSRAM copy or, for carts too big to
 * preload, the image in XIP flash (romflash.h). read_only says which: an XIP
 * pointer cannot take the in-place header fixups ApplyROMPatches() makes for a
 * handful of named carts, and a write there would be silently dropped. */
/* PSRAM the emulator still needs once the ROM is in place. Derived from the
 * allocation sites rather than guessed, because getting it wrong is only
 * discovered after the ROM has been committed -- a 7 MB cart leaves 1023 KB
 * free, needs ~1.04 MB, and dies in S9xInitDisplay with "Display init failed".
 *
 * The first group is unconditional PSRAM (memmap.c S9xInitMemory, port_glue).
 * The second is the SRAM-first allocations, which by the time the menu has run
 * routinely spill to PSRAM anyway -- the render strips already do. Budgeting
 * for the spill costs nothing but a slightly earlier switch to flash. */
static size_t snes_psram_working_set(void)
{
    size_t n = 0;

    n += RAM_SIZE;                        /* Memory.RAM            128 KB */
    n += VRAM_SIZE;                       /* Memory.VRAM            64 KB */
    n += SRAM_SIZE;                       /* Memory.SRAM           128 KB */
    n += 256u * 9u * sizeof(uint16_t);    /* IPPU.ScreenColors     4.5 KB */
    n += (size_t)MAX_2BIT_TILES * 128u;   /* IPPU.TileCache        512 KB */
    n += (size_t)MAX_2BIT_TILES;          /* IPPU.TileCached         4 KB */
    n += 0x2000u;                         /* bytes0x2000             8 KB */
    n += (size_t)SNES_HEIGHT_EXTENDED * 128u; /* s9x_port_objonline 30 KB */
    n += 120u * 1024u;                    /* soundux LocalState (file-static) */
#if RENDER_TO_FB || FILLRAM_IN_PSRAM
    n += FILLRAM_SIZE;                    /* FillRAM forced to PSRAM 32 KB */
#endif
#if ENABLE_MSU1
    n += 68u * 1024u;                     /* MSU-1 ring + data window, if a pack exists */
#endif

    n += 64u * 1024u;                     /* IAPU.RAM          } SRAM-first, */
    n += 26u * 1024u;                     /* render strips     } but spill   */
    n += 20u * 1024u;                     /* Memory.Map+MapInfo} when the    */
    n += 23u * 1024u;                     /* gfx LocalState    } arena fills */
    n += 32u * 1024u;                     /* lwmem block overhead + slack */
    return n;
}

#if AUDIO_WATCHDOG
/* Why-is-it-silent watchdog. core1 records the loudest sample it mixed; core0
 * checks once a second. Three silent seconds in a row while audio is enabled
 * means the DSP is being asked for sound and returning none -- which looks
 * identical to a healthy system from the outside: 60 fps, no underruns, no
 * resyncs, because the mixer is still feeding the queue, just with zeros.
 * Dumps the state that distinguishes the causes, once per silent spell. */

static void audio_watchdog_tick(void)
{
    static int  silent_secs = 0;
    static bool reported    = false;

    /* Deliberately NOT returning early when audio is disabled: "the setting
     * got turned off" is itself a candidate explanation, and returning here
     * would hide exactly that. It is reported below instead. */
    if (g_mix_peak != 0) { silent_secs = 0; reported = false; g_mix_peak = 0; return; }
    if (++silent_secs < 3 || reported) return;
    reported = true;

    printf("AUDIO SILENT %ds. audioEnabled=%d route=%s | so.mute=%d so.rate=%lu | "
           "DSP FLG=%02x KON=%02x KOFF=%02x keyed=%02x "
           "ENDX=%02x MVOL=%d/%d | SPC PC=%04x ports=%02x %02x %02x %02x\n",
           silent_secs, (int)settings.flags.audioEnabled,
           audio_route_to_ext() ? "ext" : "hdmi",
           (int)so.mute_sound, (unsigned long)so.playback_rate,
           APU.DSP[APU_FLG], APU.DSP[APU_KON], APU.DSP[APU_KOFF],
           APU.KeyedChannels, APU.DSP[APU_ENDX],
           (int8_t)APU.DSP[APU_MVOL_LEFT], (int8_t)APU.DSP[APU_MVOL_RIGHT],
           (unsigned)(IAPU.PC - IAPU.RAM),
           APU.OutPorts[0], APU.OutPorts[1], APU.OutPorts[2], APU.OutPorts[3]);
    printf("  channels state/vol:");
    for (int i = 0; i < 8; i++)
        printf(" %d:%d/%d,%d", i, SoundData.channels[i].state,
               SoundData.channels[i].volume_left,
               SoundData.channels[i].volume_right);
    printf("\n");

    /* ENDX=ff with the driver still writing volumes means every voice hit the
     * END flag of its BRR block immediately, which is what corrupt sample data
     * looks like. Print the sample directory and the first BRR header each
     * voice points at: header bit 0 is END, so 0x01/0x03 on every voice is
     * garbage, while sane headers put the blame on DSP state instead. */
    {
        uint32_t dir = (uint32_t)APU.DSP[0x5d] << 8;
            extern uint32_t g_apu_port_writes;
        static uint32_t prev_port_writes = 0;
        printf("  CPU->APU port writes since last report: %lu\n",
               (unsigned long)(g_apu_port_writes - prev_port_writes));
        prev_port_writes = g_apu_port_writes;
        {
            extern uint8_t  g_apu_port_log[16][2];
            extern uint32_t g_apu_port_log_pos;
            printf("  last CPU->APU writes (port=val):");
            for (int i = 0; i < 16; i++) {
                uint32_t k = (g_apu_port_log_pos + i) & 15;
                printf(" %d=%02x", g_apu_port_log[k][0], g_apu_port_log[k][1]);
            }
            printf("\n  SPC sees $f4-$f7: %02x %02x %02x %02x\n",
                   IAPU.RAM[0xf4], IAPU.RAM[0xf5], IAPU.RAM[0xf6], IAPU.RAM[0xf7]);
            {
                extern uint32_t g_apu_kon_writes, g_apu_kon_bits,
                                g_apu_koff_writes, g_apu_dsp_writes;
                static uint32_t pk, pf, pd;
                printf("  DSP writes: %lu  KON(nonzero): %lu bits=%02x  KOFF: %lu\n",
                       (unsigned long)(g_apu_dsp_writes - pd),
                       (unsigned long)(g_apu_kon_writes - pk),
                       (unsigned)g_apu_kon_bits,
                       (unsigned long)(g_apu_koff_writes - pf));
                pk = g_apu_kon_writes; pf = g_apu_koff_writes;
                pd = g_apu_dsp_writes; g_apu_kon_bits = 0;
            }
            /* Timers clock the driver's sequencer; they are ticked off the
             * scanline loop, so a frozen counter here means the SPC700 is not
             * being executed rather than that the game went quiet. */
            printf("  timers en=%d%d%d tgt=%03x/%03x/%03x cnt=%x/%x/%x\n",
                   (int)APU.TimerEnabled[0], (int)APU.TimerEnabled[1],
                   (int)APU.TimerEnabled[2],
                   (unsigned)APU.TimerTarget[0], (unsigned)APU.TimerTarget[1],
                   (unsigned)APU.TimerTarget[2],
                   IAPU.RAM[0xfd], IAPU.RAM[0xfe], IAPU.RAM[0xff]);
            /* FLG bit 5 going 1->0 lets the DSP write echo into APU RAM. If
             * that region lands on the driver, the driver dies at a fixed time
             * after the game enables echo -- which is the observed symptom.
             * Checksums of the code area say whether APU RAM is being eaten. */
            {
                uint32_t esa = (uint32_t)APU.DSP[APU_ESA] << 8;
                uint32_t edl = (uint32_t)(APU.DSP[APU_EDL] & 0x0f) * 2048;
                uint32_t a, ck1 = 0, ck2 = 0;
                for (a = 0x0200; a < 0x2000; a++) ck1 = (ck1 << 1 | ck1 >> 31) + IAPU.RAM[a];
                for (a = 0xff00; a < 0xffc0; a++) ck2 = (ck2 << 1 | ck2 >> 31) + IAPU.RAM[a];
                printf("  echo ESA=%04lx EDL=%x range=%04lx-%04lx EON=%02x | "
                       "apuram ck %08lx/%08lx\n",
                       (unsigned long)esa, APU.DSP[APU_EDL] & 0x0f,
                       (unsigned long)esa, (unsigned long)(esa + (edl ? edl : 4)),
                       APU.DSP[APU_EON],
                       (unsigned long)ck1, (unsigned long)ck2);
            }
        }
        /* Walk each voice's BRR chain to the END bit. A real instrument is
         * tens to hundreds of 9-byte blocks; 1 or 2 means the sample data in
         * APU RAM is truncated or garbage, which is exactly what "every
         * key-on ends immediately" looks like from the mixer's side. */
        printf("  DIR=%04x\n", (unsigned)dir);
        for (int v = 0; v < 8; v++) {
            uint32_t e    = (dir + ((uint32_t)APU.DSP[(v << 4) | 0x04] << 2)) & 0xffff;
            uint32_t brr  = (IAPU.RAM[e] | (IAPU.RAM[(e + 1) & 0xffff] << 8)) & 0xffff;
            uint32_t loop = (IAPU.RAM[(e + 2) & 0xffff] |
                             (IAPU.RAM[(e + 3) & 0xffff] << 8)) & 0xffff;
            uint32_t a = brr, n = 0;
            uint8_t  h = IAPU.RAM[brr];
            while (n < 1024) { n++; if (IAPU.RAM[a] & 1) break; a = (a + 9) & 0xffff; }
            printf("    v%d src=%02x start=%04x loop=%04x hdr=%02x blocks=%lu end=%04x"
                   " adsr=%02x%02x gain=%02x envx=%d pitch=%02x%02x\n",
                   v, APU.DSP[(v << 4) | 0x04], (unsigned)brr, (unsigned)loop, h,
                   (unsigned long)n, (unsigned)a,
                   APU.DSP[(v << 4) | 0x05], APU.DSP[(v << 4) | 0x06],
                   APU.DSP[(v << 4) | 0x07], SoundData.channels[v].envx,
                   APU.DSP[(v << 4) | 0x03], APU.DSP[(v << 4) | 0x02]);
        }
    }
}
#endif

/* Decimal conversion for the flash status line. snprintf lives in flash and
 * pulls in a lot of machinery; this is three lines and stays SRAM-resident
 * with the rest of that path. Returns the number of characters written. */
static int snes_u32_to_dec(char *out, uint32_t v)
{
    char tmp[10];
    int  n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

#if HSTX
/* Progress bar during a ROM-to-flash write. Must be SRAM-resident: core1 is
 * still servicing scan-out while XIP is off, and progress_bar_draw() is
 * __not_in_flash_func for the same reason. Colours are RGB555 literals so
 * nothing is read from a palette in flash. */
#define PB_COL_BORDER 0x0000u   /* black  */
#define PB_COL_EMPTY  0x7FFFu   /* white  */
#define PB_COL_FILL   0x03E0u   /* green  */

static void snes_romflash_progress(int phase, uint32_t done, uint32_t total)
{
    /* Erase is the long pole (~30 s of a ~45 s write), so give it most of the
     * bar: 0..60 for erase, 60..100 for the write. */
    uint32_t pct = total == 0 ? 0
                 : (phase == SNES_ROMFLASH_ERASE)
                     ? (uint32_t)((uint64_t)done * 60u / total)
                     : 60u + (uint32_t)((uint64_t)done * 40u / total);

    /* The write phase fires ~1792 times; only redraw when the bar moves. */
    static uint32_t last = 0xFFFFFFFFu;
    if (pct == last && done != total) return;
    last = pct;

    /* Phase plus KB, so the screen says something useful while the bar sits
     * on the same percentage for a few seconds during a block erase. Built
     * with no printf: this runs between bootrom flash calls and stays
     * SRAM-only on principle. */
    char st[32];
    const char *what = (phase == SNES_ROMFLASH_ERASE) ? "Erasing " : "Writing ";
    int n = 0;
    while (what[n] && n < 12) { st[n] = what[n]; n++; }
    uint32_t kb = done / 1024u, tkb = total / 1024u;
    n += snes_u32_to_dec(st + n, kb);
    st[n++] = '/';
    n += snes_u32_to_dec(st + n, tkb);
    st[n++] = ' '; st[n++] = 'K'; st[n++] = 'B'; st[n] = 0;
    progress_bar_draw_status(st, PB_COL_EMPTY, PB_COL_BORDER);

    progress_bar_draw(pct, 100, PB_COL_FILL, PB_COL_EMPTY, PB_COL_BORDER);
}
#else
static void snes_romflash_progress(int, uint32_t, uint32_t) {}
#endif

static bool snes9x_load_rom(uintptr_t rom_ptr, size_t romsize, bool read_only)
{
    if (!rom_ptr || !romsize) return false;

    /* Hand the buffer to snes9x. LoadROM(NULL) treats Memory.ROM as
     * already populated; AllocSize doubles as "file size" in that path. */
    Memory.ROM           = (uint8_t *)rom_ptr;
    Memory.ROM_AllocSize = romsize;
    Memory.ROM_Offset    = 0;
    Memory.ROMReadOnly   = read_only;

    if (!LoadROM(NULL)) {
        snprintf(ErrorMessage, ERRORMESSAGESIZE, "Not a SNES ROM.");
        return false;
    }

    /* Reject special-chip ROMs we don't emulate. Emulated and allowed through:
     * DSP-1/2/3/4 (dsp.c), SuperFX/GSU (fxinst.c/fxemu.c), C4 (c4.c/c4emu.c),
     * OBC1 (obc1.c), S-RTC (srtc.c), SA-1 (sa1.c/sa1cpu.c), and -- each behind
     * its own CMake option -- the SPC7110 and its RTC-4513 (ENABLE_SPC7110,
     * spc7110.c) and the S-DD1 (ENABLE_SDD1, sdd1.c). So Super Mario Kart,
     * Pilotwings, Star Fox, Yoshi's Island, Mega Man X2/X3, Metal Combat, Dai
     * Kaijuu Monogatari II, Super Mario RPG, Kirby Super Star, Tengai Makyou
     * Zero, Street Fighter Alpha 2 and Star Ocean all load. With an option
     * off, that chip's carts are refused here instead. Note: SETA (ST01x) and
     * BS-X are unimplemented but cannot be tested for -- InitROM never sets
     * Settings.SETA/BS, so such carts slip through and run without the chip. */
    if (false
#if !ENABLE_SDD1
        || Settings.SDD1
#endif
#if !ENABLE_SPC7110
        || Settings.SPC7110
#endif
       ) {
        snprintf(ErrorMessage, ERRORMESSAGESIZE,
                 "Special chip ROMs not supported.");
        return false;
    }

    S9xReset();
    return true;
}

/* -------------------------------------------------------------------------
 * On-screen FPS overlay. Stamps "NN RN FN" into the top-left of the freshly-
 * rendered SNES frame (RGB555) after S9xMainLoop returns:
 *   NN  frames emulated in the last ~1 s window (g_fps, 60 = full speed)
 *   RN  HSTX video resyncs since boot (cumulative — they should stay rare)
 *   FN  the frameskip in effect: frames skipped after each rendered one
 *   Dn Hn  S-DD1 carts only: ms/s of core0 spent on the chip, and % of the
 *          requested bytes served by its output cache (sdd1.c)
 * RENDER_TO_FB: target is the anchored framebuffer window (stride 320);
 * legacy: g_snes_private_screen (stride SNES_WIDTH) just before the blit,
 * so it rides along with it (incl. the core1 offload path) at no extra
 * sync cost. 8x8 font, white-on-black, from col 4 / overlay rows 0..7.
 *
 * Draws overlay font rows [font_first, font_last] into destination rows
 * [font_first - phys_base .. font_last - phys_base]. Full-overlay callers pass
 * (0, 7, 0); the strip hook passes a sub-range so it can stamp only the rows a
 * given copy-out chunk publishes (see fps_overlay_strip_hook). */
#define FPS_OVERLAY_ROWS 8   /* must match the font_last+1 used below */
#define FPS_OVERLAY_COL  4   /* left margin, pixels */

/* Frames skipped after each rendered frame; 0 = render every frame. The
 * 256x224 blit + the snes9x renderer (RenderScreen/RenderLine/Draw*) is the
 * single biggest non-CPU cost. Super FX and SA-1 games lean hardest on it, so
 * they render 1 frame of every 3 (skip 2); all other games render every other
 * frame (skip 1). Drives both the skip reload in run_emulator() and the F
 * field of the overlay. */
static inline int frameskip_count(void)
{
    return settings.flags.frameSkip ? ((Settings.SuperFX || Settings.SA1) ? 2 : 1) : 0;
}

/* The overlay text, rebuilt once per second by the window in run_emulator().
 * The draw below runs per rendered frame (per copy-out chunk, even), so it
 * stays a plain glyph blit — no formatting in the hot path. Written and read
 * on core0 only, so no locking. */
static char g_fps_text[24] = "60 R0 F0";

static void draw_fps_overlay(uint16_t *screen, int stride,
                             int font_first, int font_last, int phys_base)
{
    const uint16_t fg = 0x7FFF;  /* white, RGB555 */
    const uint16_t bg = 0x0000;  /* black         */
    int maxchars = (stride - FPS_OVERLAY_COL) / FONT_CHAR_WIDTH;
    if (maxchars > (int)sizeof(g_fps_text) - 1) maxchars = (int)sizeof(g_fps_text) - 1;
    for (int row = font_first; row <= font_last; row++) {
        uint16_t *dst = screen + (row - phys_base) * stride + FPS_OVERLAY_COL;
        for (int i = 0; i < maxchars && g_fps_text[i]; i++) {
            char sl = getcharslicefrom8x8font(g_fps_text[i], row); /* LSB = leftmost pixel */
            for (int b = 0; b < 8; b++) { *dst++ = (sl & 1) ? fg : bg; sl >>= 1; }
        }
    }
}

#if HSTX && RENDER_TO_FB
/* Registered as s9x_port_strip_top_hook: the strip copy-out calls this just
 * before it publishes a chunk whose rows overlap the overlay band, so the
 * digits are baked into the frame's own pixels instead of stamped into the
 * live FB afterwards (which flickered). A chunk publishes absolute rows
 * [block_start, block_end] and strip physical row 0 == absolute block_start
 * (the strip is repointed per chunk). Games that flush a short redraw across
 * the top of the frame (DKC changes a PPU register within the first few
 * scanlines) split the overlay band over several chunks — each must stamp
 * only the overlay rows IT copies out, else one chunk carries the digits and
 * the next overwrites the rest with game pixels, leaving a white sliver. */
static void fps_overlay_strip_hook(uint16_t *strip, int stride,
                                   int block_start, int block_end)
{
    if (!settings.flags.displayFrameRate) return;
    int last = block_end < FPS_OVERLAY_ROWS - 1 ? block_end : FPS_OVERLAY_ROWS - 1;
    draw_fps_overlay(strip, stride, block_start, last, block_start);
}
#endif

/* -------------------------------------------------------------------------
 * Cartridge battery SRAM persistence. snes9x keeps the save in Memory.SRAM;
 * the real battery size is Memory.SRAMMask+1 when Memory.SRAMSize>0 (and there
 * is no battery when SRAMSize==0). Saves live in /SAVES/SNES/<rom>.SAV.
 *
 * SPC7110 carts (Tengai Makyou Zero) additionally carry an RTC-4513, and this
 * board has no clock to seed it from. The chip therefore starts unset, which
 * is what a dead cart battery looks like and makes the game run its own "set
 * the date" prompt; what the player enters is kept in a 32-byte trailer
 * appended after the battery region here. The trailer is written only when
 * Settings.SPC7110RTC, so no other cart's .SAV changes size, and it is only
 * read back when the file is exactly battery+trailer long and the magic and
 * checksum both agree -- an .SAV from an older build (or from another
 * emulator) simply has no trailer and the game prompts again. The clock does
 * not advance while the board is off; without an RTC chip it cannot.
 *
 * snes9x's own S9xSRTCPreSaveState trailer (srtc.c, the Sharp S-RTC used by
 * Dai Kaijuu Monogatari II) is a different chip and is still never called --
 * that clock does still restart each power cycle.
 *
 * FIL (~550 B, embeds a 512 B sector window) and FILINFO (~276 B) are far too
 * large for the 3 KB core0 stack (PICO_STACK_SIZE) — allocate them in PSRAM via
 * Frens::f_malloc / f_free (panics on OOM, so never NULL). */
#define SNES_SAVE_DIR (GAMESAVEDIR "/SNES")   /* "/SAVES/SNES" */

static void snes_sram_path(char *out, size_t n)
{
    char base[FF_MAX_LFN];
    strncpy(base, Frens::GetfileNameFromFullPath(romName), sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    Frens::stripextensionfromfilename(base);
    snprintf(out, n, "%s/%s.SAV", SNES_SAVE_DIR, base);
}

#if ENABLE_SPC7110
/* 32 bytes: magic, version, the 20 RTC registers, and a checksum over the
 * lot. Fixed size and self-describing, so a truncated or corrupt trailer is
 * rejected rather than injecting garbage BCD into the chip. */
#define SNES_RTC_TRAILER_SIZE 32
#define SNES_RTC_TRAILER_MAGIC "S7RT"

static uint32_t snes_rtc_trailer_sum(const uint8_t *t)
{
    uint32_t sum = 0;
    for (int i = 0; i < SNES_RTC_TRAILER_SIZE - 4; i++)
        sum = (sum << 1) + (sum >> 31) + t[i];
    return sum;
}

static void snes_rtc_trailer_build(uint8_t *t)
{
    memset(t, 0, SNES_RTC_TRAILER_SIZE);
    memcpy(t, SNES_RTC_TRAILER_MAGIC, 4);
    t[4] = 1;                                   /* version */
    S9xSPC7110RTCExport(t + 8);                 /* 20 bytes */
    uint32_t sum = snes_rtc_trailer_sum(t);
    t[28] = (uint8_t)sum;         t[29] = (uint8_t)(sum >> 8);
    t[30] = (uint8_t)(sum >> 16); t[31] = (uint8_t)(sum >> 24);
}

static bool snes_rtc_trailer_valid(const uint8_t *t)
{
    if (memcmp(t, SNES_RTC_TRAILER_MAGIC, 4) != 0) return false;
    if (t[4] != 1) return false;
    uint32_t sum = snes_rtc_trailer_sum(t);
    return t[28] == (uint8_t)sum       && t[29] == (uint8_t)(sum >> 8)
        && t[30] == (uint8_t)(sum >> 16) && t[31] == (uint8_t)(sum >> 24);
}
#endif /* ENABLE_SPC7110 */

static void snes_load_sram(void)
{
    if (Memory.SRAMSize == 0) return;                 /* no battery */
    size_t sz = (size_t)Memory.SRAMMask + 1;
    memset(Memory.SRAM, 0, sz);                       /* deterministic first boot */

    char path[FF_MAX_LFN];
    snes_sram_path(path, sizeof(path));

    FILINFO *fno = (FILINFO *)Frens::f_malloc(sizeof(FILINFO));
    if (f_stat(path, fno) != FR_OK) {
        printf("SRAM: no save file %s\n", path);
        Frens::f_free(fno);
        return;
    }
    FIL *file = (FIL *)Frens::f_malloc(sizeof(FIL));
    if (f_open(file, path, FA_READ) == FR_OK) {
        UINT br = 0;
        UINT toread = (fno->fsize < sz) ? (UINT)fno->fsize : (UINT)sz;
        if (f_read(file, Memory.SRAM, toread, &br) == FR_OK)
            printf("SRAM: loaded %u bytes from %s\n", (unsigned)br, path);
        else
            printf("SRAM: read error %s\n", path);
#if ENABLE_SPC7110
        /* The RTC trailer sits immediately after the battery region. Anything
         * shorter is a pre-trailer save and leaves the clock unset. */
        if (Settings.SPC7110RTC && fno->fsize >= sz + SNES_RTC_TRAILER_SIZE) {
            uint8_t trailer[SNES_RTC_TRAILER_SIZE];
            UINT tr = 0;
            if (f_lseek(file, sz) == FR_OK &&
                f_read(file, trailer, sizeof(trailer), &tr) == FR_OK &&
                tr == sizeof(trailer) && snes_rtc_trailer_valid(trailer)) {
                S9xSPC7110RTCImport(trailer + 8);
                printf("SRAM: RTC-4513 restored from %s\n", path);
            } else {
                printf("SRAM: RTC trailer rejected in %s\n", path);
            }
        }
#endif
        f_close(file);
    } else {
        printf("SRAM: cannot open %s for read\n", path);
    }
    Frens::f_free(file);
    Frens::f_free(fno);
}

static void snes_save_sram(void)
{
    if (Memory.SRAMSize == 0) return;                 /* no battery */
    size_t sz = (size_t)Memory.SRAMMask + 1;

    f_mkdir(SNES_SAVE_DIR);                            /* idempotent; FR_EXIST ok */

    char path[FF_MAX_LFN];
    snes_sram_path(path, sizeof(path));

    FIL *file = (FIL *)Frens::f_malloc(sizeof(FIL));
    if (f_open(file, path, FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
        UINT bw = 0;
        if (f_write(file, Memory.SRAM, (UINT)sz, &bw) == FR_OK)
            printf("SRAM: saved %u bytes to %s\n", (unsigned)bw, path);
        else
            printf("SRAM: write error %s\n", path);
#if ENABLE_SPC7110
        if (Settings.SPC7110RTC) {
            uint8_t trailer[SNES_RTC_TRAILER_SIZE];
            UINT tw = 0;
            snes_rtc_trailer_build(trailer);
            if (f_write(file, trailer, sizeof(trailer), &tw) == FR_OK &&
                tw == sizeof(trailer))
                printf("SRAM: RTC-4513 trailer appended\n");
            else
                printf("SRAM: RTC trailer write error\n");
        }
#endif
        f_close(file);
    } else {
        printf("SRAM: cannot open %s for write\n", path);
    }
    Frens::f_free(file);
}

/* -------------------------------------------------------------------------
 * One emulator session — runs until the user quits to the ROM menu.
 * In-game reset is handled in place via S9xReset(). */
static void run_emulator(void)
{
#if !RENDER_TO_FB
    /* Compute centered placement of native SNES frame in 320x240 HSTX FB. */
    const int snes_h = Settings.PAL ? SNES_HEIGHT_EXTENDED : SNES_HEIGHT;
    const int marginTop = (240 - snes_h) / 2;
    const int marginLeft = (320 - SNES_WIDTH) / 2;
#endif

#if HSTX
    uint16_t * const fb = (uint16_t *)hstx_getframebuffer();
    /* Clear border once. SNES region gets overwritten every rendered frame. */
    memset(fb, 0, 320 * 240 * sizeof(uint16_t));
#endif

    if (!S9xInitDisplay()) { snprintf(ErrorMessage, ERRORMESSAGESIZE, "Display init failed"); return; }
    Frens::dumpHeapStats("after-Display");
    if (!S9xInitGFX())     { snprintf(ErrorMessage, ERRORMESSAGESIZE, "GFX init failed");     return; }
    Frens::dumpHeapStats("after-GFX");

#if HSTX && MIX_ON_CORE1
    /* Sound is fully initialized (S9xInitSound + S9xSetPlaybackRate ran
     * in main()) — core1 may start mixing. */
    mix_c1_resume();
#endif

    paceFrame(true);

    uint32_t frame = 0;
    uint8_t  skipFrames = 0;
#if HSTX && RENDER_TO_FB
    int last_screen_h = PPU.ScreenHeight;
    /* Stamp the FPS overlay inside the strip copy-out (see hook comment) so it
     * publishes with the frame — never a visible gap on the live scan-out. */
    s9x_port_strip_top_hook = fps_overlay_strip_hook;
#endif
#if HSTX
    uint32_t audio_min_level = UINT32_MAX;
    uint32_t audio_underruns_last = hstx_di_queue_get_underrun_count();
#endif
    while (true) {
#if PROFILE_BUCKETS
        uint32_t t0 = time_us_32();
#endif
        host_tick();
#if PROFILE_BUCKETS
        uint32_t t1 = time_us_32(); prof_us_host_tick += (t1 - t0);
#endif

        if (wantsMenu()) {
#if HSTX && BLIT_ON_CORE1
            /* Menu repaints the framebuffer — wait for any in-flight blit. */
            blit_wait_done();
#endif
#if HSTX && MIX_ON_CORE1
            /* The menu's wavplayer is the other DI-queue producer, and
             * menu actions may touch sound state — park the mixer. */
            mix_c1_park();
#endif
#if ENABLE_MSU1
            /* Strictly after the core1 mixer has parked and acked, so core1
             * is provably outside msu1_mix; and before the menu (which reads
             * and writes settings) takes the SD card. */
            msu1_park();
#endif
            int r = showSettingsMenu(true);
            /* Whatever button confirmed the menu item is probably still down.
             * Do not let the game see it -- see g_pad_ignore_request. */
            g_pad_ignore_request = true;
            if (r == 3) {
#if 0
                if ((clock_get_hz(clk_sys) / 1000) > EMULATOR_CLOCKFREQ_KHZ)
                {
                    /* Quit game. Instead of returning to main()'s in-place
                     * teardown + menu re-entry (S9xDeinit*, core1 park/resume,
                     * wav-player reset, screen-mode switch) — a fragile sequence
                     * that intermittently core1-hardfaults at the 504 MHz OC
                     * margin (undefined-instruction fetch glitch during the
                     * transition) — flush the battery save and hard-reboot to a
                     * clean boot + fresh ROM menu. The mixer is already parked;
                     * watchdog_reboot resets both cores and all peripherals, so
                     * there is no teardown to get wrong. */
                    snes_save_sram();
                    watchdog_reboot(0, 0, 0);
                    while (true)
                        tight_loop_contents(); /* not reached */
                }
#endif
                return;
            }
            if (r == 5) {
                /* Flush the battery save first. Memory.SRAM survives
                 * S9xReset, so this is not needed for the reset itself — but
                 * SPC7110 carts run a multi-stage power-on self-test whose
                 * progress lives in cart SRAM, and the player is expected to
                 * reset between stages. Without a flush here, pulling power
                 * after a reset loses that progress and the cart starts the
                 * diagnostic over. Cheap: one SD write per explicit reset. */
                snes_save_sram();
                /* Reset game. Do it while the mixer is still parked —
                 * S9xReset reinitializes the APU/DSP state core1 mixes
                 * from. playback_rate is untouched, so audio survives. */
                S9xReset();
#if ENABLE_MSU1
                /* The cart is back at its reset vector; silence any track it
                 * had going so the old music does not survive into the boot
                 * screen. Files and buffers stay allocated. */
                msu1_reset();
#endif
            }
            /* Repaint border in case the menu touched the framebuffer. */
#if HSTX
            memset(fb, 0, 320 * 240 * sizeof(uint16_t));
#endif
#if ENABLE_MSU1
            msu1_resume();          /* mirror of the park order above */
#endif
#if HSTX && MIX_ON_CORE1
            mix_c1_resume();
#endif
            paceFrame(true);
        }

#if HSTX && RENDER_TO_FB
        /* Overscan bit flipped ScreenHeight 224<->239: re-anchor the
         * centered copy-out window and repaint the border. (A flip
         * mid-rendered-frame is contained by the EndY clamp in gfx.c
         * until this catches it.) */
        if (PPU.ScreenHeight != last_screen_h) {
            last_screen_h = PPU.ScreenHeight;
            s9x_port_anchor_screen();
            memset(fb, 0, 320 * 240 * sizeof(uint16_t));
        }
#endif

        IPPU.RenderThisFrame = (skipFrames == 0);

#if HSTX && BLIT_ON_CORE1
        /* S9xMainLoop overwrites GFX.Screen on rendered frames; the
         * previous blit must have consumed it first. With frameskip on
         * the blit finished two frames ago and this never spins. */
        if (IPPU.RenderThisFrame) blit_wait_done();
#endif

#if PROFILE_BUCKETS
        /* Re-apply the A/B toggle each frame: bypass may have been
         * defeated by DMA / port writes since last frame. */
        dbg_apply_apu_bypass(g_prof_bypass_apu);
        bool rendered_this_frame = IPPU.RenderThisFrame;
        uint32_t t2 = time_us_32();
#endif
#if HSTX && RENDER_TO_FB
        const bool rendering = IPPU.RenderThisFrame;
        if (rendering)
            s9x_port_tear_frame_begin();
#endif
        S9xMainLoop();
#if HSTX && RENDER_TO_FB
        if (rendering) {
            int32_t psi_mid;
            bool    torn;
            if (s9x_port_tear_frame_end(&psi_mid, &torn)) {
#if PACE_VSYNC_PHASE
                if (!Settings.PAL)
                    pace_note_frame(psi_mid, torn);
#endif
            }
        }
#endif
#if PROFILE_BUCKETS
        uint32_t t3 = time_us_32();
        {
            uint32_t d = t3 - t2;
            if (rendered_this_frame) { prof_us_main_r += d; prof_frames_r++; }
            else                     { prof_us_main_s += d; prof_frames_s++; }
        }
#endif

#if HSTX && RENDER_TO_FB
        /* No post-loop overlay stamp here: fps_overlay_strip_hook already
         * stamped the digits INTO the top strip during the copy-out, so they
         * were published with the frame's pixels. Stamping into the live
         * single-buffered FB after the copy-out (as this used to) left a gap
         * where scan-out saw the top rows without the overlay — it flickered. */
#elif HSTX
        /* Blit private PSRAM screen → HSTX SRAM framebuffer, centered.
         * 256*224*2 = 112 KB per NTSC frame (~2.5 ms of PSRAM reads). */
        if (IPPU.RenderThisFrame) {
            /* Stamp the FPS digits into the source frame before the blit so
             * they ride along with it (incl. the core1 offload) — no extra
             * sync, no single-buffered-scanout timing hazard. */
            if (settings.flags.displayFrameRate)
                draw_fps_overlay(g_snes_private_screen, SNES_WIDTH, 0, FPS_OVERLAY_ROWS - 1, 0);
#if BLIT_ON_CORE1
            /* Hand the copy to core1's idle loop; core0 moves straight on
             * to emulating the next frame. */
            blit_submit(g_snes_private_screen,
                        fb + marginTop * 320 + marginLeft, snes_h);
#else
            const uint16_t * __restrict src = g_snes_private_screen;
            uint16_t       * __restrict dst = fb + marginTop * 320 + marginLeft;
            for (int y = 0; y < snes_h; y++) {
                memcpy(dst, src, SNES_WIDTH * sizeof(uint16_t));
                src += SNES_WIDTH;
                dst += 320;
            }
#endif
        }
#endif
#if PROFILE_BUCKETS
        uint32_t t4 = time_us_32(); prof_us_blit += (t4 - t3);
#endif

        if (skipFrames == 0) {
            /* frameSkip=true → skip frames to buy back frame budget. */
            skipFrames = (uint8_t)frameskip_count();
        } else {
            skipFrames--;
        }

#if ENABLE_SPC7110 && !SPC7110_FREEZE_RTC
        /* Advance the SPC7110's RTC-4513 on real elapsed time. Upstream
         * counts frames and assumes 60 of them per second; this port does not
         * reliably hit 60 and can be running frameskip, so a frame-counted
         * clock would run slow by however far behind the emulator is. A no-op
         * for every cart without the chip. */
        S9xSPC7110RTCTick(time_us_64());
#endif
#if ENABLE_MSU1
        /* Every MSU-1 SD access happens here — the deferred track open and
         * the ring refill (~2949 B/frame while a track plays). Placed at the
         * end of the frame's work so the blocking f_read is absorbed by the
         * pacing slack that paceFrame() is about to sleep away, and inside
         * the PROFILE_BUCKETS "pump" bucket so its cost is measurable. */
        msu1_pump();
#endif
#if !(HSTX && MIX_ON_CORE1)
        pump_audio();
#endif
#if PROFILE_BUCKETS
        uint32_t t5 = time_us_32(); prof_us_pump += (t5 - t4);
        if (!g_prof_bypass_pace)
#endif
        paceFrame(false, skipFrames == 0);
#if PROFILE_BUCKETS
        uint32_t t6 = time_us_32(); prof_us_pace += (t6 - t5);
#endif
        frame++;

#if HSTX
        /* Audio health tracking (always on — dropouts were reported in
         * release builds): min DI-queue level per second, sampled once
         * per frame. Underrun ground truth comes from the queue itself.
         * Only meaningful when audio routes to HDMI — with external I2S
         * audio the DI queue receives nothing and underruns by design. */
#if EXT_AUDIO_IS_ENABLED
        if (!audio_route_to_ext())
#endif
        {
            uint32_t lvl = hstx_di_queue_get_level();
            if (lvl < audio_min_level) audio_min_level = lvl;
        }
#endif

#if PROFILE_BUCKETS
        /* 4-state cycle, ~5 s per state (starts at state 0 so the first
         * boot handshakes settle before the first flip):
         *   0: baseline (APU on,  pace on)
         *   1: no APU   (APU off, pace on)
         *   2: no pace  (APU on,  pace off) — raw emulator fps
         *   3: no both  (APU off, pace off) */
        {
            static uint64_t ab_t0_us = 0;
            static uint8_t  ab_state = 0;
            uint64_t nowab = Frens::time_us();
            if (ab_t0_us == 0) ab_t0_us = nowab;
            if (nowab - ab_t0_us >= 5000000) {
                ab_state = (ab_state + 1) & 3;
                g_prof_bypass_apu  = (ab_state & 1) != 0;
                g_prof_bypass_pace = (ab_state & 2) != 0;
                ab_t0_us = nowab;
            }
        }
#endif

        /* Once-per-second window: drive the on-screen FPS overlay (g_fps) and
         * the audio-health readout below. The frame count over ~1 s is the
         * windowed average the overlay wants — immune to soft-pacing/frameskip
         * flicker. */
        static uint64_t fps_t0_us = 0;
        static uint32_t fps_f0 = 0;
        uint64_t now = Frens::time_us();
        if (fps_t0_us == 0) { fps_t0_us = now; fps_f0 = frame; }
        else if (now - fps_t0_us >= 1000000) {
            uint32_t delta = frame - fps_f0;
            g_fps = delta;
            /* Overlay line: fps, cumulative video resyncs, frameskip in
             * effect. Resyncs are a since-boot total on purpose — they are
             * rare, and a per-second value would blink past unnoticed. */
            {
                int resyncs = 0;
#if HSTX
                resyncs = get_video_output_resync_count();
#endif
                int n = snprintf(g_fps_text, sizeof(g_fps_text), "%02lu R%d F%d",
                                 (unsigned long)(delta > 99 ? 99 : delta), resyncs,
                                 frameskip_count());
#if ENABLE_SDD1
                /* S-DD1 carts append "Dn Hn": ms of core0 time this second
                 * spent on the chip, and % of the requested bytes the output
                 * cache supplied ("H-" when the game asked for nothing). */
                uint32_t sd_us, sd_req, sd_dec;
                if (n > 0 && n < (int)sizeof(g_fps_text) &&
                    sdd1_take_stats(&sd_us, &sd_req, &sd_dec)) {
                    if (sd_req)
                        snprintf(g_fps_text + n, sizeof(g_fps_text) - n, " D%lu H%lu",
                                 (unsigned long)((sd_us + 500) / 1000),
                                 (unsigned long)((uint64_t)(sd_req - sd_dec) * 100 / sd_req));
                    else
                        snprintf(g_fps_text + n, sizeof(g_fps_text) - n, " D%lu H-",
                                 (unsigned long)((sd_us + 500) / 1000));
                }
#endif
            }
#if TEAR_STATS && HSTX && RENDER_TO_FB
            {
                /* Tear guard health: rendered frames, how many reached the
                 * screen torn, strips that waited for the beam (and for how
                 * long in total), the widest psi range (> one refresh, i.e.
                 * the line count, cannot be placed tear-free) and the start
                 * phase the pacer is holding, in lines after the vsync tick. */
                uint32_t tf, tt, tw, twu, tspan;
                s9x_port_tear_stats(&tf, &tt, &tw, &twu, &tspan);
                long phase = -1;
#if PACE_VSYNC_PHASE
                if (!Settings.PAL) {
                    int32_t p = (int32_t)(pace_slot - video_frame_count * (uint32_t)PACE_LINES)
                                % PACE_LINES;
                    phase = p < 0 ? p + PACE_LINES : p;
                }
#endif
                printf("tear: frames=%lu torn=%lu waits=%lu wait=%luus span=%lu phase=%ld\n",
                       (unsigned long)tf, (unsigned long)tt, (unsigned long)tw,
                       (unsigned long)twu, (unsigned long)tspan, phase);
            }
#endif
#if PROFILE_BUCKETS
            uint32_t d  = delta ? delta : 1;
            uint32_t dr = prof_frames_r ? prof_frames_r : 1;
            uint32_t ds = prof_frames_s ? prof_frames_s : 1;
            printf("fps=%lu PAL=%d skip=%d bypAPU=%d bypPACE=%d "
                   "us/frm host=%lu mainR=%lu(x%lu) mainS=%lu(x%lu) "
                   "blit=%lu pump=%lu pace=%lu sfx=%lu(x%lu)\n",
                   (unsigned long)delta, (int)Settings.PAL, settings.flags.frameSkip,
                   (int)g_prof_bypass_apu, (int)g_prof_bypass_pace,
                   (unsigned long)(prof_us_host_tick / d),
                   (unsigned long)(prof_us_main_r / dr), (unsigned long)prof_frames_r,
                   (unsigned long)(prof_us_main_s / ds), (unsigned long)prof_frames_s,
                   (unsigned long)(prof_us_blit / d),
                   (unsigned long)(prof_us_pump / d),
                   (unsigned long)(prof_us_pace / d),
                   (unsigned long)(g_prof_us_sfx / d),
                   (unsigned long)(g_prof_sfx_runs / d));
            prof_us_host_tick = prof_us_main_r = prof_us_main_s =
                prof_us_blit = prof_us_pump = prof_us_pace = 0;
            prof_frames_r = prof_frames_s = 0;
            g_prof_us_sfx = g_prof_sfx_runs = 0;
#endif
#if HSTX
#if EXT_AUDIO_IS_ENABLED
            if (!audio_route_to_ext())
#endif
            {
#if AUDIO_WATCHDOG
                audio_watchdog_tick();
#endif
                uint32_t ur = hstx_di_queue_get_underrun_count();
                /* Only chatter when audio health is abnormal. minlvl is
                 * DI packets (4 samples each); watermark is 200. */
                if (ur != audio_underruns_last || audio_min_level < 20) {
                    printf("audio: underruns+%lu minlvl=%lu resyncs=%d\n",
                           (unsigned long)(ur - audio_underruns_last),
                           (unsigned long)audio_min_level,
                           get_video_output_resync_count());
                }
                audio_underruns_last = ur;
                audio_min_level = UINT32_MAX;
            }
#endif
#if ENABLE_MSU1
            /* Silent unless the card is struggling — same "only chatter when
             * abnormal" rule as the audio-health block above. Build with
             * -DMSU1_VERBOSE=ON for a line every second while a track plays,
             * which is how to characterise a new SD card. */
            msu1_stats_report();
#endif
            fps_t0_us = now;
            fps_f0 = frame;
        }
    }
}

int main()
{
    romName = selectedRom;
    ErrorMessage[0] = selectedRom[0] = 0;
    // Set min/max CPU freq and voltage limits for this board for overclocking. 
    // For 504 Mhz, 1.7V seems the minimum stable voltage.  Should run fine at 1.6 or 1.65 Volt, but
    // may cause hardfaults on heavy scenes. 1.7V is the safe limit for 504 Mhz. 
    // BUT CAN CAUSE DAMAGE !!!!!!
    //1.6V is the safe limit for 378 Mhz.
    vreg_voltage voltage = vreg_voltage::VREG_VOLTAGE_1_50;
#if HW_CONFIG == 2 || HW_CONFIG == 8
    Frens::setOverclockLimits(EMULATOR_CLOCKFREQ_KHZ,  EMULATOR_MAX_CLOCKFREQ_KHZ, 
                              voltage,vreg_voltage::VREG_VOLTAGE_1_70);
   
     Frens::FlashParams *flashParams;
    // assign flashParams to point to flash location

    flashParams = (Frens::FlashParams *)FLASHPARAM_ADDRESS;
  
    if ( Frens::validateFlashParams(*flashParams) ) {
        CPUFreqKHz = flashParams->cpuFreqKHz;
        voltage = flashParams->voltage;
    }
#else
    // No overclock here, but the menu still compares the live clock with these
    // limits whenever settings are saved. Left at the pico_shared defaults
    // (252 MHz) that check rewrote FlashParams and rebooted on every save.
    Frens::setOverclockLimits(EMULATOR_CLOCKFREQ_KHZ, EMULATOR_CLOCKFREQ_KHZ, voltage, voltage);
#endif   
    Frens::setClocksAndStartStdio(CPUFreqKHz, voltage);
    Frens::dumpHeapStats("startup");

    printf("==========================================================================================\n");
    printf("pico_snesPlus (snes9x core)\n");
    printf("Build: %s %s\n", __DATE__, __TIME__);
    printf("CPU freq: %d kHz\n", clock_get_hz(clk_sys) / 1000);
#if HSTX
    printf("HSTX freq: %d kHz\n", clock_get_hz(clk_hstx) / 1000);
#endif
    printf("Stack size: %d bytes\n", PICO_STACK_SIZE);
    printf("==========================================================================================\n");

    FrensSettings::initSettings(FrensSettings::emulators::SNES);

#if HSTX && (BLIT_ON_CORE1 || MIX_ON_CORE1)
    /* Register BEFORE initAll launches core1: background_task isn't
     * volatile in video_output.c, so core1's loop may legally cache it —
     * registering first guarantees visibility. The task no-ops until the
     * first blit_submit() / mix_c1_resume(). */
#if MIX_ON_CORE1
    port_sound_lock_init();
#endif
    video_output_set_background_task(core1_background_task);
#endif

    /* Framebuffer mode + 1024-byte audio buffer on RP2350. */
    isFatalError = !Frens::initAll(selectedRom, CPUFreqKHz, 0, 0,
                                   AUDIOBUFFERSIZE, false, true);
    Frens::dumpHeapStats("after-initAll");
#if HSTX && RENDER_TO_FB
    /* Start tracking the scan-out beam for the strip tear guard. */
    s9x_port_beam_install();
#endif

#if HSTX
    /* Override the 44.1 kHz default that hstx_init() in pico_shared
     * hardcodes. Reconfigures the ACR N/CTS values and the DI queue's
     * samples-per-line accumulator to match SNES_AUDIO_HZ. */
    pico_hdmi_set_audio_sample_rate(SNES_AUDIO_HZ);
#endif

    Frens::applyScreenMode(settings.screenMode);

    g_settings_visibility = g_settings_visibility_snes;
    g_available_screen_modes = g_available_screen_modes_snes;

    /* Skip the splash when we got here via the quit-game reboot
     * (watchdog_reboot in run_emulator) — it should feel like a snappy return
     * to the ROM menu, not a fresh power-on. A cold/power-on boot still shows
     * it (watchdog_caused_reboot() is false then). */
    /* Resume after a ROM-to-flash write. Writing a 7 MB cart means holding
     * interrupts off for a few hundred ms at a time, once per 64 KB erase --
     * and on PIO USB boards the host controller cannot survive that: the
     * gamepad stops producing valid reports ("Invalid DS4 report size 0") and
     * does not come back. Rather than try to nurse the USB stack through it,
     * reboot once the write is done -- USB, core1 and the QMI all come back
     * clean -- and pick the cart straight back up here so the user still only
     * chose it once. scratch[5] is free: [4] holds the SDK's watchdog_enable
     * magic, [6]/[7] are the bootloader handshake (FrensHelpers.cpp). The
     * reboot is a watchdog_enable(), not a watchdog_reboot(): pico-bootLoader
     * jumps straight back into the resident application only after a
     * watchdog_enable reboot, and shows its menu after any other. The other
     * effect of that magic, initAll() flashing the rom named in ROMINFOFILE,
     * needs a board without PSRAM, which this emulator does not run on. */
    bool showSplash = !watchdog_caused_reboot();
    bool resumedFromFlashWrite = false;
    if (watchdog_hw->scratch[SNES_RESUME_SCRATCH] == SNES_RESUME_MAGIC) {
        watchdog_hw->scratch[SNES_RESUME_SCRATCH] = 0;
        /* The path comes from the flash record, NOT from ROMINFOFILE. That
         * file lives at the SD root and every Frens emulator writes it, so it
         * routinely names another console's cart -- resuming from it once
         * flashed a 384 KB NES ROM into the SNES region and then boot-looped.
         * The record is written by the very write we are resuming from, so it
         * is both SNES-specific and exactly right. */
        const char *rec = snes_romflash_recorded_path();
        if (rec && rec[0]) {
            strncpy(selectedRom, rec, sizeof(selectedRom) - 1);
            selectedRom[sizeof(selectedRom) - 1] = 0;
            resumedFromFlashWrite = true;
            showSplash = false;
            printf("romflash: resuming %s after the flash write\n", selectedRom);
        } else {
            printf("romflash: resume asked for, but the record is invalid\n");
        }
    }

    while (true) {
        if (selectedRom[0] == 0) {
            /* Free the previous session's PSRAM ROM copy before the menu
             * lists the directory. loadRomInPsRam only frees it lazily when
             * the NEXT rom is picked, so without this the romlister sees
             * romsize fewer free bytes after a quit and hides carts that
             * would still fit (e.g. DKC's own 4 MB right after quitting it).
             * Safe here: every path back to the menu has either never set
             * Memory.ROM or already ran S9xDeinitMemory, which NULLs it.
             * No-op on cold boot (ROM_FILE_ADDR == 0); guarded because on
             * flash-only builds ROM_FILE_ADDR is a flash address, not a
             * heap pointer. */
            if (Frens::isPsramEnabled()) {
                Frens::f_free((void *)ROM_FILE_ADDR);
                ROM_FILE_ADDR = 0;
            }
            const char *romExtensions = ".smc .sfc";
            menu("Pico-snes+", ErrorMessage, isFatalError, showSplash,
                 romExtensions, selectedRom);
            showSplash = false;
            printf("Selected ROM: %s\n", selectedRom);
            Frens::dumpHeapStats("after-menu");
        }

        /* The framework's menu already copied the ROM into PSRAM and
         * stored the pointer in ROM_FILE_ADDR. We just need its size. */
        FIL *fil = (FIL *)Frens::f_malloc(sizeof(FIL));
        size_t romsize = 0;
        if (f_open(fil, selectedRom, FA_READ) == FR_OK) {
            romsize = f_size(fil);
            f_close(fil);
        }
        Frens::f_free(fil);
        if (!romsize) {
            strcpy(ErrorMessage, "ROM load failed");
            selectedRom[0] = 0;
            continue;
        }

        /* ROM_FILE_ADDR == 0 with a valid size means the framework did not
         * preload the cart -- either it skipped it (file larger than
         * availMem - 512 KB) or the allocation failed outright. The latter is
         * routine for a 7 MB cart after a couple of games: lwmem allocates
         * next-fit and GetAvailableMemory() reports total free bytes rather
         * than the largest run, so the arena can report 8 MB free and still
         * not hold 7 MB contiguously. Either way the cart runs from XIP flash
         * instead, which is where it was headed anyway. */
        uintptr_t rom_addr   = ROM_FILE_ADDR;
        bool      rom_in_flash = false;

        /* The framework preloads any ROM that fits PSRAM -- but merely fitting
         * is not enough. snes9x still needs ~1.04 MB after the ROM for
         * Memory.RAM/VRAM/SRAM, TileCache, FillRAM, the sound LocalState, the
         * render strips and the sprite line buffer. Measured on the 7 MB
         * Tengai Makyou Zero patch: the preload leaves exactly 1023 KB and the
         * last allocation in S9xInitDisplay (s9x_port_objonline, ~30 KB) comes
         * back NULL -- the session dies with "Display init failed" after the
         * ROM has already been read off the card.
         *
         * So decide on the working set, not on the ROM alone: if too little
         * PSRAM would be left, drop the preloaded copy and run the cart from
         * XIP flash instead (romflash.h), which frees the whole 8 MB for the
         * emulator. A 4 MB cart leaves ~4 MB and is untouched by this. */
        if (rom_addr) {
            uint freeAfterPreload = Frens::GetAvailableMemory();
            size_t need = snes_psram_working_set();
            if (freeAfterPreload < need) {
                printf("romflash: %u KB PSRAM left after preloading %u KB, "
                       "emulator needs %u KB - running this cart from flash\n",
                       (unsigned)(freeAfterPreload / 1024),
                       (unsigned)(romsize / 1024),
                       (unsigned)(need / 1024));
                Frens::f_free((void *)rom_addr);
                ROM_FILE_ADDR = 0;
                rom_addr      = 0;
            }
        }

        if (!rom_addr) {
            /* Drop anything the framework left in ErrorMessage. If the preload
             * failed it will hold "Cannot allocate ... bytes in PSRAM", which
             * is not an error here -- running this cart from flash is the plan.
             * Left set, it surfaces on the menu later: most visibly when the
             * user declines the write below and gets a PSRAM complaint about a
             * cart the emulator never intended to keep in PSRAM. Anything
             * genuinely wrong from here on sets its own message. */
            ErrorMessage[0] = 0;

            if (romsize > snes_romflash_capacity()) {
                snprintf(ErrorMessage, ERRORMESSAGESIZE, "ROM too large");
                selectedRom[0] = 0;
                continue;
            }

            /* Write it if it is not already there. On the launch we rebooted
             * into, skip the check entirely: the image was verified against
             * the source moments ago and the record is that proof, so
             * re-CRCing 7 MB would only cost time -- and under
             * ROMFLASH_FORCE_REWRITE holds() always answers "no", which would
             * otherwise rewrite, reboot, and land right back here: a boot
             * loop, and a testing build that can never reach the game. */
            if (!resumedFromFlashWrite && !snes_romflash_holds(selectedRom, romsize)) {
                /* Ask first. This is a minute of the console being unusable
                 * and a write to the board's flash, so it should never be a
                 * surprise -- and the user may simply have picked the wrong
                 * cart. No means straight back to the menu, nothing written. */
                const char *shortName = Frens::GetfileNameFromFullPath(selectedRom);
                char sizeLine[40];
                snprintf(sizeLine, sizeof(sizeLine), "%u KB - takes about a minute",
                         (unsigned)(romsize / 1024));
                if (!menuConfirmPrompt("This cart is too big for RAM and",
                                       "must be written to flash first.",
                                       sizeLine)) {
                    printf("romflash: user declined the write\n");
                    selectedRom[0] = 0;
                    continue;
                }

                /* Leave a notice on screen; the bar is drawn over it. */
                menuNoticeScreen("Writing to flash memory", shortName,
                                 "Do not power off.",
                                 "The console restarts when done.");
                progress_bar_draw(0, 100, PB_COL_FILL, PB_COL_EMPTY, PB_COL_BORDER);

                if (!snes_romflash_program(selectedRom, romsize,
                                           snes_romflash_progress)) {
                    snprintf(ErrorMessage, ERRORMESSAGESIZE, "Flash write failed");
                    selectedRom[0] = 0;
                    continue;
                }
                /* Written and verified. Reboot to get a clean USB host back
                 * (see SNES_RESUME_SCRATCH above) and resume this cart there;
                 * the record then names it and it starts straight from XIP. */
                printf("romflash: rebooting to restore USB, then resuming\n");
                watchdog_hw->scratch[SNES_RESUME_SCRATCH] = SNES_RESUME_MAGIC;
                watchdog_enable(1, 1);
                while (true) tight_loop_contents();
            }

            rom_addr     = (uintptr_t)snes_romflash_image();
            rom_in_flash = true;
        }
        resumedFromFlashWrite = false;

        ErrorMessage[0] = 0;

        /* Allocate snes9x's hot working set AFTER the menu has finished —
         * the menu itself needs ~tens of KB of SRAM for its screen buffer
         * and dialog state; trying to keep both alive at once OOMs.
         * Allocate APU (single 64 KB block) FIRST while the libc heap is
         * least fragmented; smaller allocations from S9xInitMemory then
         * fill the gaps around it. */
        snes9x_setup_settings();
        if (!S9xInitAPU())        { strcpy(ErrorMessage, "APU init failed");    selectedRom[0] = 0; continue; }
        Frens::dumpHeapStats("after-APU");
        if (!S9xInitMemory())     { strcpy(ErrorMessage, "Memory init failed"); S9xDeinitAPU(); selectedRom[0] = 0; continue; }
        Frens::dumpHeapStats("after-Memory");
        if (!S9xInitSound(0, 0))  { strcpy(ErrorMessage, "Sound init failed");  S9xDeinitAPU(); S9xDeinitMemory(); selectedRom[0] = 0; continue; }
        /* S9xInitSound leaves so.playback_rate = 0; without this the
         * mixer's channel freq table stays uninitialized and S9xMixSamples
         * returns silence. This is why audio was completely missing — the
         * call has to happen after S9xInitSound and before S9xReset (which
         * S9xInitMemory eventually triggers via LoadROM). */
        S9xSetPlaybackRate(SNES_AUDIO_HZ);
        Frens::dumpHeapStats("after-Sound");

        if (!snes9x_load_rom(rom_addr, romsize, rom_in_flash)) {
            S9xDeinitSound();
            S9xDeinitAPU();
            S9xDeinitMemory();
            selectedRom[0] = 0;
            continue;
        }
        Frens::dumpHeapStats("after-LoadROM");

        /* Restore battery SRAM from SD before the session starts. LoadROM's
         * S9xReset does not clear Memory.SRAM, so the loaded save survives. */
        snes_load_sram();

#if ENABLE_MSU1
        /* Detect <rom>.msu / <rom>-1.pcm next to the ROM. No pack means no
         * allocation, no SD traffic and no behaviour change at all. */
        msu1_init();
        Frens::dumpHeapStats("after-MSU1");
#endif

        run_emulator();

        /* Flush battery SRAM back to SD before S9xDeinitMemory frees it. */
        snes_save_sram();

#if ENABLE_MSU1
        msu1_deinit();   /* closes the .msu/.pcm handles, frees the PSRAM */
#endif

        /* Return to menu: tear down all snes9x state so the menu has room. */
        S9xDeinitGFX();
        S9xDeinitDisplay();
        S9xDeinitSound();
        S9xDeinitAPU();
        S9xDeinitMemory();
#if ENABLE_SDD1
        sdd1_dma_free();   /* no-op unless an S-DD1 cart was loaded */
#endif
        Frens::dumpHeapStats("after-deinit");

        selectedRom[0] = 0;
    }
    return 0;
}
