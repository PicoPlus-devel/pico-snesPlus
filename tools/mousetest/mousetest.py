#!/usr/bin/env python3
"""Build mousetest.sfc, a test ROM for the SNES Mouse in controller port 1.

The ROM shows a cursor that follows the mouse, its X/Y position, the motion
of the last frame, both buttons (highlighted while held) with a click counter
for each, the sensitivity setting the mouse reports, and the raw 32-bit report
in hex. It reads the mouse the way Nintendo's driver does: the auto-joypad
read delivers the first 16 bits ($4218/$4219), then 16 more bits are clocked
in by hand through $4016 for the Y and X motion bytes.

No SNES assembler is needed: this script carries a minimal two-pass 65816
assembler for the instructions the program uses and writes the 32 KB LoROM
image itself. The font is the 8x8 menu font of pico_shared.

Usage: tools/mousetest/mousetest.py [output.sfc]
       (default: mousetest.sfc next to this script)
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_H = os.path.join(HERE, '..', '..', 'pico_shared', 'font_8x8.h')

# ---------------------------------------------------------------------------
# Minimal 65816 assembler. The program runs with an 8-bit accumulator and
# 16-bit index registers from reset on, so an immediate's size follows from
# the mnemonic: accumulator immediates take 1 byte, X/Y immediates 2. Only
# absolute addressing (bank 0) is used, which reaches WRAM ($0000-$1FFF), the
# hardware registers and the ROM ($8000-$FFFF).

IMM8 = {'lda': 0xA9, 'cmp': 0xC9, 'and': 0x29, 'ora': 0x09, 'eor': 0x49,
        'adc': 0x69, 'sbc': 0xE9, 'rep': 0xC2, 'sep': 0xE2}
IMM16 = {'ldx': 0xA2, 'ldy': 0xA0, 'cpx': 0xE0, 'cpy': 0xC0}
ABS = {'lda': 0xAD, 'sta': 0x8D, 'stz': 0x9C, 'ldx': 0xAE, 'ldy': 0xAC,
       'stx': 0x8E, 'sty': 0x8C, 'inc': 0xEE, 'dec': 0xCE, 'cmp': 0xCD,
       'adc': 0x6D, 'sbc': 0xED, 'and': 0x2D, 'ora': 0x0D, 'rol': 0x2E,
       'jsr': 0x20, 'jmp': 0x4C}
ABSX = {'lda': 0xBD, 'sta': 0x9D, 'stz': 0x9E}
ABSY = {'lda': 0xB9}
ACC = {'lsr': 0x4A, 'asl': 0x0A, 'rol': 0x2A, 'ror': 0x6A, 'inc': 0x1A,
       'dec': 0x3A}
IMPL = {'sei': 0x78, 'cli': 0x58, 'clc': 0x18, 'sec': 0x38, 'xce': 0xFB,
        'txs': 0x9A, 'tax': 0xAA, 'tay': 0xA8, 'txa': 0x8A, 'tya': 0x98,
        'inx': 0xE8, 'iny': 0xC8, 'dex': 0xCA, 'dey': 0x88, 'pha': 0x48,
        'pla': 0x68, 'phx': 0xDA, 'plx': 0xFA, 'phy': 0x5A, 'ply': 0x7A,
        'plb': 0xAB, 'pld': 0x2B, 'rts': 0x60, 'rti': 0x40, 'nop': 0xEA}
BRANCH = {'bra': 0x80, 'bne': 0xD0, 'beq': 0xF0, 'bcc': 0x90, 'bcs': 0xB0,
          'bpl': 0x10, 'bmi': 0x30}


def assemble(source, org, symbols):
    """Assemble source at org; returns (bytes, labels). symbols holds the
    constants and data labels the source refers to."""

    def ev(expr, final):
        expr = re.sub(r"\$([0-9A-Fa-f]+)", r"0x\1", expr.strip())
        expr = re.sub(r"'(.)'", lambda m: str(ord(m.group(1))), expr)
        env = dict(symbols)
        env.update(labels)
        try:
            return eval(expr, {'__builtins__': {}}, env)
        except NameError:
            if final:
                raise
            return 0  # pass 1: sizes do not depend on values

    labels = {}
    out = bytearray()
    for final in (False, True):
        out = bytearray()
        for lineno, raw in enumerate(source.splitlines(), 1):
            line = raw.split(';', 1)[0].strip()
            if not line:
                continue
            if line.endswith(':'):
                if not final:
                    name = line[:-1]
                    if name in labels:
                        raise SystemExit('line %d: duplicate label %s' % (lineno, name))
                    labels[name] = org + len(out)
                continue
            parts = line.split(None, 1)
            mn = parts[0].lower()
            arg = parts[1].strip() if len(parts) > 1 else ''
            pc = org + len(out)
            try:
                if not arg or arg.lower() == 'a':
                    if mn in IMPL and not arg:
                        out.append(IMPL[mn])
                    elif mn in ACC:
                        out.append(ACC[mn])
                    else:
                        raise KeyError(mn)
                elif mn in BRANCH:
                    rel = ev(arg, final) - (pc + 2)
                    if final and not -128 <= rel <= 127:
                        raise SystemExit('line %d: branch out of range' % lineno)
                    out += bytes((BRANCH[mn], rel & 0xFF))
                elif arg.startswith('#'):
                    v = ev(arg[1:], final)
                    if mn in IMM8:
                        if final and not -128 <= v <= 255:
                            raise SystemExit('line %d: immediate too large' % lineno)
                        out += bytes((IMM8[mn], v & 0xFF))
                    else:
                        out += bytes((IMM16[mn], v & 0xFF, (v >> 8) & 0xFF))
                else:
                    low = arg.lower()
                    if low.endswith(',x'):
                        op, v = ABSX[mn], ev(arg[:-2], final)
                    elif low.endswith(',y'):
                        op, v = ABSY[mn], ev(arg[:-2], final)
                    else:
                        op, v = ABS[mn], ev(arg, final)
                    if final and not 0 <= v <= 0xFFFF:
                        raise SystemExit('line %d: address out of range' % lineno)
                    out += bytes((op, v & 0xFF, (v >> 8) & 0xFF))
            except KeyError:
                raise SystemExit('line %d: unsupported: %s' % (lineno, raw.strip()))
    return bytes(out), labels


# ---------------------------------------------------------------------------
# Graphics data

def load_font():
    """pico_shared's font_8x8: 95 glyphs (ASCII 32-126) stored row by row,
    leftmost pixel in bit 0. Returns 2bpp SNES tiles, one per glyph, color 1
    on color 0; tile n is ASCII n + 32."""
    text = open(FONT_H).read()
    data = [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{2})', text)]
    nchars = 95
    assert len(data) == nchars * 8, len(data)
    tiles = bytearray()
    for c in range(nchars):
        for row in range(8):
            bits = data[c + row * nchars]
            rev = int('{:08b}'.format(bits)[::-1], 2)  # SNES: leftmost = bit 7
            tiles += bytes((rev, 0))
    return bytes(tiles)


# The cursor: an arrow with its tip at the top-left pixel, the position the
# X/Y readout shows. 1 = fill (follows the button state), 2 = black outline.
CURSOR = [
    "2.......",
    "22......",
    "212.....",
    "2112....",
    "21112...",
    "211112..",
    "21122...",
    "22..2...",
]


def cursor_tile():
    """One 8x8 4bpp OBJ tile."""
    planes = [[0] * 8 for _ in range(4)]
    for y, row in enumerate(CURSOR):
        for x, ch in enumerate(row):
            color = 0 if ch == '.' else int(ch)
            for p in range(4):
                if color & (1 << p):
                    planes[p][y] |= 0x80 >> x
    tile = bytearray()
    for y in range(8):
        tile += bytes((planes[0][y], planes[1][y]))
    for y in range(8):
        tile += bytes((planes[2][y], planes[3][y]))
    return bytes(tile)


def bgr(r, g, b):
    return r | (g << 5) | (b << 10)


def palette():
    """All 256 CGRAM entries."""
    pal = [0] * 256
    pal[0] = bgr(0, 0, 12)        # backdrop: dark blue
    pal[1] = bgr(31, 31, 31)      # BG palette 0: text, white
    pal[5] = bgr(0, 31, 0)        # BG palette 1: button held, green
    pal[9] = bgr(31, 31, 0)       # BG palette 2: title, yellow
    # OBJ palettes 0-3 are picked by the button state: none, left, right, both.
    for p, fill in enumerate((bgr(31, 31, 31), bgr(0, 31, 0), bgr(31, 0, 0), bgr(31, 31, 0))):
        pal[128 + 16 * p + 1] = fill
        pal[128 + 16 * p + 2] = bgr(0, 0, 0)
    return b''.join(bytes((c & 0xFF, c >> 8)) for c in pal)


def oam():
    """All 128 sprites below the visible area, small size."""
    return bytes((0, 240, 0, 0)) * 128 + bytes(32)


# ---------------------------------------------------------------------------
# Screen layout: 32x28 tiles. OFF(row, col) is the byte offset of a cell in
# the tilemap buffer, which holds two bytes per cell (tile, attributes).

def OFF(row, col):
    return (row * 32 + col) * 2


TEXT = [
    (1, 8, 'SNES MOUSE TEST'),
    (3, 2, 'PORT 1:'),
    (5, 2, 'CURSOR'),
    (5, 12, 'X:'),
    (5, 20, 'Y:'),
    (7, 2, 'MOTION'),
    (7, 10, 'DX:'),
    (7, 18, 'DY:'),
    (9, 2, 'LEFT'),
    (9, 14, 'CLICKS:'),
    (10, 2, 'RIGHT'),
    (10, 14, 'CLICKS:'),
    (12, 2, 'SENSITIVITY:'),
    (14, 2, 'RAW:'),
    (16, 2, 'RAW: BITS 1-8, 9-16, Y, X'),
    (20, 2, 'Plug the mouse into port 1.'),
    (21, 2, 'Moving right and down counts'),
    (22, 2, 'up. Clicks of both buttons'),
    (23, 2, 'are counted.'),
    (25, 2, 'pico-snesPlus mouse test'),
]

STRINGS = {
    'S_MOUSE': 'SNES MOUSE   ',
    'S_NONE': 'NOT CONNECTED',
    'S_ON': 'ON',
    'S_OFF': '--',
}

# WRAM
SYMBOLS = {
    'OFF': OFF,
    'BUF': 0x0200,       # tilemap buffer, rows 0-27: 1792 bytes, DMA'd every frame
    'BUFLEN': 28 * 32 * 2,
    'VBL': 0x10,         # set by the NMI handler
    'MX': 0x11,          # cursor position
    'MY': 0x12,
    'JOYL': 0x13,        # $4218: right, left, sensitivity (2), signature (4)
    'JOYH': 0x14,        # $4219: bits 1-8, zero for a mouse
    'RY': 0x15,          # bits 17-24: Y motion, bit 7 = up
    'RX': 0x16,          # bits 25-32: X motion, bit 7 = left
    'BTN': 0x17,         # bit 0 left, bit 1 right
    'PBTN': 0x18,        # previous frame's BTN
    'CLKL': 0x19,        # click counters
    'CLKR': 0x1A,
    'TMP': 0x1B,
    'ISM': 0x1C,         # 1 = signature says mouse
}

# Static text, printed once at reset.
STATIC = '\n'.join(
    '    ldx #OFF(%d,%d)\n    ldy #T%d\n    jsr puts' % (r, c, i)
    for i, (r, c, _) in enumerate(TEXT))

SOURCE = r'''
reset:
    sei
    clc
    xce                 ; native mode
    rep #$10            ; X/Y 16-bit
    sep #$20            ; A 8-bit
    ldx #$1FFF
    txs
    ldx #$0000
    phx
    pld                 ; direct page 0
    lda #$00
    pha
    plb                 ; data bank 0
    lda #$80
    sta $2100           ; forced blank
    stz $4200

    lda #$01
    sta $2101           ; OBJ: 8x8, tiles at VRAM word $2000
    stz $2102
    stz $2103
    stz $2105           ; mode 0
    stz $2106
    lda #$10
    sta $2107           ; BG1 map at VRAM word $1000, 32x32
    stz $2108
    stz $2109
    stz $210A
    stz $210B           ; BG1 tiles at VRAM word $0000
    stz $210C
    stz $210D
    stz $210D
    stz $210E
    stz $210E
    lda #$80
    sta $2115           ; VRAM: increment after the high byte
    stz $2123
    stz $2124
    stz $2125
    stz $2126
    stz $2127
    stz $2128
    stz $2129
    stz $212A
    stz $212B
    lda #$11
    sta $212C           ; main screen: BG1 + OBJ
    stz $212D
    stz $212E
    stz $212F
    stz $2130
    stz $2131
    lda #$E0
    sta $2132
    stz $2133
    lda #$FF
    sta $4201
    stz $420B
    stz $420C
    stz $420D

    ; font -> VRAM $0000
    ldx #$0000
    stx $2116
    lda #$01
    sta $4300
    lda #$18
    sta $4301
    ldx #font
    stx $4302
    stz $4304
    ldx #FONTLEN
    stx $4305
    lda #$01
    sta $420B
    ; cursor -> VRAM $2000 (same DMA mode)
    ldx #$2000
    stx $2116
    ldx #cursor
    stx $4302
    stz $4304
    ldx #32
    stx $4305
    lda #$01
    sta $420B
    ; palette -> CGRAM
    stz $2121
    stz $4300
    lda #$22
    sta $4301
    ldx #pal
    stx $4302
    stz $4304
    ldx #512
    stx $4305
    lda #$01
    sta $420B
    ; sprites -> OAM
    stz $2102
    stz $2103
    stz $4300
    lda #$04
    sta $4301
    ldx #oamtab
    stx $4302
    stz $4304
    ldx #544
    stx $4305
    lda #$01
    sta $420B

    ldx #0
clrbuf:
    stz BUF,x
    inx
    cpx #BUFLEN
    bne clrbuf
STATIC
    lda #$08            ; title in BG palette 2
    ldx #OFF(1,8)
    ldy #15
    jsr setattr

    lda #128
    sta MX
    lda #112
    sta MY
    stz VBL
    stz BTN
    stz PBTN
    stz CLKL
    stz CLKR
    stz RX
    stz RY
    stz JOYL
    stz JOYH
    stz ISM
    jsr draw
    jsr upload
    lda #$0F
    sta $2100           ; screen on
    lda #$81
    sta $4200           ; NMI + auto-joypad read

main:
    lda VBL
    beq main
    stz VBL
    ; vblank: everything that touches VRAM or OAM first
    jsr upload
    stz $2102
    stz $2103
    lda MX
    sta $2104
    lda MY
    sta $2104
    stz $2104           ; tile 0
    lda BTN
    asl a
    ora #$30            ; priority 3, palette = button state
    sta $2104

    ; the auto-joypad read has the first 16 bits
joywait:
    lda $4212
    and #$01
    bne joywait
    lda $4218
    sta JOYL
    lda $4219
    sta JOYH
    ; clock in the next 16 by hand: Y byte, then X byte, MSB first
    ldy #8
rdy:
    lda $4016
    lsr a
    rol RY
    dey
    bne rdy
    ldy #8
rdx:
    lda $4016
    lsr a
    rol RX
    dey
    bne rdx

    stz ISM
    lda JOYL
    and #$0F
    cmp #$01            ; signature 0001: a mouse
    bne nomouse
    lda #$01
    sta ISM
    lda JOYL
    lsr a
    lsr a
    lsr a
    lsr a
    lsr a
    lsr a
    sta BTN             ; bit 0 left (bit 6 of $4218), bit 1 right (bit 7)
    bra counted
nomouse:
    stz BTN
counted:
    lda PBTN            ; count presses, not frames held
    eor #$FF
    and BTN
    lsr a
    bcc noleft
    inc CLKL
noleft:
    lsr a
    bcc noright
    inc CLKR
noright:
    lda BTN
    sta PBTN

    lda ISM
    beq moved
    ; Y: sign and magnitude, bit 7 = up; keep the cursor on 0-223
    lda RY
    and #$7F
    sta TMP
    lda RY
    bmi yup
    lda MY
    clc
    adc TMP
    bcs ymax
    cmp #224
    bcc yset
ymax:
    lda #223
    bra yset
yup:
    lda MY
    sec
    sbc TMP
    bcs yset
    lda #0
yset:
    sta MY
    ; X: bit 7 = left; 0-255
    lda RX
    and #$7F
    sta TMP
    lda RX
    bmi xleft
    lda MX
    clc
    adc TMP
    bcc xset
    lda #255
    bra xset
xleft:
    lda MX
    sec
    sbc TMP
    bcs xset
    lda #0
xset:
    sta MX
moved:
    jsr draw
    jmp main

; Fill in the changing parts of the screen buffer.
draw:
    ldx #OFF(3,10)
    ldy #S_MOUSE
    lda ISM
    bne drawstatus
    ldy #S_NONE
drawstatus:
    jsr puts

    lda MX
    ldx #OFF(5,14)
    jsr dec3
    lda MY
    ldx #OFF(5,22)
    jsr dec3

    lda ISM
    bne drawmotion
    lda #$00            ; no mouse: no motion to show
    ldx #OFF(7,13)
    jsr sdec
    lda #$00
    ldx #OFF(7,21)
    jsr sdec
    bra drawbuttons
drawmotion:
    lda RX
    ldx #OFF(7,13)
    jsr sdec
    lda RY
    ldx #OFF(7,21)
    jsr sdec

drawbuttons:
    ldx #OFF(9,9)
    ldy #S_OFF
    lda #$00
    sta TMP
    lda BTN
    and #$01
    beq drawleft
    ldy #S_ON
    lda #$04
    sta TMP
drawleft:
    jsr puts
    lda TMP
    ldx #OFF(9,2)
    ldy #9
    jsr setattr
    lda CLKL
    ldx #OFF(9,22)
    jsr dec3

    ldx #OFF(10,9)
    ldy #S_OFF
    lda #$00
    sta TMP
    lda BTN
    and #$02
    beq drawright
    ldy #S_ON
    lda #$04
    sta TMP
drawright:
    jsr puts
    lda TMP
    ldx #OFF(10,2)
    ldy #9
    jsr setattr
    lda CLKR
    ldx #OFF(10,22)
    jsr dec3

    lda ISM             ; no mouse: show 0
    beq drawspeed
    lda JOYL
    lsr a
    lsr a
    lsr a
    lsr a
    and #$03
drawspeed:
    clc
    adc #'0'-32
    sta BUF+OFF(12,15)

    lda JOYH
    ldx #OFF(14,7)
    jsr hex2
    lda JOYL
    ldx #OFF(14,10)
    jsr hex2
    lda RY
    ldx #OFF(14,13)
    jsr hex2
    lda RX
    ldx #OFF(14,16)
    jsr hex2
    rts

; Copy the screen buffer to the BG1 map. Vblank or forced blank only.
upload:
    ldx #$1000
    stx $2116
    lda #$01
    sta $4300
    lda #$18
    sta $4301
    ldx #BUF
    stx $4302
    stz $4304
    ldx #BUFLEN
    stx $4305
    lda #$01
    sta $420B
    rts

; Print the 0-terminated ROM string at Y to the buffer at offset X.
puts:
    lda $0000,y
    beq putsdone
    sec
    sbc #32
    sta BUF,x
    inx
    inx
    iny
    bra puts
putsdone:
    rts

; Set the attribute byte of Y cells from offset X to A.
setattr:
    sta BUF+1,x
    inx
    inx
    dey
    bne setattr
    rts

; A (0-255) as three decimal digits at offset X.
dec3:
    ldy #0
hund:
    cmp #100
    bcc hunddone
    sbc #100
    iny
    bra hund
hunddone:
    pha
    tya
    clc
    adc #'0'-32
    sta BUF,x
    pla
    ldy #0
tens:
    cmp #10
    bcc tensdone
    sbc #10
    iny
    bra tens
tensdone:
    pha
    tya
    clc
    adc #'0'-32
    sta BUF+2,x
    pla
    clc
    adc #'0'-32
    sta BUF+4,x
    rts

; A as a sign-and-magnitude motion byte: sign, then three digits, at offset X.
sdec:
    pha
    and #$80
    beq plus
    lda #'-'-32
    bra sign
plus:
    lda #'+'-32
sign:
    sta BUF,x
    pla
    and #$7F
    inx
    inx
    jmp dec3

; A as two hex digits at offset X.
hex2:
    pha
    lsr a
    lsr a
    lsr a
    lsr a
    jsr hexdig
    sta BUF,x
    pla
    and #$0F
    jsr hexdig
    sta BUF+2,x
    rts
hexdig:
    cmp #10
    bcc hexnum
    clc
    adc #'A'-10-32
    rts
hexnum:
    clc
    adc #'0'-32
    rts

nmi:
    pha
    lda $4210           ; acknowledge
    lda #$01
    sta VBL
    pla
    rti

irq:
    rti
'''.replace('STATIC', STATIC)


def build():
    font = load_font()
    blobs = [('font', font), ('cursor', cursor_tile()), ('pal', palette()),
             ('oamtab', oam())]
    for name, s in STRINGS.items():
        blobs.append((name, s.encode('ascii') + b'\0'))
    for i, (_, _, s) in enumerate(TEXT):
        blobs.append(('T%d' % i, s.encode('ascii') + b'\0'))

    symbols = dict(SYMBOLS)
    symbols['FONTLEN'] = len(font)
    # Code first at $8000, then the data; data addresses depend on the code
    # size, so assemble once with placeholders to learn it.
    code, _ = assemble(SOURCE, 0x8000, dict(symbols, **{n: 0 for n, _ in blobs}))
    addr = 0x8000 + len(code)
    for name, data in blobs:
        symbols[name] = addr
        addr += len(data)
    code, labels = assemble(SOURCE, 0x8000, symbols)

    rom = bytearray(b'\xFF' * 0x8000)
    image = code + b''.join(data for _, data in blobs)
    if 0x8000 + len(image) > 0xFFB0:
        raise SystemExit('program does not fit below the header')
    rom[0:len(image)] = image

    # LoROM header at $FFC0
    hdr = 0x7FC0
    rom[hdr:hdr + 21] = b'SNES MOUSE TEST'.ljust(21)
    rom[hdr + 0x15] = 0x20      # LoROM, SlowROM
    rom[hdr + 0x16] = 0x00      # ROM only
    rom[hdr + 0x17] = 0x05      # 32 KB
    rom[hdr + 0x18] = 0x00      # no SRAM
    rom[hdr + 0x19] = 0x01      # North America (NTSC)
    rom[hdr + 0x1A] = 0x00
    rom[hdr + 0x1B] = 0x00      # version
    rom[hdr + 0x1C:hdr + 0x20] = b'\xFF\xFF\x00\x00'
    vec = lambda off, a: rom.__setitem__(slice(off, off + 2), bytes((a & 0xFF, a >> 8)))
    for off in (0x7FE4, 0x7FE6, 0x7FE8, 0x7FEE):   # native COP, BRK, ABORT, IRQ
        vec(off, labels['irq'])
    vec(0x7FEA, labels['nmi'])                     # native NMI
    for off in (0x7FF4, 0x7FF8, 0x7FFA, 0x7FFE):   # emulation COP, ABORT, NMI, IRQ
        vec(off, labels['irq'])
    vec(0x7FFC, labels['reset'])                   # emulation RESET
    csum = sum(rom) & 0xFFFF
    rom[hdr + 0x1C:hdr + 0x20] = bytes((~csum & 0xFF, (~csum >> 8) & 0xFF,
                                        csum & 0xFF, csum >> 8))
    return bytes(rom), len(code)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, 'mousetest.sfc')
    rom, codelen = build()
    with open(out, 'wb') as f:
        f.write(rom)
    print('%s: %d bytes (code %d bytes)' % (out, len(rom), codelen))


if __name__ == '__main__':
    main()
