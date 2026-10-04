# Host-side render test harness

Runs the vendored snes9x core from this repository natively on Linux and
dumps rendered frames as PPM images. Render bugs can be reproduced,
bisected and fixed on a desktop machine in seconds — no board, flashing or
capture hardware needed. It was built to find the DKC "Nintendo presents"
mode-5 strip-seam bug and is kept for future rendering work. In live mode
(`LIVE=1`, see [Live mode](#live-mode-live1)) it shows the frames in a window,
plays the sound and takes keyboard input, so a game can also be checked by
eye and ear, and a session played by hand can be replayed exactly for
dumps and comparisons.

The harness boots the core through the exact same sequence `main.cpp` uses
on the RP2350 (same `Settings`, same init order, same `LoadROM(NULL)`
hand-off) and, in the `RENDER_TO_FB=1` variant, mimics the strip renderer
from `port_glue.cpp` faithfully: 16-row staging strips, `repoint`/`copyout`
per chunk, centered window in a 320x240 framebuffer. Keep that mimic in
sync when `port_glue.cpp` changes.

## Build

```bash
tools/host-harness/build.sh
```

Needs a native `gcc`. SDL2 is optional: when its development package is
installed, every binary is built with live mode; without it the binaries are
built exactly as before, and `LIVE=1` exits with a message saying so. The
first line `build.sh` prints tells which of the two it did.

```bash
sudo apt install libsdl2-dev        # Debian/Ubuntu: only needed for live mode
```

Produces six binaries in this directory:

| binary      | meaning                                                        |
| ----------- | -------------------------------------------------------------- |
| `fb1_nolut` | strip renderer + device color math — **the device render flow** |
| `fb0_nolut` | classic full-frame render, device color math                   |
| `fb0_lut`   | classic full-frame render, upstream ZERO-LUT color math        |
| `msu1`      | device render flow + MSU-1 (`ENABLE_MSU1=1`), see below         |
| `spc7110`   | device render flow + SPC7110 (`ENABLE_SPC7110=1`), see below    |
| `sdd1`      | device render flow + S-DD1 (`ENABLE_SDD1=1`), see below         |

Byte-comparing the PPM output between the first three isolates a bug's
layer: `fb1` vs `fb0` differs → strip renderer; `fb0_nolut` vs `fb0_lut`
differs → the LUT-free color math (`NO_ZERO_LUT`).

## Run

```bash
./fb1_nolut <rom.sfc> <outdir> <tag> <maxframe> [dumpstep] [dumpfrom]

# examples
mkdir -p out
./fb1_nolut dkc.sfc out dkc 1200 20      # frames 0-1200, dump every 20th
./fb1_nolut dkc.sfc out dkc 600 1 550    # dump every frame from 550 to 600
```

Frames are written as `<outdir>/<tag>_f00560.ppm` (RGB888 P6). By default no
input is fed to the joypads, so attract sequences and intros play by
themselves.

`PAD_AUTO=<n>` taps port 1 every `n` frames, holding for `PAD_HOLD` frames
(default 8), starting at `PAD_FROM` (default 240 — nothing is pressed before
that) with the buttons in `PAD_MASK` (default `0x1000`, Start alone; bit order
bit15..bit4 is B Y Sel Sta Up Dn Lf Rt A X L R).

Two defaults there are deliberate. Holding several buttons at power-on is how
Hudson's SPC7110 carts enter their built-in **SPC7110 CHECK PROGRAM**, so a
mask like `0x9080` (B+Start+A) from frame 0 puts every one of them into the
diagnostic instead of the game. And `PAD_FROM` keeps the first frames
untouched for the same reason.

`PAD_UNTIL=<frame>` stops the automated presses again. This matters for any
multi-stage run: `PAD_AUTO` would otherwise still be holding a button when
`RESET_AT` fires, and the SPC7110 check program reads a button held at reset
as a request to start its first stage over, so the run never advances.

`RESET_AT=<frame>` issues a soft reset (`S9xReset`), the way you would press
the console's reset button. `Memory.SRAM` survives it.

`FRAME_US=<path>` writes the host time of every `S9xMainLoop` call to that
file, one `frame microseconds` line each. The absolute numbers are the
desktop's, but a frame that costs several times its neighbours here deserves a
look on the device. It is how the pause at "FIGHT!" in Street Fighter Alpha 2
was shown not to be emulation cost: no frame of the round intro costs more
than three times a normal fight frame, and `AUDIODBG` shows the game uploading
~47 KB of sound data to the SPC700 while it waits.

`TRACE_FROM=<frame>` in the environment logs, from that frame on, every
strip-chunk row range (`fb1` only) and the PPU state per frame (BGMode,
$2130-$2133, TM/TS, screen height) to stderr — this is how a mid-frame
split or a screen-mode surprise shows up.

`MOUSE=1` attaches a scripted SNES Mouse (port 1, the port Mario Paint
requires): the cursor circles the screen center so motion never saturates
at an edge, and `MOUSE_CLICK=<frame>` holds the left button for 10 frames
from that frame on. This drives the same core mouse path that
`port_glue.cpp` feeds from a USB HID mouse on device. Quick check:
Mario Paint's title cursor follows the circle, and a click timed over a
title letter triggers its easter-egg animation.

```bash
MOUSE=1 MOUSE_CLICK=471 ./fb1_nolut mariopaint.sfc out mp 700 50 450
```

## Live mode (`LIVE=1`)

With `LIVE=1` the harness opens a window that shows every frame as it is
rendered, plays the mixed audio through the desktop's sound output, and
reads the keyboard as the port-1 pad. The emulation is paced to real time:
60 frames per second, 50 for a PAL game. Live mode is meant for checks that
are easier to make by eye and ear than from PPM dumps and raw audio files:
whether a scene looks right, whether music plays at the right tempo and
without glitches, and whether a game reacts correctly to input.

Live mode is **not a preview of device speed**. The desktop runs the core at
full speed and renders every frame, whereas on the Fruit Jam many games need
frame skip and some still run below 60 fps. Neither frame skip nor device
slowdown is modelled; `FPS=<n>` on the `msu1` binary is the only slowdown
model (see [below](#using-live-mode-with-the-other-options)). Live mode shows
what the core draws and plays, not how fast the board runs it.

Without `LIVE=1` nothing changes: headless runs produce the same PPMs and
audio, byte for byte, as a build without SDL2.

### Starting

```bash
cd tools/host-harness
LIVE=1 ./fb1_nolut smw.sfc
```

Only the ROM is required. The other arguments keep their headless meaning
and are optional:

```bash
LIVE=1 ./fb1_nolut <rom.sfc> [outdir] [tag] [maxframe] [dumpstep] [dumpfrom]
```

| argument   | live default | meaning                                              |
| ---------- | ------------ | ---------------------------------------------------- |
| `outdir`   | `.`          | directory for F12 dumps and periodic dumps           |
| `tag`      | `live`       | file name prefix: `<outdir>/<tag>_f00192.ppm`        |
| `maxframe` | unlimited    | quit automatically after this frame                  |
| `dumpstep` | none         | also dump every `dumpstep`-th frame, as headless     |
| `dumpfrom` | `0`          | first frame of the periodic dumps                    |

For example, `LIVE=1 ./fb1_nolut smw.sfc out smw 3599 600` plays one minute,
writes a dump into `out/` every ten seconds, and then quits. As in headless
runs, the output directory must already exist.

Every binary has live mode. Pick the one that matches the cart:

| cart                                                            | binary                  |
| --------------------------------------------------------------- | ----------------------- |
| all others, including SuperFX, SA-1, DSP-1, C4, OBC1 and S-RTC  | `fb1_nolut`             |
| S-DD1: Street Fighter Alpha 2, Star Ocean                        | `sdd1`                  |
| SPC7110: Tengai Makyou Zero, Momotarou Dentetsu Happy, Super Power League 4 | `spc7110`  |
| MSU-1 packs                                                     | `msu1` with `MSU=0`     |
| comparing the render paths                                      | `fb0_nolut`, `fb0_lut`  |

Only `sdd1` and `spc7110` are built with those chips, and in the other
binaries these carts do not work: Street Fighter Alpha 2 runs with garbage
graphics, and Star Ocean and Tengai Makyou Zero show a black screen.

### Controls

Keys reach the harness only while its window has keyboard focus; click the
window if key presses have no effect. The pad keys are the ones a USB
keyboard uses on the device (`pico_shared/hid_app.cpp`):

| key        | SNES button |
| ---------- | ----------- |
| arrow keys | D-pad       |
| Z          | A           |
| X          | B           |
| C          | X           |
| V          | Y           |
| Q          | L           |
| W          | R           |
| S          | Start       |
| A          | Select      |

The letter keys are matched by position, as on the device, so on a
non-QWERTY layout they are the keys in the same places.

| key        | action |
| ---------- | ------ |
| Space      | Pause or resume. While paused the sound stops and the window title shows `PAUSED`. |
| N          | While paused: run exactly one frame. Held down, it keeps stepping at the keyboard repeat rate. |
| Tab (hold) | Fast-forward: run as fast as the host allows, typically several hundred frames per second. The sound output is silent while Tab is held, but the game's sound is still emulated and mixed. |
| F12        | Write the frame on screen to `<outdir>/<tag>_f<frame>.ppm` and print its path. Works while paused. |
| F5         | Soft reset (`S9xReset`, the same as `RESET_AT`) at the start of the next frame. While paused it takes effect on the next step or on resume. |
| Esc        | Quit. Closing the window, or Ctrl-C in the terminal, does the same. |

Quitting always runs the normal end of the run: the chip tallies (`SDD1:`,
`SPC7110:`, `MSU1:`) are printed and `SRAM_OUT` is written.

The keyboard pad is combined with `PAD_AUTO` (the two masks are ORed), so
scripted taps and your own presses can be used together. During a
`PAD_PLAY` replay the recording supplies the pad instead, until it ends
(see [Recording and replaying input](#recording-and-replaying-input-pad_rec-pad_play)).

### Window, sound and pacing

- `SCALE=<n>` sets the initial window size to 320x240 times `n` (default 3,
  960x720). The window can be resized; the picture is scaled by whole
  multiples with nearest-neighbour filtering and centred.
- The strip-renderer binaries (`fb1_nolut`, `msu1`, `spc7110`, `sdd1`) show
  the full 320x240 framebuffer the device sends to HDMI, with the SNES
  picture centred in it. The `fb0` binaries show the native SNES picture
  centred in the same area; hi-res frames (512 wide: modes 5 and 6, and
  interlace) are squeezed to 256 columns.
- `MUTE=1` runs without sound output.
- Pacing follows the sound output: after each frame the harness waits until
  no more than three frames of audio (about 50 ms) are queued, so the
  emulation runs exactly as fast as the sound device consumes samples. With
  `MUTE=1`, or when no sound device can be opened, a timer on the frame
  period (16.667 ms, or 20 ms for PAL) is used instead.
- The window title shows the ROM name, the binary, the current frame number
  and the measured frame rate, for example
  `THE LEGEND OF ZELDA (fb1_nolut) | frame 4210 | 60.0 fps`. The frame
  number is the one the headless options expect: `dumpfrom`, `TRACE_FROM`,
  `DSPLOG`, `APUDUMP`, `SDD1_TRACE` and `RESET_AT`. The rate is updated once
  per second.
- `AUDIO_OUT=<path>` also works in live mode. It records everything that was
  mixed, including the frames run during fast-forward, so it is the complete
  sound of the session rather than what was heard.

At start-up the terminal shows the drivers SDL chose and a summary of the
keys:

```
LIVE: video x11, audio pulseaudio, window 960x720
LIVE: pad  arrows=D-pad  Z=A  X=B  C=X  V=Y  Q=L  W=R  S=Start  A=Select
LIVE: keys Space=pause  N=step  Tab=fast-forward (hold)  F12=dump frame  F5=reset  Esc=quit
```

### Using live mode with the other options

All environment options of the headless harness still apply. Some
combinations that are useful while playing:

**Battery saves across sessions.** `SRAM` loads a save at start and
`SRAM_OUT` writes it back when you quit:

```bash
LIVE=1 SRAM=zelda.srm SRAM_OUT=zelda.srm ./fb1_nolut zelda.sfc
```

The first time, when `zelda.srm` does not exist yet, the harness reports
that it cannot open it and starts with a blank battery.

If you also record the session (`PAD_REC`), keep a copy of the save as it
was before the session: a replay has to start from the same save the
recording started from, and `SRAM_OUT` overwrites it on quit.

**Sound tracing while playing.** The `AUDIODBG` reports go to the terminal
while the game runs in the window:

```bash
LIVE=1 AUDIODBG=60 ./spc7110 "Tengai Makyou Zero (English v7.0).sfc"
```

**Host time per frame.** `FRAME_US` measures only the emulation of each
frame, not the wait for real time, so its figures mean the same as in a
headless run:

```bash
LIVE=1 FRAME_US=/tmp/frame_us.txt ./sdd1 "Street Fighter Alpha 2 (USA).sfc"
sort -k2 -n /tmp/frame_us.txt | tail       # the most expensive frames
```

**MSU-1.** With `MSU=0` the game's own driver plays the pack. `FPS=<n>`
additionally runs the window at that frame rate, which models a device that
cannot keep up: the MSU-1 track keeps its tempo, because it is streamed in
real time as on the device, while the SNES's own sound slows down with the
game.

```bash
LIVE=1 MSU=0 ./msu1 alttp_msu.sfc
LIVE=1 MSU=0 FPS=40 ./msu1 alttp_msu.sfc    # runs at 40 fps
```

**Mouse.** `MOUSE=1` still attaches the scripted SNES Mouse, and its cursor
circles on screen in live mode as well. The host mouse is not used.

**Resets.** `RESET_AT=<frame>` works as in headless runs, in addition to F5.

### Recording and replaying input (`PAD_REC`, `PAD_PLAY`)

A session played by hand cannot be repeated frame-exactly by hand, so a
glitch seen while playing is hard to pin down with dumps and A/B
comparisons. `PAD_REC=<file>` records all input the machine receives during
a run; `PAD_PLAY=<file>` feeds it back, in a headless run or in another live
session. The emulation is deterministic, so the replay reproduces the
recorded session exactly: every frame and every audio sample.

Both options also work without `LIVE=1`. `PAD_REC` on a headless `PAD_AUTO`
run, for example, turns the automated taps into a file that can then be
edited.

#### File format

Plain text, one line per change:

```
# pico_snesPlus host-harness input recording
# binary: sdd1
# rom: /home/frank/roms/SNES/Street Fighter Alpha 2 (USA).sfc
# <frame> <hexmask> | <frame> reset [wram] | <frame> end
300 reset
600 1080
608 0000
1200 end
```

| line                 | meaning |
| -------------------- | ------- |
| `<frame> <hexmask>`  | The pad state from this frame on, as a hexadecimal mask in the `PAD_MASK` bit order (bit 15 to 4: B Y Select Start Up Down Left Right A X L R). `1080` is Start + A. |
| `<frame> reset`      | Soft reset at the start of this frame (F5 or `RESET_AT`). |
| `<frame> reset wram` | The same, with WRAM cleared (`RESET_AT` together with `RESET_WRAM`). |
| `<frame> end`        | The recording stopped before this frame. |

Lines starting with `#` are comments, and frame numbers must not decrease.
The header names the binary and the ROM, and lists those environment
settings that change the machine (`SRAM`, `MOUSE`, `MOUSE_CLICK`, `MSU`,
`MSU_VOL`, `MSU_REPEAT`, `FPS`, `RTC_SPEED`), so the replay command can be
rebuilt from the file. The recorded pad is the combined one (keyboard and
`PAD_AUTO` together), so a replay needs neither.

#### Rules for an exact replay

- **The same chips.** Replay with the same binary, or at least with one
  built with the same chips. The render path does not influence the game, so
  a recording made with `fb1_nolut` replays exactly in `fb0_nolut` and
  `fb0_lut`, which is how a renderer A/B is done. A binary with another chip
  set is another machine. The harness prints a note when the `# binary:`
  line differs from the binary being run, and runs anyway.
- **The same machine settings.** The same ROM, a save with the same content
  (not one that `SRAM_OUT` has overwritten since), and the same `MOUSE`,
  `MSU`, `FPS` and `RTC_SPEED` values as listed in the header.
- **The same core, unless that is the point.** After a change to the core a
  replay shows what the change does with the same input, but from the first
  frame where the change makes a difference the game can take another course.
- `PAD_AUTO` and `RESET_AT` are ignored during a replay, up to the
  recording's `end` line; the recording already contains their effect.

Recording and replay also run the sound mixer for every frame, whether or
not sound is output or written. The mixer is part of the emulated machine:
the SNES sound driver reads back the voice envelope (ENVX), voice output
(OUTX) and end-of-sample (ENDX) registers that the mixer updates, so a
game's course can depend on whether mixing happens. A plain headless run
(without `AUDIODBG`, `LIVE`, `PAD_REC` or `PAD_PLAY`) does not mix, as
before. A recording replayed with `PAD_PLAY` therefore matches the session
it was recorded in, while a plain headless run with the same `PAD_AUTO`
settings is not guaranteed to.

#### From a glitch on screen to PPM dumps

1. Play with recording switched on:

   ```bash
   cd tools/host-harness
   LIVE=1 PAD_REC=/tmp/smw.pad ./fb1_nolut smw.sfc
   ```

2. When the glitch appears, press Space to pause, step to the exact frame
   with N, and press F12. The terminal prints the frame number and the dump,
   for example `LIVE: frame 2417 dumped to ./live_f02417.ppm`. Press Esc to
   quit; the recording is closed with an `end` line.

3. Replay headless and dump every frame around it:

   ```bash
   mkdir -p out
   PAD_PLAY=/tmp/smw.pad ./fb1_nolut smw.sfc out a 2430 1 2400
   cmp live_f02417.ppm out/a_f02417.ppm      # silent: the replay is the session
   ```

4. The headless tools now apply to exactly that moment, as often as
   needed: `TRACE_FROM=2410` for the PPU state per frame, `AUDIODBG` and
   `DSPLOG` for the sound, `SDD1_TRACE` on an S-DD1 cart. Comparisons use
   the same file:

   ```bash
   # strip renderer against the classic full-frame path, same input
   PAD_PLAY=/tmp/smw.pad ./fb0_nolut smw.sfc out b 2430 1 2400

   # before and after a core change
   mkdir -p /tmp/before out/before out/after && cp fb1_nolut /tmp/before/
   #   ... change the core, then rebuild:
   ./build.sh
   PAD_PLAY=/tmp/smw.pad /tmp/before/fb1_nolut smw.sfc out/before x 2430 1 2400
   PAD_PLAY=/tmp/smw.pad ./fb1_nolut           smw.sfc out/after  x 2430 1 2400
   diff -rq out/before out/after             # silent: the change is invisible here
   ```

   `fb1` and `fb0` dumps differ in geometry, as described under
   [Inspecting output](#inspecting-output), so compare those two by eye or
   crop them first.

#### Continuing from a recording

A live replay gives control to the keyboard at the recording's `end` line,
and the terminal prints
`PAD_PLAY: recording ended at frame <n>, the keyboard has control`. This is
a way back to a point deep in a game without a save state: replay the
session that got there, holding Tab to get there quickly if you like, and
play on from where it stopped. To keep the continuation, record it to a new
file. The new file contains the replayed part as well, so it replays from
power-on to the new end:

```bash
LIVE=1 PAD_PLAY=/tmp/smw.pad PAD_REC=/tmp/smw2.pad ./fb1_nolut smw.sfc
```

In a headless replay the pad is simply released after the `end` line, and
`PAD_AUTO` and `RESET_AT`, if set, take over from that frame.

#### Writing input by hand

A file can also be written by hand, as a more precise alternative to
`PAD_AUTO`. Without an `end` line the last pad state lasts until the run
ends:

```
# press Start on the title screen, then hold Right for two seconds
300 1000
308 0000
400 0100
520 0000
```

### Troubleshooting

- **No window appears.** Live mode needs a display: `DISPLAY` or
  `WAYLAND_DISPLAY` must be set. Under WSL2 it is provided by WSLg. SDL's
  choice can be forced with `SDL_VIDEODRIVER=x11` or `SDL_VIDEODRIVER=wayland`;
  the first `LIVE:` line names the driver in use.
- **No sound.** The first `LIVE:` line shows `audio off` when no sound device
  could be opened; the harness then paces on a timer. Under WSLg the output
  is PulseAudio, and `SDL_AUDIODRIVER=pulseaudio` forces it if SDL picks
  another backend. `MUTE=1` takes the sound path out entirely.
- **Sound drops out for a few seconds while the picture carries on.** The
  WSLg PulseAudio output occasionally stalls. The harness then keeps the
  game at full speed on its own timer, skips the sound it cannot deliver,
  and resumes with normal latency once the output recovers. `AUDIO_OUT` and
  replays are not affected, because they record what was mixed, not what
  was heard.
- **Crackling or stutter.** The host cannot keep up with real time; the frame
  rate in the window title shows it. `build.sh` builds with `-O2`; a build
  with sanitizers or without optimization can be too slow for SuperFX or
  SA-1 games. A short gap when Tab is released is expected, while the sound
  queue fills again.
- **Keys have no effect.** The window does not have keyboard focus; click it.
- **A replay does not match the session.** See the
  [rules for an exact replay](#rules-for-an-exact-replay): another chip set,
  another ROM or save, other machine settings, or a changed core.
- **`built without SDL2, so LIVE=1 is not available`.** SDL2 was not found
  when the harness was built. Install `libsdl2-dev` and run `build.sh` again.

## MSU-1 (`./msu1`)

The `msu1` binary is the device render flow plus MSU-1, with a stdio backend
standing in for the FatFs one in `msu1_port.cpp`. It exercises the real
thing: the `$2000-$2007` traps in `ppu.c`, the PCM ring in `msu1.c`, the
core0 refill (`msu1_pump`) and the core1 mixer (`msu1_mix`), in the same
per-frame order the firmware runs them. Pack files are looked up next to the
ROM, exactly as on device (`<rom minus extension>.msu` / `-<n>.pcm`).

`MSU=0` initialises MSU-1 and lets **the ROM's own driver** work the
registers — the realistic end-to-end test:

```bash
MSU=0 AUDIO_OUT=/tmp/a.raw ./msu1 alttp_msu.sfc out z 1800 1000 9999
aplay -f S16_LE -r 44100 -c 2 /tmp/a.raw          # or: ffplay -f s16le -ar 44100 -ac 2
```

`MSU=<track>` instead has the harness drive it: check the identity string,
dump some data-track bytes through `$2001`, select the track, poll
`AUDIO_BUSY`, then set volume and press play. Use this to test a bare pack
against any ROM. `MSU_VOL=<0-255>` and `MSU_REPEAT=<0|1>` tune it.

```bash
MSU=1 MSU_REPEAT=1 AUDIO_OUT=/tmp/a.raw ./msu1 rom.sfc out t 300 1000 9999
```

`AUDIO_OUT=<path>` dumps the fully mixed stream (SNES DSP + MSU) as raw
44.1 kHz stereo s16le, one video frame at a time. The run ends with the
`MSU1:` health line and a peak comparison of SNES-only vs SNES+MSU, which is
what tells you the PCM actually reached the mix rather than the game just
being noisy on its own.

### Frame-rate modelling (`FPS=<10-60>`)

`msu1_pump` runs once per *video frame* but PCM is consumed in *real time*,
so a game running below 60 fps needs a bigger refill per frame — 4410 B at
40 fps versus 2949 B at 60. Getting that wrong does not show up at 60 fps at
all, and shows up below it as a permanently empty ring and continuous
underruns (this is real: Zelda's intro FMV runs at 40 fps and streams video
through the data port while the music plays).

`FPS=<n>` models it. The harness drives MSU-1 off a **virtual clock** that
advances one frame period per emulated frame, so the core sees exactly the
intervals it would see on hardware even though the harness runs far faster
than real time. Check `ring %` and `urun`:

```bash
MSU=0 FPS=40 ./msu1 alttp_msu.sfc out z 900 1000 9999   # want: ring ~93%, urun 0
```

Because the clock is virtual, the `avg`/`max`/`open` microsecond figures in
the `MSU1:` line are meaningless here — read cost can only be measured on
the device. `ring`, `urun`, `SD`/`rd` and `data` are all real.

A synthetic pack makes the checks exact — a known sine can be compared
sample-for-sample against `(src * volume) >> 8`, which catches ring-wrap and
loop-splice errors that are inaudible in a real soundtrack.

## Audio tracing (`AUDIODBG`, `DSPLOG`, `APUDUMP`)

The harness mixes one video frame of audio per frame at the real-time rate
(735 samples, or 882 for a PAL game at 50 fps), the way `core1_mix_task`
does on the device, so sound faults reproduce here too — which is a great
deal faster than reflashing a board to test a theory. It does so whenever
something consumes the sound: `AUDIODBG`, live mode, or input
recording/replay.
Like the device it mixes in 64-frame chunks (`harness_mix`): `S9xMixSamples`
works in `soundux.c` buffers of `SOUND_BUFFER_SIZE` (1321) slots, and before
2026-09-28 the harness asked for a whole frame (1470 slots) in one call. That
overran into the echo filter taps and envelope rate tables, so earlier harness
audio (saturated echo, wrong ENVX) did not match the device.

`AUDIODBG=<n>` reports every `n` frames: mixed-sample count, the peak
amplitude since the last report, the DSP's keyed/ENDX/FLG registers, how many
key-ons the driver asked for and on which voices, how many times the 65816
wrote an APU port, the SPC700's PC and output ports, the three timer counters,
voice 0's eight DSP registers, and every voice's envelope state and level.

    AUDIODBG=60 SRAM=game.SAV ./spc7110 game.sfc out tag 5400 99999

A silent game with key-ons still being issued is a different fault from one
where the driver has stopped asking; the `KON` column separates them. Timer
counters that free-run instead of sitting at zero mean the driver has stopped
reading them, i.e. it has left its main loop.

`AUDIO_OUT=<path>` alongside `AUDIODBG` (or in a live, `PAD_REC` or
`PAD_PLAY` run) writes that same mixed stream to a file (44.1 kHz, s16,
stereo), for listening:

    AUDIODBG=60 AUDIO_OUT=/tmp/a.raw ./fb1_nolut game.sfc out tag 3600 99999
    aplay -f S16_LE -r 44100 -c 2 /tmp/a.raw

`DSPLOG=<from>,<to>` traces every DSP register write in a frame window, with
voice 0's state alongside, which shows what the driver actually intends for a
voice (ADSR against GAIN, key-off, envelope mode).

`APUDUMP=<frame>` hex-dumps four regions of SPC700 RAM at that frame, for
disassembling a driver that has become stuck in a loop.

Requires `-DAUDIO_WATCHDOG=1`, which `build.sh` passes for the `spc7110`
variant. The same reporting exists on the device behind the `AUDIO_WATCHDOG`
cmake option, which prints on three consecutive silent seconds.

## Inspecting output

ffmpeg handles PPM everywhere:

```bash
# contact sheet to find the interesting frame
ffmpeg -pattern_type glob -i 'out/dkc_*.ppm' -vf tile=8x8 sheet.png

# zoom into a region (crop=w:h:x:y, then scale up with nearest neighbor)
ffmpeg -i out/dkc_f00560.ppm -vf "crop=120:80:190:160,scale=480:320:flags=neighbor" zoom.png
```

`cmp a.ppm b.ppm` byte-compares two dumps; identical files mean identical
rendering, which makes regression checks trivial (render the same frame
range before and after a core change and compare).

Note the geometry when comparing variants: `fb1` dumps the full 320x240
framebuffer with the SNES image centered (default NTSC window starts at
x=32, y=8); `fb0` dumps the native SNES resolution, which is 512 wide when
the frame used hi-res mode 5/6 or interlace (even pixels correspond to the
force-lores output).

## SPC7110 (`spc7110`)

Built with `ENABLE_SPC7110=1 SPC7110_STATS=1`, and deliberately the only
variant that is — the other four must keep producing byte-identical PPMs for
every cart, which is the regression check that the `ppu.c` / `dma.c` /
`getset.c` hooks stay inert when the chip is absent:

```bash
tools/host-harness/build.sh
mkdir -p out
for r in zelda dkc smw; do
    ./fb1_nolut $r.sfc out/a a 150 150
    ./spc7110   $r.sfc out/b a 150 150
    diff -rq out/a out/b      # must be silent
done
```

On an SPC7110 cart the run ends with a tally of what the chip actually did:

```bash
PAD_AUTO=40 ./spc7110 "Tengai Makyou Zero (English v7.0).sfc" out tmz 600 100
```

```
SPC7110: mmio r/w = 197021/607  window($d0-$ff) = 0  FIFO bytes = 65537
SPC7110: decomp_init = 1 (mode0 0, mode1 0, mode2 1)  max seek index = 0
SPC7110: register reads:  4800:65537 4809:65537 ...
SPC7110: register writes: 4811:58 4820:6 4830:25 4840:26 4841:50 ...
```

`max seek index` is the one to watch for performance: `decomp_init` ends with
`while (index--) decomp_read()`, so a single write to `$4806` can decode up to
262140 bytes inside one emulated CPU store. `FIFO bytes` divided by the frame
count is the steady-state decompression load the RP2350 has to absorb.

## S-DD1 (`sdd1`)

Built with `ENABLE_SDD1=1 SDD1_STATS=1`, and like `spc7110` the only variant
with its chip, so the others stay a byte-identical regression check for the
`dma.c` / `ppu.c` / `cpu.c` hooks. Without the chip Street Fighter Alpha 2
still runs, but every decompressed tile is garbage, which makes a quick A/B.
Star Ocean does not get that far: without the chip it never enables a
background layer, so the screen stays black.

```bash
./fb1_nolut "Street Fighter Alpha 2 (USA).sfc" out/a sfa2 1100 1100   # stripes
./sdd1      "Street Fighter Alpha 2 (USA).sfc" out/b sfa2 1100 1100   # Akuma in flames
PAD_AUTO=40 PAD_FROM=600 PAD_MASK=0x1080 ./sdd1 "Street Fighter Alpha 2 (USA).sfc" out sfa2 8000 400
PAD_AUTO=30 PAD_FROM=900 PAD_MASK=0x1080 ./sdd1 "Star Ocean (Japan).sfc" out so 10000 500
```

The first `PAD_AUTO` run reaches a Ryu vs M. Bison fight; the second plays
through Star Ocean's opening. The run ends with a tally (here SF Alpha 2 over
20000 frames with `AUDIODBG` on, which keeps the sound driver's timing
device-like):

```
SDD1: transfers = 5554 (5342 from cache)  bytes requested = 8573266  decompressed = 502482 (94.1 % from cache)  largest transfer = 32000
SDD1: decompressions by bitplane type 0/1/2/3 = 30/0/182/0
SDD1: frames with a transfer = 3968 of 20001  worst frame requested 40192 bytes (frame 1669), decompressed 40192 (frame 1669)  avg requested per busy frame = 2160
SDD1: bank writes = 0  pages selected = 0x000f  armed-but-ignored DMAs = 0
SDD1: bank register value bits seen: $4804=00 $4805=00 $4806=00 $4807=00
SDD1: register reads:
```

`decompressed` against `bytes requested` is the output cache at work
(`SDD1_CACHE`, see `sdd1.c`): only misses cost decompression time. The cache
must never change what the game sees, so after touching it build the harness
once more with `-DSDD1_CACHE=0` added to the `sdd1` line and compare: frames
and `AUDIO_OUT` must be byte-identical over a long run of each game.

`SDD1_TRACE=<from>,<to>` prints every decompression in that frame window
(channel, A-bus source, ROM offset, length, first bytes), which shows exactly
which ROM data a scene is built from; comparing two ROM versions this way
showed the Star Ocean English patch leaves the opening's subtitle graphics
untouched (Mesen shows the same Japanese subtitles there).

`worst frame ... decompressed` is the one to watch for performance: the
device decompresses a whole transfer synchronously inside the `$420B` write on
core0, so a first-time scene load of 32-40 KB is a hitch of a few frames even
with the cache. `SDD1_TRACE` marks each transfer `hit` or `miss`. `pages
selected` is a bitmask of the 1 MB ROM pages the game mapped into `$c0-$ff`;
Star Ocean (6 MB) must show `0x003f`. Anything above bit 5 there means a bank
register read back wrong: Star Ocean saves and restores `$4806`/`$4807` by
reading them (thousands of reads per session), which is why `S9xGetCPU` answers
S-DD1 register reads from FillRAM instead of open bus.
