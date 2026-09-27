/* pico_snesPlus — oversized ROMs held in XIP flash instead of PSRAM.
 * See romflash.h for the layout and why the record lives in flash. */

#include "romflash.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/watchdog.h"
#include "hardware/sync.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "ff.h"

#include "FrensHelpers.h"
#include "crc32.h"

/* Defined in pico_shared/FrensHelpers.cpp but not declared in its header.
 * Forward-declared here rather than patching the submodule, which is shared
 * with ~10 sibling emulators. Reads the JEDEC capacity exponent once and
 * caches it; setClocksAndStartStdio() primes it before the overclock, so
 * calling it later is free and safe. */
namespace Frens { uint storage_get_flash_capacity(); }

/* Region: the top 8 MB of the board's 16 MB flash. Clear of the standalone
 * layout (app at 0x10000000), of the bootloader layout (FRENS_APP_BASE =
 * 0x10080000, see pico_shared/BootPartition.cmake) and of FlashParams, which
 * is placed immediately after the binary. */
#define ROMFLASH_BASE   0x10800000u
#define ROMFLASH_SIZE   (8u * 1024u * 1024u)
/* 64 KB, not one 4 KB sector, purely so the image below starts 64 KB-aligned.
 * flash_range_erase() only reaches for the 64 KB block-erase command on an
 * aligned range; at 4 KB granularity a 7 MB ROM is ~1792 sector erases and the
 * write takes minutes instead of tens of seconds. The record itself still only
 * occupies the first sector. */
#define ROMFLASH_HDR    (64u * 1024u)
#define ROMFLASH_IMAGE  (ROMFLASH_BASE + ROMFLASH_HDR)
#define ROMFLASH_MAX    (ROMFLASH_SIZE - ROMFLASH_HDR)
#define ROMFLASH_MAGIC  0x31524E53u   /* "SNR1" */

struct RomFlashRecord
{
    uint32_t magic;
    uint32_t base;      /* image base this record was written for */
    uint32_t size;      /* bytes of ROM image */
    uint32_t crc;       /* crc32 over the image as read back from flash */
    uint16_t fdate;     /* f_stat of the source file when it was written */
    uint16_t ftime;
    uint32_t reserved;
    char     path[FF_MAX_LFN + 1];
};

static_assert(sizeof(RomFlashRecord) <= ROMFLASH_HDR,
              "romflash record must fit in one flash sector");

static const RomFlashRecord *record(void)
{
    return (const RomFlashRecord *)(uintptr_t)ROMFLASH_BASE;
}

/* The region needs a board that actually carries 16 MB. Ask the chip rather
 * than trusting PICO_FLASH_SIZE_BYTES: that macro comes from a board header,
 * and an old tinyusb BSP header declares the Fruit Jam as 8 MB (CMakeLists.txt
 * overrides it, but the override is one stale include away from being wrong
 * again). storage_get_flash_capacity() reads the JEDEC id off the part itself,
 * so it is true regardless of what the build thinks. The other supported
 * boards carry less flash and would happily program addresses that alias back
 * over the app, so this must stay a check and not an assumption. */
static bool region_present(void)
{
    uint32_t capacity = Frens::storage_get_flash_capacity();
    uint32_t need = (ROMFLASH_BASE - XIP_BASE) + ROMFLASH_SIZE;

    if (capacity < need) {
        printf("romflash: flash is %u MB, need %u MB for the ROM region\n",
               (unsigned)(capacity / (1024 * 1024)),
               (unsigned)(need / (1024 * 1024)));
        return false;
    }
    /* Belt and braces: never let the region collide with the linked image or
     * the FlashParams sector that follows it. */
    if ((uintptr_t)&__flash_binary_end + FLASH_SECTOR_SIZE > ROMFLASH_BASE) {
        printf("romflash: app image reaches %p, region starts at %08X\n",
               (void *)&__flash_binary_end, (unsigned)ROMFLASH_BASE);
        return false;
    }
    return true;
}

size_t snes_romflash_capacity(void)
{
    return region_present() ? ROMFLASH_MAX : 0;
}

const uint8_t *snes_romflash_image(void)
{
    return (const uint8_t *)(uintptr_t)ROMFLASH_IMAGE;
}

const char *snes_romflash_recorded_path(void)
{
    if (!region_present()) return NULL;
    const RomFlashRecord *rec = record();
    if (rec->magic != ROMFLASH_MAGIC) return NULL;
    if (rec->base  != ROMFLASH_IMAGE) return NULL;
    if (rec->path[0] == 0)            return NULL;
    return rec->path;
}

/* crc32 over the image sitting in flash. Table-driven byte loop over ~7 MB of
 * XIP: tens of milliseconds, paid once per launch. */
