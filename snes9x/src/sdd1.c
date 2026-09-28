/* This file is part of Snes9x. See LICENSE file. */

/* S-DD1 decompressor and bank mapper
 *
 * Based on code and documentation by Andreas Naive, who deserves a great deal
 * of thanks and credit for figuring this out.
 *
 * Andreas says:
 * The author is greatly indebted with The Dumper, without whose help and
 * patience providing him with real S-DD1 data the research had never been
 * possible. He also wish to note that in the very beggining of his research,
 * Neviksti had done some steps in the right direction. By last, the author is
 * indirectly indebted to all the people that worked and contributed in the
 * S-DD1 issue in the past.
 *
 * Ported from libretro/snes9x2010 src/sdd1.c. Pico port changes:
 *   - the decompressor's state was ~220 bytes of file-scope .bss; it is now a
 *     struct in the same PSRAM block as the DMA staging buffer, allocated when
 *     an S-DD1 cart is reset, so a cart without the chip pays nothing but the
 *     pointer. Every decompression resets that state, so nothing is lost by
 *     keeping it off the SRAM heap.
 *   - context_MPS and prev_bits are narrowed from int; only their low bits
 *     are ever used.
 *   - the DMA trigger moved here from S9xDoDMA (sdd1_dma_stage), so the SRAM-
 *     resident dma.c only pays for a call.
 *   - sdd1_mirror is iterative rather than recursive. */

#if ENABLE_SDD1

#include <stdio.h>
#include <string.h>

#include "snes9x.h"
#include "memmap.h"
#include "ppu.h"
#include "sdd1.h"
#include "port_alloc.h"

#if SDD1_STATS
struct Sdd1Stats sdd1_stats;
int sdd1_trace;
#endif

/* ------------------------------------------------------------------------
 * Bank mapper
 * ------------------------------------------------------------------------ */

/* Fold an out-of-range ROM position back into the image the way the cartridge
 * address decoding mirrors it, including for a non-power-of-two size such as
 * Star Ocean's 48 Mbit: pages 6 and 7 mirror pages 4 and 5. Same
 * decomposition as bsnes' mirror(). */
static uint32_t sdd1_mirror (uint32_t size, uint32_t pos)
{
	uint32_t base = 0, mask;

	while (size && pos >= size)
	{
		mask = UINT32_C(1) << 31;
		while (!(pos & mask))
			mask >>= 1;
		if (size > mask)
		{
			base += mask;
			size -= mask;
		}
		pos -= mask;
	}
	return size ? base + pos : 0;
}

void S9xSetSDD1MemoryMap (uint32_t bank, uint32_t value)
{
	uint32_t c, i;

	bank = 0xc00 + bank * 0x100;
	/* Four bank-select bits (16 pages of 1 MB), not three: ares and the MiSTer
	 * core both decode bits 0-3. */
	value = (value & 0x0f) << 20;

#if SDD1_STATS
	sdd1_stats.pages_seen |= 1u << (value >> 20);
#endif

	for (c = 0; c < 0x100; c += 16)
	{
		uint8_t *block = &Memory.ROM[sdd1_mirror(Memory.CalculatedSize, value + (c << 12))];
		for (i = c; i < c + 16; i++)
			Memory.Map[i + bank] = block;
	}
}

/* $4800/$4801 need no action on write: S9xSetCPU stores every byte in
 * FillRAM after its switch, and sdd1_dma_stage reads them from there. */
void S9xSetSDD1 (uint8_t byte, uint16_t address)
{
#if SDD1_STATS
	sdd1_stats.bank_writes++;
	sdd1_stats.bank_or[address - 0x4804] |= byte;
#endif
	S9xSetSDD1MemoryMap(address - 0x4804, byte);
}

static void sdd1_dma_alloc (void);

void S9xResetSDD1 (void)
{
	uint32_t i;

	sdd1_dma_alloc();

	memset(&Memory.FillRAM[0x4800], 0, 4);
	for (i = 0; i < 4; i++)
	{
		Memory.FillRAM[0x4804 + i] = (uint8_t) i;
		S9xSetSDD1MemoryMap(i, i);
	}
}

/* ------------------------------------------------------------------------
 * Decompressor
 * ------------------------------------------------------------------------ */

