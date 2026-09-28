# Host-side render test harness

Runs the vendored snes9x core from this repository natively on Linux and
dumps rendered frames as PPM images. Render bugs can be reproduced,
bisected and fixed on a desktop machine in seconds — no board, flashing or
capture hardware needed. It was built to find the DKC "Nintendo presents"
mode-5 strip-seam bug and is kept for future rendering work.

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

Needs only a native `gcc`. Produces six binaries in this directory:

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

The harness mixes one video frame of audio per frame at the real-time rate,
the way `core1_mix_task` does on the device, so sound faults reproduce here
too — which is a great deal faster than reflashing a board to test a theory.
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

`AUDIO_OUT=<path>` alongside `AUDIODBG` writes that same mixed stream to a
file (44.1 kHz, s16, stereo), for listening:

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
`dma.c` / `ppu.c` / `cpu.c` hooks. Without the chip both games still boot, but
every decompressed tile is garbage, which makes a quick A/B:

```bash
./fb1_nolut "Street Fighter Alpha 2 (USA).sfc" out/a sfa2 1100 1100   # stripes
./sdd1      "Street Fighter Alpha 2 (USA).sfc" out/b sfa2 1100 1100   # Akuma in flames
PAD_AUTO=40 PAD_FROM=600 PAD_MASK=0x1080 ./sdd1 "Street Fighter Alpha 2 (USA).sfc" out sfa2 8000 400
PAD_AUTO=30 PAD_FROM=900 PAD_MASK=0x1080 ./sdd1 "Star Ocean (Japan).sfc" out so 10000 500
```

The first `PAD_AUTO` run reaches a Ryu vs M. Bison fight; the second plays
through Star Ocean's opening. The run ends with a tally:

```
SDD1: decompressions = 2100 (bitplane type 0/1/2/3 = 37/0/2063/0)  bytes = 3285170  largest transfer = 32000
SDD1: frames with decompression = 1483 of 8001  worst frame = 40192 bytes (frame 1669)  avg per busy frame = 2215
SDD1: bank writes = 0  pages selected = 0x000f  armed-but-ignored DMAs = 0
SDD1: bank register value bits seen: $4804=00 $4805=00 $4806=00 $4807=00
SDD1: register reads:
```

`SDD1_TRACE=<from>,<to>` prints every decompression in that frame window
(channel, A-bus source, ROM offset, length, first bytes), which shows exactly
which ROM data a scene is built from; comparing two ROM versions this way
showed the Star Ocean English patch leaves the opening's subtitle graphics
untouched (Mesen shows the same Japanese subtitles there).

`worst frame` is the one to watch for performance: the device decompresses a
whole transfer synchronously inside the `$420B` write on core0. `pages
selected` is a bitmask of the 1 MB ROM pages the game mapped into `$c0-$ff`;
Star Ocean (6 MB) must show `0x003f`. Anything above bit 5 there means a bank
register read back wrong: Star Ocean saves and restores `$4806`/`$4807` by
reading them (thousands of reads per session), which is why `S9xGetCPU` answers
S-DD1 register reads from FillRAM instead of open bus.
