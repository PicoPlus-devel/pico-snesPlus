/* Live SDL2 front end for the host harness (LIVE=1, see README.md).
 *
 * Deliberately free of snes9x headers: harness_sdl.c only sees plain
 * pixels, samples and a SNES pad mask, so SDL's and the core's macros and
 * typedefs never meet in one translation unit. */
#ifndef HARNESS_SDL_H
#define HARNESS_SDL_H

#include <stdint.h>

/* Event bits returned by hsdl_poll(). */
#define HSDL_EV_QUIT  (1 << 0)   /* Esc, window close or Ctrl-C */
#define HSDL_EV_DUMP  (1 << 1)   /* F12: dump the frame on screen */
#define HSDL_EV_RESET (1 << 2)   /* F5: soft reset */
#define HSDL_EV_STEP  (1 << 3)   /* N while paused: run one frame */

/* Opens the window (320x240 * scale) and, when audio_on, a 44.1 kHz s16
 * stereo output. frame_us is the frame period used for pacing when there is
 * no audio device. Returns 0 if the window cannot be created. */
int      hsdl_init(const char *title, int scale, int audio_on, uint32_t frame_us);
void     hsdl_quit(void);

/* RGB555 pixels (R at bit 10), w <= 512, h <= 240. Frames up to 320x240 are
 * centred in a 320x240 canvas; 512-wide hi-res frames are squeezed to 256. */
void     hsdl_present(const uint16_t *px, int w, int h, int pitch_px);
void     hsdl_redraw(void);

/* Queue one video frame of mixed audio (interleaved stereo). Dropped while
 * muted, paused or fast-forwarding, and while the output is stalled. */
void     hsdl_audio(const int16_t *buf, int frames);

/* Pump SDL events. Returns the keyboard's SNES pad mask (snes9x.h bit
 * order) and ORs HSDL_EV_* bits into *events. */
uint32_t hsdl_poll(int *events);
int      hsdl_paused(void);
void     hsdl_idle(void);          /* short sleep for the pause loop */

/* Wait until the next frame is due: on the audio queue when there is an
 * audio device, else on a timer. Returns at once while fast-forwarding. */
void     hsdl_pace(void);

/* Refresh the window title (ROM, frame, measured fps, state) about once a
 * second, or at once when pause or fast-forward changes. */
void     hsdl_status(uint32_t frame);

#endif