typedef struct
{
	uint8_t  *in_buf;
	int32_t   valid_bits;
	uint16_t  in_stream;
	uint16_t  high_context_bits;
	uint16_t  low_context_bits;
	uint16_t  prev_bits[8];
	uint8_t   bit_ctr[8];
	uint8_t   context_states[32];
	uint8_t   context_MPS[32];
} Sdd1Decomp;

static const struct
{
	uint8_t code_size;
	uint8_t MPS_next;
	uint8_t LPS_next;
} sdd1_evolution_table[] = {
	/*  0 */ { 0,25,25},
	/*  1 */ { 0, 2, 1},
	/*  2 */ { 0, 3, 1},
	/*  3 */ { 0, 4, 2},
	/*  4 */ { 0, 5, 3},
	/*  5 */ { 1, 6, 4},
	/*  6 */ { 1, 7, 5},
	/*  7 */ { 1, 8, 6},
	/*  8 */ { 1, 9, 7},
	/*  9 */ { 2,10, 8},
	/* 10 */ { 2,11, 9},
	/* 11 */ { 2,12,10},
	/* 12 */ { 2,13,11},
	/* 13 */ { 3,14,12},
	/* 14 */ { 3,15,13},
	/* 15 */ { 3,16,14},
	/* 16 */ { 3,17,15},
	/* 17 */ { 4,18,16},
	/* 18 */ { 4,19,17},
	/* 19 */ { 5,20,18},
	/* 20 */ { 5,21,19},
	/* 21 */ { 6,22,20},
	/* 22 */ { 6,23,21},
	/* 23 */ { 7,24,22},
	/* 24 */ { 7,24,23},
	/* 25 */ { 0,26, 1},
	/* 26 */ { 1,27, 2},
	/* 27 */ { 2,28, 4},
	/* 28 */ { 3,29, 8},
	/* 29 */ { 4,30,12},
	/* 30 */ { 5,31,16},
	/* 31 */ { 6,32,18},
	/* 32 */ { 7,24,22}
};

static const uint8_t run_table[128] = {
	128,  64,  96,  32, 112,  48,  80,  16, 120,  56,  88,  24, 104,  40,  72,
	  8, 124,  60,  92,  28, 108,  44,  76,  12, 116,  52,  84,  20, 100,  36,
	 68,   4, 126,  62,  94,  30, 110,  46,  78,  14, 118,  54,  86,  22, 102,
	 38,  70,   6, 122,  58,  90,  26, 106,  42,  74,  10, 114,  50,  82,  18,
	 98,  34,  66,   2, 127,  63,  95,  31, 111,  47,  79,  15, 119,  55,  87,
	 23, 103,  39,  71,   7, 123,  59,  91,  27, 107,  43,  75,  11, 115,  51,
	 83,  19,  99,  35,  67,   3, 125,  61,  93,  29, 109,  45,  77,  13, 117,
	 53,  85,  21, 101,  37,  69,   5, 121,  57,  89,  25, 105,  41,  73,   9,
	113,  49,  81,  17,  97,  33,  65,   1
};

static inline uint8_t GetCodeword (Sdd1Decomp *s, int bits)
{
	uint8_t tmp;

	if (!s->valid_bits)
	{
		s->in_stream |= *(s->in_buf++);
		s->valid_bits = 8;
	}
	s->in_stream <<= 1;
	s->valid_bits--;
	s->in_stream ^= 0x8000;
	if (s->in_stream & 0x8000)
		return 0x80 + (1 << bits);
	tmp = (s->in_stream >> 8) | (0x7f >> bits);
	s->in_stream <<= bits;
	s->valid_bits -= bits;
	if (s->valid_bits < 0)
	{
		s->in_stream |= (*(s->in_buf++)) << (-s->valid_bits);
		s->valid_bits += 8;
	}
	return run_table[tmp];
}

static inline uint8_t GolombGetBit (Sdd1Decomp *s, int code_size)
{
	if (!s->bit_ctr[code_size])
		s->bit_ctr[code_size] = GetCodeword(s, code_size);
	s->bit_ctr[code_size]--;
	if (s->bit_ctr[code_size] == 0x80)
	{
		s->bit_ctr[code_size] = 0;
		return 2; /* secret code for 'last zero'. ones are always last. */
	}
	return (s->bit_ctr[code_size] == 0) ? 1 : 0;
}