static uint32_t image_crc(size_t size)
{
    const uint8_t *p = snes_romflash_image();
    uint32_t crc = 0;
    size_t done = 0;
    while (done < size) {
        UINT n = (UINT)((size - done > 65536u) ? 65536u : (size - done));
        crc = update_crc32(crc, p + done, n);
        done += n;
    }
    return crc;
}

bool snes_romflash_holds(const char *path, size_t size)
{
    if (!region_present()) return false;

#if ROMFLASH_FORCE_REWRITE
    /* Testing build: always claim the region does not hold this cart, so the
     * write path (and its progress bar) runs on every launch. */
    (void)path; (void)size;
    printf("romflash: ROMFLASH_FORCE_REWRITE - ignoring any existing image\n");
    return false;
#endif

    const RomFlashRecord *rec = record();

    if (rec->magic != ROMFLASH_MAGIC)            return false;
    if (rec->base  != ROMFLASH_IMAGE)            return false;
    if (rec->size  != (uint32_t)size)            return false;
    if (strcasecmp(rec->path, path) != 0)        return false;

    /* Size and timestamp catch the one thing the CRC cannot: the file on the
     * card was replaced by a different game under the same name. */
    FILINFO *fno = (FILINFO *)Frens::f_malloc(sizeof(FILINFO));
    bool stamped = (f_stat(path, fno) == FR_OK) &&
                   fno->fdate == rec->fdate && fno->ftime == rec->ftime;
    Frens::f_free(fno);
    if (!stamped) {
        printf("romflash: record is stale for %s\n", path);
        return false;
    }

    uint32_t crc = image_crc(size);
    if (crc != rec->crc) {
        printf("romflash: image crc %08X != recorded %08X\n",
               (unsigned)crc, (unsigned)rec->crc);
        return false;
    }

    printf("romflash: %s already in flash at %08X (%u KB)\n",
           path, (unsigned)ROMFLASH_IMAGE, (unsigned)(size / 1024));
    return true;
}

/* --- XIP timing across a flash write -------------------------------------
 *
 * On RP2350 flash_range_erase/_program end in the bootrom's
 * flash_enter_cmd_xip(), which resets qmi_hw->m[0] to its default: a plain 03h
 * serial read at CLKDIV=4, RXDELAY=0. At 378 MHz sysclk that is ~94.5 MHz with
 * no read delay -- far outside what a 03h read tolerates -- so the very next
 * instruction fetched from flash is garbage. The SDK saves and restores only
 * QMI CS1 (the PSRAM); nothing puts M0 back. This is the same hazard
 * FlashParams.cpp documents, and it is why streaming a ROM from SD across
 * erase/program calls crashes the moment f_read() is called: FatFs, the SD
 * driver and printf all live in flash.
 *
 * FlashParams gets away with one erase+program by never touching flash again
 * before the reboot. This path cannot -- it has to go back to the card ~1792
 * times. So instead of avoiding flash execution, make it sound again: drop M0
 * to a divisor a 03h serial read is comfortable at (<= 50 MHz). Slow, but this
 * is a one-off write that ends in a reboot, and it means the SD path, printf
 * and the watchdog all keep working in between.
 *
 * The fixup must run from RAM and must be inside the same interrupts-off
 * window as the flash op, so that no flash fetch can happen in between. */
static uint32_t safe_m0_timing;   /* computed before the first write */
static uint32_t saved_m0_timing;  /* the tuned quad-read setup, captured  */
static uint32_t saved_m0_rfmt;    /* before the first erase and put back  */
static uint32_t saved_m0_rcmd;    /* after the last program               */

static inline uint32_t romflash_calc_timing(void)
{
    uint32_t hz  = clock_get_hz(clk_sys);
    uint32_t div = (hz + 49999999u) / 50000000u;   /* ceil -> <= 50 MHz */
    if (div < 2)   div = 2;
    if (div > 255) div = 255;
    /* MIN_DESELECT[15:11]=14, RXDELAY[10:8]=2, CLKDIV[7:0] */
    return 0x60000000u | (14u << 11) | (2u << 8) | div;
}

static void __no_inline_not_in_flash_func(romflash_erase)(uint32_t off, size_t n)
{
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(off, n);
    qmi_hw->m[0].timing = safe_m0_timing;
    __compiler_memory_barrier();
    restore_interrupts(ints);
}

/* Put M0 back exactly as it was. Restoring the captured registers rather than
 * recomputing them is exact by construction -- whatever boot2 and
 * setClocksAndStartStdio() had programmed is what comes back, including the
 * quad-read format the bootrom replaced with 03h serial. This is what lets the
 * caller continue straight into the game instead of rebooting. */
