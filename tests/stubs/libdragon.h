/**
 * @file libdragon.h
 * @brief Minimal libdragon contract used to type-check the default backend.
 *
 * The `tests/libtrpak_default_backend.o` target compiles libtrpak.c *without*
 * `TRPAK_NO_DEFAULT_IO` and with this directory on the include path, so the
 * `platform_*` callbacks are compiled on the host. Only declarations are
 * provided — nothing is linked — which is enough to catch a renamed function
 * or a changed signature in the libdragon layer without an N64 toolchain.
 *
 * Keep these prototypes in sync with the real libdragon headers.
 */
#ifndef TEST_LIBDRAGON_H
#define TEST_LIBDRAGON_H

#include <stddef.h>
#include <stdint.h>

/** Reads 32 bytes from an accessory address over Joybus. */
int joybus_accessory_read(int controller, uint16_t address, uint8_t *data);

/** Writes 32 bytes to an accessory address over Joybus. */
int joybus_accessory_write(
    int controller,
    uint16_t address,
    const uint8_t *data
);

/** Busy-waits for the given number of milliseconds. */
void wait_ms(unsigned int milliseconds);

/** Writes back and invalidates the data cache lines covering a buffer. */
void data_cache_hit_writeback_invalidate(void *address, size_t size);

#endif
