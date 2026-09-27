/* SPC7110 — Hudson's graphics decompressor, memory mapper, multiply/divide
 * unit and (on ROMType $F9) an RTC-4513.
 *
 * The implementation in spc7110.c is byuu and neviksti's reverse-engineered
 * emulator (version 0.03, 2008-08-10), by way of the C port in
 * libretro/snes9x2010. It is entirely self-contained: unlike snes9x <= 1.51
 * and ZSNES, no pre-decompressed "decomp pack" file is needed next to the ROM.
 *
 * Pico port changes are marked in spc7110.c; the ones that affect callers are:
 *   - S9xSPC7110RTCTick takes a monotonic microsecond clock instead of
 *     counting frames, because this port does not reliably hit 60 fps.
 *   - the RTC is never seeded from time(), which is a stub on bare metal;
 *     it starts unset (the game then prompts) and is carried in the .SAV.
 *   - save-state marshalling is gone; this port has no save states. */

#ifndef _SPC7110_H_
#define _SPC7110_H_

#ifdef __cplusplus
extern "C" {
#endif

#define SPC7110_DECOMP_BUFFER_SIZE	64

typedef struct
{
	uint8_t	index;
	uint8_t	invert;
} ContextState;

void S9xInitSPC7110 (void);
void S9xResetSPC7110 (void);
void S9xFreeSPC7110 (void);
void S9xSetSPC7110 (uint8_t Byte, uint16_t Address);
uint8_t S9xGetSPC7110 (uint16_t address);
uint8_t S9xGetSPC7110Byte (uint32_t address);
uint8_t * S9xGetBasePointerSPC7110 (uint32_t address);

/* Advance the RTC-4513. now_us is a free-running monotonic microsecond count
 * (time_us_64() on the device, a virtual clock in the host harness). Call it
 * once per emulated frame; a no-op unless the cart has the RTC. */
void S9xSPC7110RTCTick (uint64_t now_us);

/* The 20 RTC bytes, for the .SAV trailer (main.cpp). */
void S9xSPC7110RTCExport (uint8_t *out);
void S9xSPC7110RTCImport (const uint8_t *in);
#define SPC7110_RTC_BYTES 20

/* Decompression FIFO. A read pops one byte; bank $50 and $4800 both land here.
 * r4809/r480a are the transfer length counter the DMA path decrements. */
uint8_t spc7110_decomp_read (void);
void spc7110_decomp_start (void);
void spc7110_decomp_write(uint8_t data);
extern uint8_t r4809;	/* compression length low  */
extern uint8_t r480a;	/* compression length high */

/* DMA staging, mirroring msu1_dma_stage(). A DMA sourced from $4800 or from
 * bank $50 is a fixed-address A-bus transfer, so dma.c's GetBasePointer fast
 * path would copy one stale FillRAM byte `count` times. Drain the FIFO into a
 * transient PSRAM buffer instead and let the normal loop walk that. Returns
 * NULL for every transfer that is not the SPC7110's. */
uint8_t *spc7110_dma_stage(uint8_t abank, uint16_t aaddress, uint32_t count,
                           bool in_sa1_dma);
void spc7110_dma_free(void);

#if SPC7110_STATS
/* Bring-up / budget instrumentation, built only by the host harness
 * (-DSPC7110_STATS=1). max_seek_index is the one to watch: decomp_init ends
 * with `while (index--) decomp_read()`, so a single write to $4806 can decode
 * up to 262140 bytes inside one emulated CPU store. */
struct Spc7110Stats
{
	uint32_t mmio_reads;
	uint32_t mmio_writes;
	uint32_t window_reads;      /* $d0-$ff through the chip */
	uint32_t fifo_reads;        /* bytes popped out of the DCU */
	uint32_t decomp_inits;
	uint32_t mode_inits[4];
	uint32_t max_seek_index;
	uint32_t reg_writes[0x43];  /* $4800-$4842 */
	uint32_t reg_reads[0x43];
};
extern struct Spc7110Stats spc7110_stats;
#endif

#ifdef __cplusplus
}
#endif

#endif