static void __no_inline_not_in_flash_func(romflash_restore_xip)(void)
{
    uint32_t ints = save_and_disable_interrupts();
    qmi_hw->m[0].rfmt   = saved_m0_rfmt;
    qmi_hw->m[0].rcmd   = saved_m0_rcmd;
    qmi_hw->m[0].timing = saved_m0_timing;
    __compiler_memory_barrier();
    restore_interrupts(ints);
}

static void __no_inline_not_in_flash_func(romflash_program)(uint32_t off,
                                                           const uint8_t *data,
                                                           size_t n)
{
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(off, data, n);
    qmi_hw->m[0].timing = safe_m0_timing;
    __compiler_memory_barrier();
    restore_interrupts(ints);
}

/* The program source must live in internal SRAM. Erase and program take the
 * QMI out of XIP mode, which stops the PSRAM aperture as well as flash, so a
 * Frens::f_malloc buffer (PSRAM whenever PSRAM is enabled — which it always is
 * on the boards that reach this path) would be unreadable exactly when the
 * bootrom needs to read it. Plain malloc gives us SRAM. */
static uint8_t *alloc_sram_buffer(size_t *out_size)
{
    static const size_t sizes[] = { 64 * 1024, 32 * 1024, 16 * 1024,
                                    8 * 1024, FLASH_SECTOR_SIZE };
    for (size_t i = 0; i < count_of(sizes); i++) {
        uint8_t *p = (uint8_t *)malloc(sizes[i]);
        if (p) {
            *out_size = sizes[i];
            return p;
        }
    }
    *out_size = 0;
    return NULL;
}

