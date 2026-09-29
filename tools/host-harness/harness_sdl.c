/* Live SDL2 front end for the host harness (LIVE=1, see README.md).
 *
 * Shows each frame in a window, plays the mixed audio and reads the keyboard
 * as a SNES pad, so a game can be watched, heard and played on the desktop.
 * harness.c stays in charge of the emulation; this file only moves pixels,
 * samples and key state, and paces the loop to real time.
 *
 * Pacing follows the audio clock: after each frame the loop waits until no
 * more than three frames of audio are queued, so emulation runs exactly as
 * fast as the output consumes samples (735 per frame -> 60 fps, 882 -> 50).
 * The wait never exceeds one frame period, so a stalled output cannot slow
 * the game down. Without an audio device a timer on the frame period does
 * the same job. */
#include <stdio.h>
#include <string.h>
#include <SDL.h>

#include "harness_sdl.h"

#define CANVAS_W 320
#define CANVAS_H 240
#define TEX_W    512            /* widest frame: fb0 hi-res mode 5/6 */
#define TEX_H    240

static SDL_Window       *win;
static SDL_Renderer     *ren;
static SDL_Texture      *tex;
static SDL_AudioDeviceID adev;
static SDL_Rect          src_rect, dst_rect;
static char              title_base[128];

static uint32_t frame_period_us;
static uint64_t next_tick;
static uint32_t chunk_bytes;    /* size of the last queued frame of audio */
static int      paused;
static int      fast_forward;
static int      title_dirty = 1;
static uint32_t last_frame = UINT32_MAX;
static uint32_t fps_t0, fps_frames;
static double   fps;

int hsdl_init(const char *title, int scale, int audio_on, uint32_t frame_us)
{
    frame_period_us = frame_us ? frame_us : 16667;
    snprintf(title_base, sizeof title_base, "%s", title);

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "LIVE: SDL video init failed: %s\n", SDL_GetError());
        return 0;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");   /* nearest neighbour */
    if (scale < 1) scale = 1;
    win = SDL_CreateWindow(title_base, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           CANVAS_W * scale, CANVAS_H * scale, SDL_WINDOW_RESIZABLE);
    if (!win) {
        fprintf(stderr, "LIVE: cannot open a window: %s\n", SDL_GetError());
        return 0;
    }
    /* No PRESENTVSYNC: the audio clock paces the loop, a display vsync would
     * fight it. */
    ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) {
        fprintf(stderr, "LIVE: cannot create a renderer: %s\n", SDL_GetError());
        return 0;
    }
    SDL_RenderSetLogicalSize(ren, CANVAS_W, CANVAS_H);
    SDL_RenderSetIntegerScale(ren, SDL_TRUE);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB555, SDL_TEXTUREACCESS_STREAMING,
                            TEX_W, TEX_H);
    if (!tex) {
        fprintf(stderr, "LIVE: cannot create a texture: %s\n", SDL_GetError());
        return 0;
    }

    if (audio_on) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            fprintf(stderr, "LIVE: no audio (%s), pacing on a timer\n", SDL_GetError());
        } else {
            SDL_AudioSpec want, have;
            memset(&want, 0, sizeof want);
            want.freq     = 44100;
            want.format   = AUDIO_S16SYS;
            want.channels = 2;
            want.samples  = 512;
            adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
            if (!adev)
                fprintf(stderr, "LIVE: cannot open audio output (%s), pacing on a timer\n",
                        SDL_GetError());
            else
                SDL_PauseAudioDevice(adev, 0);
        }
    }

    printf("LIVE: video %s, audio %s, window %dx%d\n",
           SDL_GetCurrentVideoDriver(),
           adev ? SDL_GetCurrentAudioDriver() : "off",
           CANVAS_W * scale, CANVAS_H * scale);
    printf("LIVE: pad  arrows=D-pad  Z=A  X=B  C=X  V=Y  Q=L  W=R  S=Start  A=Select\n"
           "LIVE: keys Space=pause  N=step  Tab=fast-forward (hold)  F12=dump frame  "
           "F5=reset  Esc=quit\n");
    fflush(stdout);
    return 1;
}

void hsdl_quit(void)
{
    if (adev) SDL_CloseAudioDevice(adev);
    if (tex)  SDL_DestroyTexture(tex);
    if (ren)  SDL_DestroyRenderer(ren);
    if (win)  SDL_DestroyWindow(win);
    adev = 0; tex = NULL; ren = NULL; win = NULL;
    SDL_Quit();
}

void hsdl_redraw(void)
{
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    if (src_rect.w)
        SDL_RenderCopy(ren, tex, &src_rect, &dst_rect);
    SDL_RenderPresent(ren);
}

void hsdl_present(const uint16_t *px, int w, int h, int pitch_px)
{
    if (w > TEX_W) w = TEX_W;
    if (h > TEX_H) h = TEX_H;
    src_rect = (SDL_Rect){ 0, 0, w, h };
    SDL_UpdateTexture(tex, &src_rect, px, pitch_px * (int)sizeof(uint16_t));
    const int dw = w > CANVAS_W ? w / 2 : w;
    dst_rect = (SDL_Rect){ (CANVAS_W - dw) / 2, (CANVAS_H - h) / 2, dw, h };
    hsdl_redraw();
}

void hsdl_audio(const int16_t *buf, int frames)
{
    if (!adev || paused || fast_forward) return;
    chunk_bytes = (uint32_t)frames * 2 * sizeof(int16_t);
    if (SDL_GetQueuedAudioSize(adev) > 8 * chunk_bytes)
        return;                 /* output stalled: drop, see hsdl_pace() */
    SDL_QueueAudio(adev, buf, chunk_bytes);
}

