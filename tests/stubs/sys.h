/**
 * @file sys.h
 * @brief Minimal EverDrive 64 DMA contract for the `TRPAK_ENABLE_ED64_DMA`
 *        build.
 *
 * libtrpak.c only includes `sys.h` when compiled with `TRPAK_ENABLE_ED64_DMA`;
 * these declarations let the host build type-check `platform_dma_store()` and
 * `platform_dma_load()` without an EverDrive SDK. An application targeting real
 * hardware supplies its own `sys.h` instead.
 */
#ifndef TEST_SYS_H
#define TEST_SYS_H

#include <stddef.h>

/** Copies `size` bytes from RDRAM into cartridge/SDRAM space. */
void dma_write_s(void *source, unsigned long destination, size_t size);

/** Copies `size` bytes from cartridge/SDRAM space back into RDRAM. */
void dma_read_s(void *destination, unsigned long source, size_t size);

#endif