bool snes_romflash_program(const char *path, size_t size,
                           snes_romflash_progress_fn progress)
{
    if (!region_present()) return false;

    if (size == 0 || size > ROMFLASH_MAX) {
        printf("romflash: %u KB does not fit the %u KB region\n",
               (unsigned)(size / 1024), (unsigned)(ROMFLASH_MAX / 1024));
        return false;
    }

    size_t bufsize = 0;
    uint8_t *buffer = alloc_sram_buffer(&bufsize);
    if (!buffer) {
        printf("romflash: no SRAM for a program buffer\n");
        return false;
    }

    FILINFO *fno = (FILINFO *)Frens::f_malloc(sizeof(FILINFO));
    if (f_stat(path, fno) != FR_OK) {
        printf("romflash: cannot stat %s\n", path);
        Frens::f_free(fno);
        free(buffer);
        return false;
    }
    uint16_t fdate = fno->fdate, ftime = fno->ftime;
    Frens::f_free(fno);

    FIL *fil = (FIL *)Frens::f_malloc(sizeof(FIL));
    if (f_open(fil, path, FA_READ) != FR_OK) {
        printf("romflash: cannot open %s\n", path);
        Frens::f_free(fil);
        free(buffer);
        return false;
    }

    /* Core1 is driving HSTX scan-out and its .rodata lives in flash; an erase
     * or program takes XIP away from both cores. Stop it — the picture is
     * gone for the rest of this session either way, and the caller reboots. */
    safe_m0_timing  = romflash_calc_timing();
    saved_m0_timing = qmi_hw->m[0].timing;
    saved_m0_rfmt   = qmi_hw->m[0].rfmt;
    saved_m0_rcmd   = qmi_hw->m[0].rcmd;
    printf("romflash: writing %s (%u KB) to %08X "
           "(XIP drops to CLKDIV=%u for the duration)\n",
           path, (unsigned)(size / 1024), (unsigned)ROMFLASH_IMAGE,
           (unsigned)(safe_m0_timing & 0xFF));

    /* core1 is deliberately left running. It drives HSTX scan-out from an SRAM
     * framebuffer and every function on that path is __not_in_flash_func, so it
     * survives the XIP-off windows -- the bootloader relies on the same thing
     * to keep its progress bar visible while it writes an app image. */

    /* Safety net: if anything below faults or stalls, come back up rather than
     * sitting on a frozen screen forever. Fed between operations. */
    watchdog_enable(8000, 1);

    /* Invalidate first: a power cut from here on leaves no record at all,
     * never a record that describes a half-written image. */
    romflash_erase(ROMFLASH_BASE - XIP_BASE, ROMFLASH_HDR);

    /* Erase the whole image range up front rather than per chunk. The bounce
     * buffer is whatever the SRAM heap can still spare -- typically 4 KB by
     * the time the menu has run -- and erasing in buffer-sized pieces would
     * force 4 KB sector erases, ~1792 of them. Erasing the range in one go
     * lets flash_range_erase() use the 64 KB block command (112 erases), but
     * that is ~30 s in a single call, so it goes in watchdog-sized batches. */
    {
        const uint32_t image_off = ROMFLASH_IMAGE - XIP_BASE;
        const size_t   span      = (size + 0xFFFFu) & ~(size_t)0xFFFFu;
        for (size_t done = 0; done < span; ) {
            size_t n = span - done;
            if (n > 512u * 1024u) n = 512u * 1024u;
            romflash_erase(image_off + (uint32_t)done, n);
            watchdog_update();
            done += n;
            if (progress) progress(SNES_ROMFLASH_ERASE, (uint32_t)done, (uint32_t)span);
            printf("romflash: erased %u / %u KB\n",
                   (unsigned)(done / 1024), (unsigned)(span / 1024));
        }
    }

    bool     ok      = true;
    size_t   written = 0;
    uint32_t src_crc = 0;   /* CRC of what we meant to write, see below */
    while (written < size) {
        size_t want = size - written;
        if (want > bufsize) want = bufsize;

        UINT br = 0;
        if (f_read(fil, buffer, (UINT)want, &br) != FR_OK || br != want) {
            /* A short read anywhere but the very end would desector-align
             * every offset after it, so treat it as fatal rather than
             * limping on. */
            printf("romflash: read error at %u (%u of %u bytes)\n",
                   (unsigned)written, (unsigned)br, (unsigned)want);
            ok = false;
            break;
        }

        /* flash_range_program wants whole pages; pad the tail of the last
         * chunk with the erased value. The range is already erased above. */
        size_t prog = (br + FLASH_PAGE_SIZE - 1) & ~(size_t)(FLASH_PAGE_SIZE - 1);
        if (prog > br) memset(buffer + br, 0xFF, prog - br);

        /* CRC the source as it goes by rather than reading the image back
         * afterwards: the read-back would run at the slow safe timing, and a
         * CRC taken here is the stronger check anyway -- it is what we meant
         * to write, so the verify below genuinely compares intent against
         * what landed in flash. */
        src_crc = update_crc32(src_crc, buffer, br);

        uint32_t off = (ROMFLASH_IMAGE - XIP_BASE) + (uint32_t)written;
        romflash_program(off, buffer, prog);
        watchdog_update();

        written += br;
        if (progress) progress(SNES_ROMFLASH_WRITE, (uint32_t)written, (uint32_t)size);
        if ((written & 0xFFFFF) == 0 || written == size)
            printf("romflash: %u / %u KB\n",
                   (unsigned)(written / 1024), (unsigned)(size / 1024));
    }

    f_close(fil);
    Frens::f_free(fil);
    free(buffer);

    if (!ok || written != size) {
        printf("romflash: write failed (%u of %u bytes)\n",
               (unsigned)written, (unsigned)size);
        romflash_restore_xip();
        watchdog_disable();
        return false;
    }

    /* Build the record, but do not commit it until the image verifies. */
    RomFlashRecord rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = ROMFLASH_MAGIC;
    rec.base  = ROMFLASH_IMAGE;
    rec.size  = (uint32_t)size;
    rec.crc   = src_crc;
    rec.fdate = fdate;
    rec.ftime = ftime;
    strncpy(rec.path, path, sizeof(rec.path) - 1);

    const size_t recpages = (sizeof(rec) + FLASH_PAGE_SIZE - 1) &
                            ~(size_t)(FLASH_PAGE_SIZE - 1);
    uint8_t *page = (uint8_t *)malloc(recpages);
    if (!page) {
        printf("romflash: no SRAM for the record page\n");
        romflash_restore_xip();
        watchdog_disable();
        return false;
    }
    memset(page, 0xFF, recpages);
    memcpy(page, &rec, sizeof(rec));
    romflash_program(ROMFLASH_BASE - XIP_BASE, page, recpages);
    free(page);

    /* Everything is written; put the tuned quad-read timing back so the rest
     * of the session -- including the verify immediately below -- runs at full
     * XIP speed, and so the caller can go straight on into the game. */
    romflash_restore_xip();
    watchdog_disable();

    uint32_t back = image_crc(size);
    if (back != rec.crc) {
        /* Erase the record rather than leave one that lies. The next launch
         * then simply rewrites. */
        printf("romflash: verify FAILED (flash %08X != source %08X)\n",
               (unsigned)back, (unsigned)rec.crc);
        romflash_erase(ROMFLASH_BASE - XIP_BASE, ROMFLASH_HDR);
        romflash_restore_xip();
        return false;
    }

    printf("romflash: wrote and verified %u KB, crc %08X\n",
           (unsigned)(size / 1024), (unsigned)rec.crc);
    return true;
}
