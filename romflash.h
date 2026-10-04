/* pico_snesPlus — oversized ROMs held in XIP flash instead of PSRAM.
 *
 * A 7 MB cart (Tengai Makyou Zero's English translation, SPC7110) does not
 * fit in the 8 MB PSRAM alongside snes9x's ~1 MB working set, and the
 * framework's loader refuses it long before that: flashromtoPsram() skips the
 * PSRAM preload for any file larger than availMem - 512 KB and returns
 * nullptr, leaving ROM_FILE_ADDR == 0.
 *
 * This module takes that case over. The ROM is programmed once into a region
 * of the board's 16 MB flash that is clear of both flash layouts (standalone
 * app at 0x10000000, bootloader app partition at 0x10080000) and clear of
 * FlashParams, which sits immediately after the binary. Memory.ROM then points
 * straight at XIP and PSRAM is left entirely to the emulator.
 *
 *   0x10000000  bootloader / app       (~0.6 MB + FlashParams sector)
 *   0x10800000  record sector          4 KB, describes what is in the region
 *   0x10801000  ROM image              up to 8 MB - 4 KB
 *   0x11000000  end of flash
 *
 * The record lives in flash rather than on the SD card so the region is
 * self-describing: swapping cards cannot make it lie. It is erased before the
 * first image sector is touched and written only after the image verifies, so
 * a record that exists always describes complete flash content.
 *
 * ROM_FILE_ADDR is deliberately NOT used to carry the flash pointer: main.cpp
 * calls Frens::f_free(ROM_FILE_ADDR) whenever PSRAM is enabled, which would
 * hand a flash address to lwmem. */

#ifndef PICO_SNESPLUS_ROMFLASH_H
#define PICO_SNESPLUS_ROMFLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest ROM image the region can hold. */
size_t snes_romflash_capacity(void);

/* True when the image already in flash is exactly this file (record magic,
 * path, size, modification time, then a CRC over the flash image itself).
 * Reads flash only; safe to call before anything is allocated. */
bool snes_romflash_holds(const char *path, size_t size);

/* Base of the ROM image in the XIP aperture. Only meaningful after
 * snes_romflash_holds() has returned true. */
const uint8_t *snes_romflash_image(void);

/* Path of the cart the region currently holds, or NULL if the record is not
 * valid. Points into flash; copy it if you need it to outlive a rewrite.
 * Deliberately ignores ROMFLASH_FORCE_REWRITE -- this answers "what is in the
 * region", not "should it be rewritten". */
const char *snes_romflash_recorded_path(void);

/* Progress callback. phase is SNES_ROMFLASH_ERASE or _WRITE. Called between
 * flash operations, with XIP already put back to a safe timing, so it may run
 * from flash — but core1 is still servicing scan-out throughout, so anything
 * it draws must itself be SRAM-resident (see progress_bar.h). May be NULL. */
#define SNES_ROMFLASH_ERASE 0
#define SNES_ROMFLASH_WRITE 1
typedef void (*snes_romflash_progress_fn)(int phase, uint32_t done, uint32_t total);

/* Program `path` into the region, and leave the system able to carry straight
 * on into the game: QMI M0's timing and read format are saved before the first
 * erase and restored after the last program, so no reboot is needed and core1
 * keeps driving the display the whole time. Verifies the image before writing
 * the record, so a false return always leaves no record and the next attempt
 * simply rewrites. */
bool snes_romflash_program(const char *path, size_t size,
                           snes_romflash_progress_fn progress);

#ifdef __cplusplus
}
#endif

#endif