static inline uint8_t ProbGetBit (Sdd1Decomp *s, uint8_t context)
{
	uint8_t state = s->context_states[context];
	uint8_t bit   = GolombGetBit(s, sdd1_evolution_table[state].code_size);

	if (bit & 1)
	{
		s->context_states[context] = sdd1_evolution_table[state].LPS_next;
		if (state < 2)
		{
			s->context_MPS[context] ^= 1;
			return s->context_MPS[context]; /* just inverted, so just return it */
		}
		return s->context_MPS[context] ^ 1; /* we know bit is 1, so use a constant */
	}
	else if (bit)
		s->context_states[context] = sdd1_evolution_table[state].MPS_next; /* zero here, zero there, no difference so drop through. */
	return s->context_MPS[context]; /* we know bit is 0, so don't bother xoring */
}

static inline uint8_t GetBit (Sdd1Decomp *s, uint8_t cur_bitplane)
{
	uint8_t bit = ProbGetBit(s, ((cur_bitplane & 1) << 4)
			| ((s->prev_bits[cur_bitplane] & s->high_context_bits) >> 5)
			| (s->prev_bits[cur_bitplane] & s->low_context_bits));

	s->prev_bits[cur_bitplane] <<= 1;
	s->prev_bits[cur_bitplane] |= bit;
	return bit;
}

static void SDD1_decompress (Sdd1Decomp *s, uint8_t *out, uint8_t *in, uint32_t len)
{
	uint8_t bit, i, plane;
	uint8_t byte1, byte2;
	int     bitplane_type;

	bitplane_type = in[0] >> 6;

	switch (in[0] & 0x30)
	{
		case 0x00:
			s->high_context_bits = 0x01c0;
			s->low_context_bits  = 0x0001;
			break;
		case 0x10:
			s->high_context_bits = 0x0180;
			s->low_context_bits  = 0x0001;
			break;
		case 0x20:
			s->high_context_bits = 0x00c0;
			s->low_context_bits  = 0x0001;
			break;
		case 0x30:
			s->high_context_bits = 0x0180;
			s->low_context_bits  = 0x0003;
			break;
	}

	s->in_stream  = (in[0] << 11) | (in[1] << 3);
	s->valid_bits = 5;
	s->in_buf     = in + 2;
	memset(s->bit_ctr, 0, sizeof(s->bit_ctr));
	memset(s->context_states, 0, sizeof(s->context_states));
	memset(s->context_MPS, 0, sizeof(s->context_MPS));
	memset(s->prev_bits, 0, sizeof(s->prev_bits));

#if SDD1_STATS
	sdd1_stats.mode_count[bitplane_type]++;
#endif

	switch (bitplane_type)
	{
		case 0:
			while (1)
			{
				for (byte1 = byte2 = 0, bit = 0x80; bit; bit >>= 1)
				{
					if (GetBit(s, 0))
						byte1 |= bit;
					if (GetBit(s, 1))
						byte2 |= bit;
				}
				*(out++) = byte1;
				if (!--len)
					break;
				*(out++) = byte2;
				if (!--len)
					break;
			}
			break;
		case 1:
			i = plane = 0;
			while (1)
			{
				for (byte1 = byte2 = 0, bit = 0x80; bit; bit >>= 1)
				{
					if (GetBit(s, plane))
						byte1 |= bit;
					if (GetBit(s, plane + 1))
						byte2 |= bit;
				}
				*(out++) = byte1;
				if (!--len)
					break;
				*(out++) = byte2;
				if (!--len)
					break;
				if (!(i += 32))
					plane = (plane + 2) & 7;
			}
			break;
		case 2:
			i = plane = 0;
			while (1)
			{
				for (byte1 = byte2 = 0, bit = 0x80; bit; bit >>= 1)
				{
					if (GetBit(s, plane))
						byte1 |= bit;
					if (GetBit(s, plane + 1))
						byte2 |= bit;
				}
				*(out++) = byte1;
				if (!--len)
					break;
				*(out++) = byte2;
				if (!--len)
					break;
				if (!(i += 32))
					plane ^= 2;
			}
			break;
		case 3:
			do
			{
				for (byte1 = plane = 0, bit = 1; bit; bit <<= 1, plane++)
					if (GetBit(s, plane))
						byte1 |= bit;
				*(out++) = byte1;
			} while (--len);
			break;
	}
}

