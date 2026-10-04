/* S-DD1 — Nintendo's graphics decompressor and 1 MB ROM bank mapper, used by
 * Street Fighter Alpha 2 and Star Ocean.
 *
 * The implementation in sdd1.c is Andreas Naive's decompressor by way of the
 * C port in libretro/snes9x2010, including that tree's later fixes (per-
 * channel $4800/$4801 arming, 4-bit bank registers, power-of-two mirroring for
 * Star Ocean's 48 Mbit ROM).
 *
 * Pico port changes that affect callers:
 *   - the decompressor state is no longer file-scope .bss; it lives in PSRAM
 *     together with a cache of decompressed output (~268 KB). Both are
 *     allocated by S9xResetSDD1 and freed by sdd1_dma_free, which the caller
 *     must run when the session ends. A cart without the chip costs 4 bytes
 *     of SRAM.
 *   - the DMA hook is sdd1_dma_stage(), the same shape as spc7110_dma_stage().
 *   - save-state marshalling is gone; this port has no save states. */

#ifndef _SDD1_H_
#define _SDD1_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* $4804-$4807: bank (0-3) selects which of $c0-$cf / $d0-$df / $e0-$ef /
 * $f0-$ff is remapped; value picks the 1 MB ROM page (low 4 bits). */
void S9xSetSDD1MemoryMap (uint32_t bank, uint32_t value);

/* Register write hook for $4804-$4807, called from S9xSetCPU. */
void S9xSetSDD1 (uint8_t byte, uint16_t address);

/* Power-on state: nothing armed, page i in window i. Must run after the PPU
 * reset, which fills $4800-$48ff with open-bus bytes (see cpu.c). Also
 * allocates the PSRAM staging block on the first call. */
void S9xResetSDD1 (void);

/* DMA staging, mirroring spc7110_dma_stage(). The chip substitutes
 * decompressed data for a fixed-address DMA out of banks $c0-$ff on a channel
 * armed in both $4800 and $4801; find the result in the PSRAM output cache or
 * decompress it there, and let dma.c's normal loop walk that. Returns NULL for
 * every transfer the chip does not intercept. count is 1..0x10000. */
uint8_t *sdd1_dma_stage (uint8_t channel, uint32_t count);
void sdd1_dma_free (void);

/* Counters since the previous call, then reset: microseconds spent in
 * sdd1_dma_stage (device only; 0 on the host), bytes the game asked for, and
 * bytes that had to be decompressed rather than taken from the cache. False
 * when no S-DD1 cart is loaded. */
bool sdd1_take_stats (uint32_t *us, uint32_t *requested, uint32_t *decompressed);

#if SDD1_STATS
/* Bring-up instrumentation, built only by the host harness (-DSDD1_STATS=1). */
struct Sdd1Stats
{
	uint32_t stages;          /* transfers the chip handled */
	uint32_t hit_stages;      /* ... of which served from the cache */
	uint32_t bytes;           /* bytes requested, cumulative */
	uint32_t decomp_bytes;    /* bytes actually decompressed (cache misses) */
	uint32_t max_transfer;    /* largest single transfer */
	uint32_t ignored_armed;   /* armed channel, but not a fixed $c0+ DMA */
	uint32_t bank_writes;     /* $4804-$4807 writes */
	uint8_t  bank_or[4];      /* OR of every value written to $4804-$4807 */
	uint32_t pages_seen;      /* bit n set: 1 MB page n was selected */
	uint32_t reg_reads[8];    /* $4800-$4807 reads */
	uint32_t mode_count[4];   /* bitplane type of each decompression */
};
extern struct Sdd1Stats sdd1_stats;
/* Nonzero: print every decompression (source, ROM offset, length). The
 * harness sets it for the SDD1_TRACE=<from>,<to> frame window. */
extern int sdd1_trace;
#endif

#ifdef __cplusplus
}
#endif

#endif