/* Keyboard -> SNES pad. Same keys as a USB keyboard on the device
 * (pico_shared/hid_app.cpp); bits as SNES_*_MASK in snes9x.h. */
static uint32_t keyboard_pad(void)
{
    static const struct { SDL_Scancode key; uint32_t bit; } map[] = {
        { SDL_SCANCODE_X,     1u << 15 },   /* B */
        { SDL_SCANCODE_V,     1u << 14 },   /* Y */
        { SDL_SCANCODE_A,     1u << 13 },   /* Select */
        { SDL_SCANCODE_S,     1u << 12 },   /* Start */
        { SDL_SCANCODE_UP,    1u << 11 },
        { SDL_SCANCODE_DOWN,  1u << 10 },
        { SDL_SCANCODE_LEFT,  1u <<  9 },
        { SDL_SCANCODE_RIGHT, 1u <<  8 },
        { SDL_SCANCODE_Z,     1u <<  7 },   /* A */
        { SDL_SCANCODE_C,     1u <<  6 },   /* X */
        { SDL_SCANCODE_Q,     1u <<  5 },   /* L */
        { SDL_SCANCODE_W,     1u <<  4 },   /* R */
    };
    const Uint8 *ks = SDL_GetKeyboardState(NULL);
    uint32_t pad = 0;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
        if (ks[map[i].key]) pad |= map[i].bit;
    return pad;
}

uint32_t hsdl_poll(int *events)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            *events |= HSDL_EV_QUIT;
        } else if (e.type == SDL_KEYDOWN) {
            const int repeat = e.key.repeat;
            switch (e.key.keysym.scancode) {
            case SDL_SCANCODE_ESCAPE:
                *events |= HSDL_EV_QUIT;
                break;
            case SDL_SCANCODE_SPACE:
                if (repeat) break;
                paused = !paused;
                title_dirty = 1;
                if (paused && adev) SDL_ClearQueuedAudio(adev);
                next_tick = 0;
                break;
            case SDL_SCANCODE_N:            /* held: keeps stepping */
                if (paused) *events |= HSDL_EV_STEP;
                break;
            case SDL_SCANCODE_F12:
                if (!repeat) *events |= HSDL_EV_DUMP;
                break;
            case SDL_SCANCODE_F5:
                if (!repeat) *events |= HSDL_EV_RESET;
                break;
            default:
                break;
            }
        } else if (e.type == SDL_WINDOWEVENT) {
            if (e.window.event == SDL_WINDOWEVENT_EXPOSED ||
                e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
                hsdl_redraw();
        }
    }
    const int ff = SDL_GetKeyboardState(NULL)[SDL_SCANCODE_TAB] != 0;
    if (ff != fast_forward) {
        fast_forward = ff;
        title_dirty = 1;
        next_tick = 0;
        if (ff && adev) SDL_ClearQueuedAudio(adev);
    }
    return keyboard_pad();
}

int hsdl_paused(void)
{
    return paused;
}

void hsdl_idle(void)
{
    SDL_Delay(15);
}

void hsdl_pace(void)
{
    if (paused || fast_forward) return;
    const uint64_t freq   = SDL_GetPerformanceFrequency();
    const uint64_t period = freq * frame_period_us / 1000000u;
    uint64_t now = SDL_GetPerformanceCounter();
    if (!next_tick || now > next_tick + 4 * period)
        next_tick = now;                        /* start, or resync after a stall */
    next_tick += period;
    if (adev) {
        /* Keep ~3 frames (~50 ms) queued. A healthy output drains one frame
         * of audio per frame period, and each frame that paces on it
         * re-anchors the schedule, so the audio clock rules. The WSLg
         * PulseAudio sink sometimes stalls for seconds: then the wait gives
         * up one period past the schedule, which keeps the game at 60 fps
         * (hsdl_audio stops queueing at 8 frames, so latency stays bounded
         * and the sound comes back within a few frames once it resumes). */
        const uint32_t limit    = 3 * chunk_bytes;
        const uint64_t deadline = next_tick + period;
        while (SDL_GetQueuedAudioSize(adev) > limit) {
            if ((now = SDL_GetPerformanceCounter()) >= deadline) break;
            SDL_Delay(1);
        }
        if (now < deadline)
            next_tick = now;
        return;
    }
    while ((now = SDL_GetPerformanceCounter()) < next_tick) {
        const uint64_t ms = (next_tick - now) * 1000u / freq;
        SDL_Delay(ms > 1 ? (uint32_t)(ms - 1) : 0);
    }
}

void hsdl_status(uint32_t frame)
{
    const uint32_t t = SDL_GetTicks();
    if (frame != last_frame) {
        if (paused) title_dirty = 1;            /* show every single step */
        last_frame = frame;
        fps_frames++;
    }
    if (!fps_t0) fps_t0 = t;
    if (t - fps_t0 >= 1000) {
        fps = fps_frames * 1000.0 / (double)(t - fps_t0);
        fps_frames = 0;
        fps_t0 = t;
        title_dirty = 1;
    }
    if (!title_dirty) return;
    title_dirty = 0;
    char buf[256];
    snprintf(buf, sizeof buf, "%s | frame %u | %.1f fps%s", title_base, frame, fps,
             paused ? " | PAUSED (N = step)" : fast_forward ? " | FAST-FORWARD" : "");
    SDL_SetWindowTitle(win, buf);
}