/* ------------------------------------------------------------------------
 * DMA staging
 * ------------------------------------------------------------------------ */

/* One PSRAM block for the decompressor state and the output of the largest
 * possible transfer (dma.c normalises a zero TransferBytes to 0x10000). Fixed
 * size, for the same reasons as the SPC7110's staging buffer (spc7110.c): a
 * failed mid-game allocation would leave dma.c copying compressed bytes, and
 * regrowth would churn the next-fit heap.
 *
 * Allocated at cart reset rather than on the first transfer: both S-DD1 games
 * decompress from the first screen on, and reset is where a failure can be
 * reported once without a static "already said so" flag in SRAM. Freed by
 * main.cpp when the session ends. */
#define SDD1_DMA_BUF_BYTES 0x10000u

typedef struct
{
	Sdd1Decomp st;
	uint8_t    out[SDD1_DMA_BUF_BYTES];
} Sdd1Stage;

static Sdd1Stage *sdd1_stage_buf;

static void sdd1_dma_alloc (void)
{
	if (sdd1_stage_buf)
		return;
	sdd1_stage_buf = (Sdd1Stage *) port_alloc_psram(sizeof(Sdd1Stage));
	if (!sdd1_stage_buf)
		/* Loud, not silent: without the buffer dma.c copies the compressed
		 * bytes, so say so rather than let it look like a game bug. */
		printf("sdd1: cannot allocate %u bytes of PSRAM - graphics will be wrong\n",
		       (unsigned) sizeof(Sdd1Stage));
}

void sdd1_dma_free (void)
{
	port_alloc_free(sdd1_stage_buf);
	sdd1_stage_buf = NULL;
}

uint8_t *sdd1_dma_stage (uint8_t channel, uint32_t count)
{
	SDMA    *d = &DMA[channel];
	uint8_t  armed = Memory.FillRAM[0x4800] & Memory.FillRAM[0x4801] & (1 << channel);
	uint8_t *in;

	if (!armed)
		return NULL;

	/* The chip streams decompressed data only for reads of banks $c0-$ff,
	 * which a fixed-address DMA makes repeatedly at one address. Both retail
	 * games arm channel 0 with $4800 = $4801 = $01 (snes9x2010, verified by
	 * logging there). An armed channel doing anything else keeps its run bit
	 * for the transfer it was armed for. */
	if (!d->AAddressFixed || d->ABank < 0xc0)
	{
#if SDD1_STATS
		sdd1_stats.ignored_armed++;
#endif
		return NULL;
	}

	/* On completion the hardware clears just this channel's run bit and
	 * leaves any other armed channel alone. */
	Memory.FillRAM[0x4801] &= ~(1 << channel);

	/* No buffer: already reported by sdd1_dma_alloc. No base pointer: cannot
	 * happen for $c0-$ff, which S9xSetSDD1MemoryMap always points at ROM. */
	in = GetBasePointer((d->ABank << 16) | d->AAddress);
	if (!sdd1_stage_buf || !in)
		return NULL;

	SDD1_decompress(&sdd1_stage_buf->st, sdd1_stage_buf->out, in + d->AAddress, count);

#if SDD1_STATS
	if (sdd1_trace)
		printf("sdd1: ch%u %02x:%04x rom+%06x len %5u hdr %02x %02x -> %02x %02x %02x %02x\n",
		       channel, d->ABank, d->AAddress,
		       (unsigned) (in + d->AAddress - Memory.ROM), (unsigned) count,
		       in[d->AAddress], in[d->AAddress + 1],
		       sdd1_stage_buf->out[0], sdd1_stage_buf->out[1],
		       sdd1_stage_buf->out[2], sdd1_stage_buf->out[3]);
	sdd1_stats.stages++;
	sdd1_stats.bytes += count;
	if (count > sdd1_stats.max_transfer)
		sdd1_stats.max_transfer = count;
#endif

	return sdd1_stage_buf->out;
}

#endif /* ENABLE_SDD1 */
