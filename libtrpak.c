/**
 * @file libtrpak.c
 * @brief Implementation of the Nintendo 64 Transfer Pak library.
 *
 * Public operations use the trpak_* namespace declared in libtrpak.h.
 *
 * @section impl_layers Layering
 *
 * The file is organized bottom-up:
 *
 * 1. **Default backend** (`platform_*`): thin wrappers over libdragon's Joybus
 *    accessory API and, optionally, EverDrive 64 DMA. Compiled out entirely
 *    with `TRPAK_NO_DEFAULT_IO`, which is how the host test suite builds it.
 * 2. **Transport** (`transport_read`, `transport_write`, `write_filled`): the
 *    only places that touch the installed ::trpak_io callbacks, translating
 *    their non-zero failures into ::TRPAK_ERR_IO.
 * 3. **Accessory control**: power, access mode, status polling.
 * 4. **Header decoding**: pure functions over a byte buffer, testable without
 *    hardware.
 * 5. **Banking**: per-mapper ROM/RAM register sequences.
 * 6. **Bulk operations**: `*_internal` loops shared by the buffer-based and
 *    DMA-based public entry points, plus init/shutdown.
 *
 * @section impl_windows Window arithmetic
 *
 * Nothing here addresses the Game Boy bus directly. Writes go to a 16 KiB
 * window at Transfer Pak `0xC000`-`0xFFFF` whose contents are chosen by the
 * bank register at `0xA000`, so the Game Boy address actually touched is
 * `slice * 0x4000 + (tp_address - 0xC000)`. That single formula explains every
 * magic address below:
 *
 * | Slice | Transfer Pak address | Game Boy address | Register hit          |
 * | ----- | -------------------- | ---------------- | --------------------- |
 * | `0`   | `0xC000`             | `0x0000`         | RAM enable            |
 * | `0`   | `0xE000`             | `0x2000`         | ROM bank (MBC5 low 8) |
 * | `0`   | `0xE100`             | `0x2100`         | ROM bank (MBC2 needs bit 8 of the address set) |
 * | `0`   | `0xF000`             | `0x3000`         | ROM bank bit 8 (MBC5) |
 * | `1`   | `0xC000`             | `0x4000`         | RAM bank / upper ROM bits |
 * | `1`   | `0xE000`             | `0x6000`         | MBC1 banking mode     |
 * | `1`   | `0xE016`             | `0x6016`         | MBC1 banking mode     |
 * | `2`   | `0xE000`-`0xFFFF`    | `0xA000`-`0xBFFF`| Cartridge save RAM    |
 *
 * @section impl_state State
 *
 * Two globals hold everything: the file-local ::runtime (backend, port, DMA
 * base) and the public ::trcart (cartridge metadata). The library therefore
 * drives a single Transfer Pak and is neither thread-safe nor reentrant.
 */

#include "libtrpak.h"

#include <string.h>

#ifndef TRPAK_NO_DEFAULT_IO
#include <libdragon.h>
#endif

#ifdef TRPAK_ENABLE_ED64_DMA
#include "sys.h"
#endif

/** Power control register; write `0x84` to switch the cartridge on. */
#define TP_POWER_ADDRESS       0x8000u
/** Bank register selecting which 16 KiB Game Boy slice the window shows. */
#define TP_REGISTER_ADDRESS    0xA000u
/** Access-mode control on write, status bitfield on read. */
#define TP_ACCESS_ADDRESS      0xB000u
/** First address of the 16 KiB data window. */
#define TP_ROM_WINDOW_START    0xC000u
/** Where cartridge RAM appears once slice 2 is selected. */
#define TP_RAM_WINDOW_START    0xE000u
/** Address of the last complete 32-byte block in the window. */
#define TP_WINDOW_END          0xFFE0u
/** Readiness poll budget; with the delay below this is a 500 ms timeout. */
#define TP_READY_POLL_ATTEMPTS 50u
/** Pause between readiness polls, in milliseconds. */
#define TP_READY_POLL_DELAY_MS 10u

/**
 * @brief Backend and addressing state shared by every operation.
 */
typedef struct trpak_runtime {
    trpak_io io;        /**< Installed transport callbacks. */
    int controller;     /**< Controller port index passed to the callbacks. */
    uintptr_t dma_base; /**< Base address used by the DMA helpers. */
    bool configured;    /**< False until a usable backend is installed. */
} trpak_runtime;

/** Single global runtime; zero-initialized, so `configured` starts false. */
static trpak_runtime runtime;

/** Scratch block reused by trpak_init() while assembling the header. */
static uint8_t transfer_data[TRPAK_TRANSFER_BLOCK_SIZE];

/** Metadata of the cartridge described by the last successful trpak_init(). */
trpak_cart trcart;

#ifndef TRPAK_NO_DEFAULT_IO
/**
 * @brief Default read primitive: libdragon's Joybus accessory read.
 *
 * The modern Joybus API is used deliberately; the deprecated
 * `read_mempak_address()` helper is not.
 */
static int platform_read_block(
    void *user,
    int controller,
    uint16_t address,
    uint8_t data[TRPAK_TRANSFER_BLOCK_SIZE]
)
{
    (void)user;
    return joybus_accessory_read(controller, address, data);
}

/**
 * @brief Default write primitive: libdragon's Joybus accessory write.
 */
static int platform_write_block(
    void *user,
    int controller,
    uint16_t address,
    const uint8_t data[TRPAK_TRANSFER_BLOCK_SIZE]
)
{
    (void)user;
    return joybus_accessory_write(controller, address, data);
}

/**
 * @brief Default delay primitive: libdragon's busy wait.
 */
static void platform_delay(void *user, unsigned int milliseconds)
{
    (void)user;
    wait_ms(milliseconds);
}

#ifdef TRPAK_ENABLE_ED64_DMA
/**
 * @brief Copies a freshly read block into EverDrive 64 SDRAM.
 *
 * The cache is written back and invalidated before the transfer so the DMA
 * engine observes the bytes the CPU just wrote instead of stale memory.
 */
static int platform_dma_store(
    void *user,
    const uint8_t *source,
    uintptr_t destination,
    size_t size
)
{
    (void)user;
    data_cache_hit_writeback_invalidate((void *)source, size);
    dma_write_s((void *)source, (unsigned long)destination, size);
    return 0;
}

/**
 * @brief Reads a block back out of EverDrive 64 SDRAM.
 *
 * The cache is invalidated on both sides of the transfer: before, so no dirty
 * line is flushed over the incoming data; after, so the CPU sees what DMA
 * actually deposited.
 */
static int platform_dma_load(
    void *user,
    uint8_t *destination,
    uintptr_t source,
    size_t size
)
{
    (void)user;
    data_cache_hit_writeback_invalidate(destination, size);
    dma_read_s(destination, (unsigned long)source, size);
    data_cache_hit_writeback_invalidate(destination, size);
    return 0;
}
#endif
#endif

/**
 * @copydoc trpak_use_default_io
 *
 * Resetting the whole structure also clears any previously installed custom
 * backend, its user pointer, and its DMA base.
 */
void trpak_use_default_io(void)
{
    memset(&runtime, 0, sizeof(runtime));
    runtime.controller = 0;
    runtime.dma_base = TRPAK_DEFAULT_DMA_BASE;

#ifndef TRPAK_NO_DEFAULT_IO
    runtime.io.read_block = platform_read_block;
    runtime.io.write_block = platform_write_block;
    runtime.io.delay = platform_delay;
#ifdef TRPAK_ENABLE_ED64_DMA
    runtime.io.dma_store = platform_dma_store;
    runtime.io.dma_load = platform_dma_load;
#endif
    /* Only claim to be configured when a real backend was compiled in;
     * otherwise every transfer must fail until the application installs one. */
    runtime.configured = true;
#endif
}

/**
 * @brief Guarantees a usable backend before a transfer is attempted.
 *
 * Installs the default backend on first use, then re-checks that the mandatory
 * callbacks exist — which they will not when the library was built with
 * `TRPAK_NO_DEFAULT_IO` and nothing was configured.
 *
 * @retval TRPAK_OK     A backend with read and write callbacks is installed.
 * @retval TRPAK_ERR_IO No usable backend.
 */
static int ensure_io(void)
{
    if (!runtime.configured) {
        trpak_use_default_io();
    }

    if (!runtime.configured || runtime.io.read_block == NULL ||
        runtime.io.write_block == NULL) {
        return TRPAK_ERR_IO;
    }

    return TRPAK_OK;
}

/** @copydoc trpak_configure_io */
int trpak_configure_io(const trpak_io *io, int controller, uintptr_t dma_base)
{
    if (io == NULL || io->read_block == NULL || io->write_block == NULL ||
        controller < 0 || controller > 3) {
        return TRPAK_ERR_INVALID_ARGUMENT;
    }

    /* Copied by value so the caller's structure need not outlive this call. */
    runtime.io = *io;
    runtime.controller = controller;
    runtime.dma_base = dma_base;
    runtime.configured = true;
    return TRPAK_OK;
}

/**
 * @brief Reads one block through the installed backend.
 *
 * Central choke point for reads: it validates the destination, lazily installs
 * a backend, and collapses any backend failure into ::TRPAK_ERR_IO.
 *
 * @param address Transfer Pak address.
 * @param data    Destination for ::TRPAK_TRANSFER_BLOCK_SIZE bytes.
 * @return ::TRPAK_OK or ::TRPAK_ERR_IO.
 */
static int transport_read(uint16_t address, uint8_t *data)
{
    if (data == NULL || ensure_io() != TRPAK_OK) {
        return TRPAK_ERR_IO;
    }

    if (runtime.io.read_block(
            runtime.io.user, runtime.controller, address, data) != 0) {
        return TRPAK_ERR_IO;
    }

    return TRPAK_OK;
}

/**
 * @brief Writes one block through the installed backend.
 *
 * @param address Transfer Pak address.
 * @param data    Source of ::TRPAK_TRANSFER_BLOCK_SIZE bytes.
 * @return ::TRPAK_OK or ::TRPAK_ERR_IO.
 */
static int transport_write(uint16_t address, const uint8_t *data)
{
    if (data == NULL || ensure_io() != TRPAK_OK) {
        return TRPAK_ERR_IO;
    }

    if (runtime.io.write_block(
            runtime.io.user, runtime.controller, address, data) != 0) {
        return TRPAK_ERR_IO;
    }

    return TRPAK_OK;
}

/**
 * @brief Writes a whole block filled with one repeated byte.
 *
 * The Transfer Pak has no narrower write than 32 bytes, so every single-byte
 * register poke — power, bank register, access mode, and every MBC register —
 * is expressed as a block of identical bytes. The cartridge decodes the value
 * from the byte that lands on the target address, and the surrounding copies
 * fall on mirrored register addresses that decode identically.
 *
 * @param address Transfer Pak address.
 * @param value   Byte replicated across the block.
 * @return ::TRPAK_OK or ::TRPAK_ERR_IO.
 */
static int write_filled(uint16_t address, uint8_t value)
{
    uint8_t data[TRPAK_TRANSFER_BLOCK_SIZE];
    memset(data, value, sizeof(data));
    return transport_write(address, data);
}

/**
 * @copydoc trpak_set_power
 *
 * `0x84` is the documented enable pattern; `0xFE` is simply a value that is
 * not `0x84`, which the accessory treats as power off. The 200 ms pause covers
 * the cartridge's own power-up ramp before any register is trusted.
 */
int trpak_set_power(bool enabled)
{
    int result = write_filled(TP_POWER_ADDRESS, enabled ? 0x84u : 0xFEu);

    if (result == TRPAK_OK && runtime.io.delay != NULL) {
        runtime.io.delay(runtime.io.user, 200u);
    }

    return result;
}

/**
 * @copydoc trpak_get_power
 *
 * The buffer is pre-filled with `0xFF` so a backend that silently returns
 * success without writing anything cannot be mistaken for either valid state.
 */
int trpak_get_power(bool *enabled)
{
    uint8_t data[TRPAK_TRANSFER_BLOCK_SIZE];
    int result;

    if (enabled == NULL) {
        return TRPAK_ERR_INVALID_ARGUMENT;
    }

    memset(data, 0xFF, sizeof(data));
    result = transport_read(TP_POWER_ADDRESS, data);
    if (result != TRPAK_OK) {
        return result;
    }

    if (data[0] == 0x00u) {
        *enabled = false;
    } else if (data[0] == 0x84u) {
        *enabled = true;
    } else {
        /* Anything else means the accessory is not answering as expected. */
        return TRPAK_ERR_POWER_STATE;
    }

    return TRPAK_OK;
}

/** @copydoc trpak_set_access_state */
int trpak_set_access_state(bool enabled)
{
    return write_filled(TP_ACCESS_ADDRESS, enabled ? 0x01u : 0x00u);
}

/** @copydoc trpak_get_status */
int trpak_get_status(uint8_t *status)
{
    uint8_t data[TRPAK_TRANSFER_BLOCK_SIZE];
    int result;

    if (status == NULL) {
        return TRPAK_ERR_INVALID_ARGUMENT;
    }

    memset(data, 0xFF, sizeof(data));
    result = transport_read(TP_ACCESS_ADDRESS, data);
    if (result != TRPAK_OK) {
        return result;
    }

    *status = data[0];
    return TRPAK_OK;
}

/**
 * @copydoc trpak_get_access_state
 *
 * Order matters: removal is reported even when other bits are also set,
 * because acting on a stale ready bit after a cartridge was pulled out is the
 * dangerous case.
 */
int trpak_get_access_state(int *state)
{
    uint8_t status;
    int result;

    if (state == NULL) {
        return TRPAK_ERR_INVALID_ARGUMENT;
    }

    result = trpak_get_status(&status);
    if (result != TRPAK_OK) {
        return result;
    }

    if ((status & TRPAK_STATUS_REMOVED) != 0u) {
        *state = 3;
    } else if ((status & TRPAK_STATUS_IS_RESETTING) != 0u) {
        *state = 2;
    } else if ((status & TRPAK_STATUS_READY) != 0u) {
        *state = 1;
    } else {
        *state = 0;
    }

    return TRPAK_OK;
}

/**
 * @brief Verifies the accessory is still safe to talk to.
 *
 * Called before every block of every bulk operation, which is what lets a dump
 * or restore abort as soon as the cartridge is pulled out instead of writing
 * garbage.
 *
 * @retval TRPAK_OK               Powered, present, ready, not resetting.
 * @retval TRPAK_ERR_NO_CARTRIDGE Removal bit set.
 * @retval TRPAK_ERR_POWER_OFF    Power bit cleared.
 * @retval TRPAK_ERR_ACCESS_STATE Not ready yet, or a reset is in progress.
 * @retval TRPAK_ERR_IO           Status read failed.
 */
static int check_cartridge_ready(void)
{
    uint8_t status;
    int result = trpak_get_status(&status);

    if (result != TRPAK_OK) {
        return result;
    }
    if ((status & TRPAK_STATUS_REMOVED) != 0u) {
        return TRPAK_ERR_NO_CARTRIDGE;
    }
    if ((status & TRPAK_STATUS_POWERED) == 0u) {
        return TRPAK_ERR_POWER_OFF;
    }
    if ((status & TRPAK_STATUS_READY) == 0u ||
        (status & TRPAK_STATUS_IS_RESETTING) != 0u) {
        return TRPAK_ERR_ACCESS_STATE;
    }

    return TRPAK_OK;
}

/**
 * @brief Polls until the cartridge is ready, or the budget runs out.
 *
 * Only transient conditions are retried. Success, an absent cartridge, and an
 * I/O failure all return immediately, since none of them improve by waiting;
 * "not ready yet" and "resetting" are the states worth polling through.
 *
 * @retval TRPAK_OK                   Cartridge became ready.
 * @retval TRPAK_ERR_NO_CARTRIDGE     No cartridge inserted.
 * @retval TRPAK_ERR_IO               Status read failed.
 * @retval TRPAK_ERR_TRANSFER_TIMEOUT Still not ready after ~500 ms.
 */
static int wait_for_cartridge_ready(void)
{
    unsigned int attempt;
    int result;

    for (attempt = 0u; attempt < TP_READY_POLL_ATTEMPTS; attempt++) {
        result = check_cartridge_ready();
        if (result == TRPAK_OK || result == TRPAK_ERR_NO_CARTRIDGE ||
            result == TRPAK_ERR_IO) {
            return result;
        }
        if (runtime.io.delay != NULL) {
            runtime.io.delay(runtime.io.user, TP_READY_POLL_DELAY_MS);
        }
    }

    return TRPAK_ERR_TRANSFER_TIMEOUT;
}

/**
 * @brief Translates the ROM size code at Game Boy `0x0148` into a bank count.
 *
 * Codes `0x00`-`0x08` are the regular powers of two from 32 KiB to 8 MiB.
 * Codes `0x52`-`0x54` are the rare non-power-of-two sizes (1.1, 1.2 and
 * 1.5 MiB). Anything else is rejected instead of guessed.
 *
 * @param code  Raw size code.
 * @param banks Receives the number of 16 KiB banks.
 * @retval TRPAK_OK                   Code recognized.
 * @retval TRPAK_ERR_INVALID_ARGUMENT `banks` is NULL.
 * @retval TRPAK_ERR_INVALID_HEADER   Unknown size code.
 */
static int decode_rom_size(uint8_t code, uint16_t *banks)
{
    if (banks == NULL) {
        return TRPAK_ERR_INVALID_ARGUMENT;
    }

    switch (code) {
    case 0x00u: *banks = 2u; break;    /*  32 KiB */
    case 0x01u: *banks = 4u; break;    /*  64 KiB */
    case 0x02u: *banks = 8u; break;    /* 128 KiB */
    case 0x03u: *banks = 16u; break;   /* 256 KiB */
    case 0x04u: *banks = 32u; break;   /* 512 KiB */
    case 0x05u: *banks = 64u; break;   /*   1 MiB */
    case 0x06u: *banks = 128u; break;  /*   2 MiB */
    case 0x07u: *banks = 256u; break;  /*   4 MiB */
    case 0x08u: *banks = 512u; break;  /*   8 MiB */
    case 0x52u: *banks = 72u; break;   /* 1.1 MiB */
    case 0x53u: *banks = 80u; break;   /* 1.2 MiB */
    case 0x54u: *banks = 96u; break;   /* 1.5 MiB */
    default: return TRPAK_ERR_INVALID_HEADER;
    }

    return TRPAK_OK;
}

/**
 * @brief Expands the cartridge type byte at Game Boy `0x0147`.
 *
 * Sets trpak_cart::mapper plus the RAM, battery, RTC, and rumble flags. The
 * raw byte is preserved in trpak_cart::cartridge_type. Types are decoded even
 * for mappers with no banking implementation (MMM01, MBC4); trpak_init()
 * rejects those later, so callers can still inspect what was inserted.
 *
 * @param type Raw cartridge type byte.
 * @param out  Metadata structure being filled.
 * @retval TRPAK_OK                        Type recognized.
 * @retval TRPAK_ERR_UNSUPPORTED_CARTRIDGE Unknown type byte.
 */
static int decode_cartridge_type(uint8_t type, trpak_cart *out)
{
    out->cartridge_type = type;

    switch (type) {
    case 0x00u: /* ROM ONLY */
        out->mapper = TRPAK_MAPPER_NONE;
        break;
    case 0x01u: /* MBC1 */
        out->mapper = TRPAK_MAPPER_MBC1;
        break;
    case 0x02u: /* MBC1 + RAM */
        out->mapper = TRPAK_MAPPER_MBC1;
        out->ram = true;
        break;
    case 0x03u: /* MBC1 + RAM + BATTERY */
        out->mapper = TRPAK_MAPPER_MBC1;
        out->ram = true;
        out->battery = true;
        break;
    case 0x05u: /* MBC2 (RAM is internal to the mapper) */
        out->mapper = TRPAK_MAPPER_MBC2;
        out->ram = true;
        break;
    case 0x06u: /* MBC2 + BATTERY */
        out->mapper = TRPAK_MAPPER_MBC2;
        out->ram = true;
        out->battery = true;
        break;
    case 0x08u: /* ROM + RAM */
        out->mapper = TRPAK_MAPPER_NONE;
        out->ram = true;
        break;
    case 0x09u: /* ROM + RAM + BATTERY */
        out->mapper = TRPAK_MAPPER_NONE;
        out->ram = true;
        out->battery = true;
        break;
    case 0x0Bu: /* MMM01 */
        out->mapper = TRPAK_MAPPER_MMM01;
        break;
    case 0x0Cu: /* MMM01 + RAM */
        out->mapper = TRPAK_MAPPER_MMM01;
        out->ram = true;
        break;
    case 0x0Du: /* MMM01 + RAM + BATTERY */
        out->mapper = TRPAK_MAPPER_MMM01;
        out->ram = true;
        out->battery = true;
        break;
    case 0x0Fu: /* MBC3 + TIMER + BATTERY */
        out->mapper = TRPAK_MAPPER_MBC3;
        out->battery = true;
        out->rtc = true;
        break;
    case 0x10u: /* MBC3 + TIMER + RAM + BATTERY */
        out->mapper = TRPAK_MAPPER_MBC3;
        out->ram = true;
        out->battery = true;
        out->rtc = true;
        break;
    case 0x11u: /* MBC3 */
        out->mapper = TRPAK_MAPPER_MBC3;
        break;
    case 0x12u: /* MBC3 + RAM */
        out->mapper = TRPAK_MAPPER_MBC3;
        out->ram = true;
        break;
    case 0x13u: /* MBC3 + RAM + BATTERY */
        out->mapper = TRPAK_MAPPER_MBC3;
        out->ram = true;
        out->battery = true;
        break;
    case 0x15u: /* MBC4 */
        out->mapper = TRPAK_MAPPER_MBC4;
        break;
    case 0x16u: /* MBC4 + RAM */
        out->mapper = TRPAK_MAPPER_MBC4;
        out->ram = true;
        break;
    case 0x17u: /* MBC4 + RAM + BATTERY */
        out->mapper = TRPAK_MAPPER_MBC4;
        out->ram = true;
        out->battery = true;
        break;
    case 0x19u: /* MBC5 */
        out->mapper = TRPAK_MAPPER_MBC5;
        break;
    case 0x1Au: /* MBC5 + RAM */
        out->mapper = TRPAK_MAPPER_MBC5;
        out->ram = true;
        break;
    case 0x1Bu: /* MBC5 + RAM + BATTERY */
        out->mapper = TRPAK_MAPPER_MBC5;
        out->ram = true;
        out->battery = true;
        break;
    case 0x1Cu: /* MBC5 + RUMBLE */
        out->mapper = TRPAK_MAPPER_MBC5;
        out->rumble = true;
        break;
    case 0x1Du: /* MBC5 + RUMBLE + RAM */
        out->mapper = TRPAK_MAPPER_MBC5;
        out->ram = true;
        out->rumble = true;
        break;
    case 0x1Eu: /* MBC5 + RUMBLE + RAM + BATTERY */
        out->mapper = TRPAK_MAPPER_MBC5;
        out->ram = true;
        out->battery = true;
        out->rumble = true;
        break;
    case 0xFCu: /* POCKET CAMERA */
        out->mapper = TRPAK_MAPPER_CAMERA;
        out->ram = true;
        out->battery = true;
        break;
    case 0xFFu: /* HuC1 + RAM + BATTERY */
        out->mapper = TRPAK_MAPPER_HUC1;
        out->ram = true;
        out->battery = true;
        break;
    default:
        return TRPAK_ERR_UNSUPPORTED_CARTRIDGE;
    }

    return TRPAK_OK;
}

/**
 * @brief Derives RAM size and bank count from the header and mapper.
 *
 * Runs after decode_cartridge_type(), because two decisions depend on it: a
 * cartridge whose type declares no RAM gets zeroed sizes regardless of the
 * size code, and MBC2 ignores the code entirely — its RAM is 512 half-bytes
 * built into the mapper.
 *
 * @param out Metadata structure with trpak_cart::ram, ::mapper and ::_ramsize
 *            already set.
 * @retval TRPAK_OK                 Sizes decoded.
 * @retval TRPAK_ERR_INVALID_HEADER Unknown RAM size code on a cartridge that
 *                                  claims to have RAM.
 */
static int decode_ram_size(trpak_cart *out)
{
    if (!out->ram) {
        out->rambanks = 0u;
        out->ramsize = 0u;
        return TRPAK_OK;
    }

    if (out->mapper == TRPAK_MAPPER_MBC2) {
        /* 512 x 4 bits, addressed as 512 bytes through the window. */
        out->rambanks = 1u;
        out->ramsize = 512u;
        return TRPAK_OK;
    }

    switch (out->_ramsize) {
    case 0x01u: /* 2 KiB: a partially populated single bank. */
        out->rambanks = 1u;
        out->ramsize = 2u * 1024u;
        break;
    case 0x02u: /* 8 KiB */
        out->rambanks = 1u;
        out->ramsize = 8u * 1024u;
        break;
    case 0x03u: /* 32 KiB */
        out->rambanks = 4u;
        out->ramsize = 32u * 1024u;
        break;
    case 0x04u: /* 128 KiB */
        out->rambanks = 16u;
        out->ramsize = 128u * 1024u;
        break;
    case 0x05u: /* 64 KiB, as used by MBC30. */
        out->rambanks = 8u;
        out->ramsize = 64u * 1024u;
        break;
    default:
        return TRPAK_ERR_INVALID_HEADER;
    }

    return TRPAK_OK;
}

/**
 * @copydoc trpak_check_header_checksum
 *
 * The Game Boy boot ROM computes `x = x - byte - 1` over the header bytes and
 * refuses to boot on mismatch, so this is a strong signal that the window is
 * really showing bank 0 of a real cartridge.
 */
bool trpak_check_header_checksum(
    const uint8_t header[TRPAK_HEADER_SIZE],
    size_t header_size
)
{
    uint8_t checksum = 0u;
    size_t i;

    if (header == NULL || header_size < TRPAK_HEADER_SIZE) {
        return false;
    }

    /* Buffer indices 0x34-0x4C are Game Boy addresses 0x0134-0x014C. */
    for (i = 0x34u; i <= 0x4Cu; i++) {
        checksum = (uint8_t)(checksum - header[i] - 1u);
    }

    return checksum == header[0x4Du];
}

/**
 * @copydoc trpak_parse_cartridge_header
 *
 * Decoding order is deliberate: checksum first, so no field is trusted from a
 * corrupt or absent header; then ROM size; then the cartridge type; and RAM
 * size last, since it depends on both the RAM flag and the mapper.
 */
int trpak_parse_cartridge_header(
    const uint8_t header[TRPAK_HEADER_SIZE],
    size_t header_size,
    trpak_cart *out
)
{
    size_t title_length;
    int result;

    if (header == NULL || out == NULL || header_size < TRPAK_HEADER_SIZE) {
        return TRPAK_ERR_INVALID_ARGUMENT;
    }
    if (!trpak_check_header_checksum(header, header_size)) {
        return TRPAK_ERR_INVALID_HEADER;
    }

    memset(out, 0, sizeof(*out));
    /* 0x0143: only 0x80 and 0xC0 are CGB markers; other values belong to the
     * title of older cartridges and must not be reported as a GBC flag. */
    out->gbc = (header[0x43] == 0x80u || header[0x43] == 0xC0u)
        ? header[0x43]
        : 0u;
    out->sgb = header[0x46];
    out->_romsize = header[0x48];
    out->_ramsize = header[0x49];

    /* CGB cartridges gave up the 16th title byte to the flag at 0x0143. */
    title_length = out->gbc != 0u ? 15u : 16u;
    memcpy(out->title, &header[0x34], title_length);
    out->title[title_length] = '\0';

    result = decode_rom_size(out->_romsize, &out->rombanks);
    if (result != TRPAK_OK) {
        return result;
    }
    out->romsize = (uint32_t)out->rombanks * TRPAK_ROM_BANK_SIZE;

    result = decode_cartridge_type(header[0x47], out);
    if (result != TRPAK_OK) {
        return result;
    }

    return decode_ram_size(out);
}

/** @copydoc trpak_mapper_is_supported */
bool trpak_mapper_is_supported(uint8_t mapper)
{
    switch (mapper) {
    case TRPAK_MAPPER_NONE:
    case TRPAK_MAPPER_MBC1:
    case TRPAK_MAPPER_MBC2:
    case TRPAK_MAPPER_MBC3:
    case TRPAK_MAPPER_MBC5:
    case TRPAK_MAPPER_CAMERA:
    case TRPAK_MAPPER_HUC1:
        return true;
    default:
        /* MMM01, MBC4, HuC3 and TAMA5 are detected but never driven. */
        return false;
    }
}

/**
 * @copydoc trpak_select_rom_bank
 *
 * Every mapper follows the same shape: point the window at slice 0 so the MBC
 * registers are writable, poke the bank number, then point it back at slice 1
 * so the switchable bank is what the data window exposes. Bank 0 is the
 * exception — it stays on slice 0, which is where the fixed bank lives.
 *
 * The window slice this function leaves behind is part of its contract: the
 * caller always reads the same Transfer Pak addresses (`0xC000`-`0xFFE0`) and
 * relies on this function to have pointed them at the requested bank. MBC1
 * uses that freedom to serve banks `0x20`/`0x40`/`0x60` through slice 0.
 */
int trpak_select_rom_bank(uint16_t bank)
{
    uint16_t original_bank = bank;
    int result;

    if (trcart.rombanks != 0u && bank >= trcart.rombanks) {
        return TRPAK_ERR_INVALID_BANK;
    }

    if (!trpak_mapper_is_supported(trcart.mapper)) {
        return TRPAK_ERR_UNSUPPORTED_CARTRIDGE;
    }

    /* Cartridges without an MBC, and bank 0 on mappers whose sequence below
     * does not special-case it, only need the window pointed at the right
     * slice: no MBC register write is involved. */
    if (trcart.mapper == TRPAK_MAPPER_NONE ||
        (bank == 0u && trcart.mapper != TRPAK_MAPPER_MBC1 &&
         trcart.mapper != TRPAK_MAPPER_HUC1 && trcart.mapper != TRPAK_MAPPER_MBC5)) {
        result = write_filled(TP_REGISTER_ADDRESS, (uint8_t)bank);
        if (result == TRPAK_OK) {
            trcart.bank = original_bank;
        }
        return result;
    }

    /* The chained conditions below compare against 0 rather than TRPAK_OK
     * because the two are the same value; the first failing write short
     * circuits the rest and is returned as-is. */
    switch (trcart.mapper) {
    case TRPAK_MAPPER_MBC1: {
        uint8_t lower = (uint8_t)(bank & 0x1Fu);        /* BANK1, GB 0x2000 */
        uint8_t upper = (uint8_t)((bank >> 5) & 0x03u); /* BANK2, GB 0x4000 */

        /* Two bits of BANK2 over five of BANK1: 128 banks, 2 MiB. */
        if (bank > 0x7Fu) {
            return TRPAK_ERR_INVALID_BANK;
        }

        if (lower == 0u) {
            /* BANK1 reads back as 1 when written as 0, so banks 0x00, 0x20,
             * 0x40 and 0x60 can never appear in the switchable region. In
             * advanced banking mode (1) BANK2 is applied to the fixed region
             * as well, which puts exactly those four banks at GB 0x0000, so
             * the window is left on slice 0 for the caller to read.
             *
             * Bank 0 is the one case that needs no mode change: it is what
             * the fixed region shows in simple mode (0), which is also the
             * cartridge's resting state and what the RAM path expects to
             * find undisturbed. */
            if ((result = write_filled(TP_REGISTER_ADDRESS, 0x01u)) != 0 ||
                (result = write_filled(
                     0xE016u, bank == 0u ? 0x00u : 0x01u)) != 0 ||
                (result = write_filled(0xC000u, upper)) != 0 ||
                (result = write_filled(TP_REGISTER_ADDRESS, 0x00u)) != 0) {
                return result;
            }
        } else {
            /* Simple mode: BANK2:BANK1 addresses the switchable region, so
             * the bank shows up on slice 1. Slice 1 reaches GB 0x6016 (mode)
             * and GB 0x4000 (BANK2); slice 0 reaches GB 0x2100 (BANK1). */
            if ((result = write_filled(TP_REGISTER_ADDRESS, 0x01u)) != 0 ||
                (result = write_filled(0xE016u, 0x00u)) != 0 ||
                (result = write_filled(0xC000u, upper)) != 0 ||
                (result = write_filled(TP_REGISTER_ADDRESS, 0x00u)) != 0 ||
                (result = write_filled(0xE100u, lower)) != 0 ||
                (result = write_filled(TP_REGISTER_ADDRESS, 0x01u)) != 0) {
                return result;
            }
        }
        break;
    }
    case TRPAK_MAPPER_HUC1:
        /* HuC1 uses a single six-bit ROM bank register at GB 0x2000. */
        if (bank > 0x3Fu) {
            return TRPAK_ERR_INVALID_BANK;
        }
        if (bank == 0u) {
            result = write_filled(TP_REGISTER_ADDRESS, 0x00u);
            if (result != TRPAK_OK) {
                return result;
            }
        } else if (
            (result = write_filled(TP_REGISTER_ADDRESS, 0x00u)) != 0 ||
            (result = write_filled(0xE000u, (uint8_t)bank)) != 0 ||
            (result = write_filled(TP_REGISTER_ADDRESS, 0x01u)) != 0) {
            return result;
        }
        break;
    case TRPAK_MAPPER_MBC2:
        /* MBC2 has 16 banks and only decodes the register when bit 8 of the
         * Game Boy address is set, hence GB 0x2100 rather than 0x2000. */
        if (bank > 0x0Fu) {
            return TRPAK_ERR_INVALID_BANK;
        }
        if ((result = write_filled(TP_REGISTER_ADDRESS, 0x00u)) != 0 ||
            (result = write_filled(0xE100u, (uint8_t)bank)) != 0 ||
            (result = write_filled(TP_REGISTER_ADDRESS, 0x01u)) != 0) {
            return result;
        }
        break;
    case TRPAK_MAPPER_MBC3:
        /* Seven-bit bank register at GB 0x2000-0x3FFF. */
        if (bank > 0x7Fu) {
            return TRPAK_ERR_INVALID_BANK;
        }
        if ((result = write_filled(TP_REGISTER_ADDRESS, 0x00u)) != 0 ||
            (result = write_filled(0xE100u, (uint8_t)bank)) != 0 ||
            (result = write_filled(TP_REGISTER_ADDRESS, 0x01u)) != 0) {
            return result;
        }
        break;
    case TRPAK_MAPPER_CAMERA:
        /* The camera mapper behaves like MBC3 for plain ROM banking. */
        if (bank > 0x3Fu) {
            return TRPAK_ERR_INVALID_BANK;
        }
        if ((result = write_filled(TP_REGISTER_ADDRESS, 0x00u)) != 0 ||
            (result = write_filled(0xE100u, (uint8_t)bank)) != 0 ||
            (result = write_filled(TP_REGISTER_ADDRESS, 0x01u)) != 0) {
            return result;
        }
        break;
    case TRPAK_MAPPER_MBC5: {
        uint8_t lower = (uint8_t)(bank & 0xFFu);        /* GB 0x2000 */
        uint8_t upper = (uint8_t)((bank >> 8) & 0x01u); /* GB 0x3000 */

        /* MBC5 splits nine bank bits across two registers and, unlike MBC1,
         * can select bank 0 into the switchable window. */
        if (bank > 0x1FFu) {
            return TRPAK_ERR_INVALID_BANK;
        }

        if ((result = write_filled(TP_REGISTER_ADDRESS, 0x00u)) != 0 ||
            (result = write_filled(0xE000u, lower)) != 0 ||
            (result = write_filled(0xF000u, upper)) != 0 ||
            (result = write_filled(TP_REGISTER_ADDRESS, 0x01u)) != 0) {
            return result;
        }
        break;
    }
    default:
        return TRPAK_ERR_UNSUPPORTED_CARTRIDGE;
    }

    trcart.bank = original_bank;
    return TRPAK_OK;
}

/**
 * @copydoc trpak_select_ram_bank
 *
 * Shape of the sequence: enable RAM through GB `0x0000` (slice 0), then write
 * the bank number to GB `0x4000` (slice 1). MBC1 additionally needs banking
 * mode 1 so that register means "RAM bank" instead of "upper ROM bits".
 */
int trpak_select_ram_bank(uint16_t bank)
{
    int result;

    if (!trcart.ram || trcart.rambanks == 0u) {
        return TRPAK_ERR_NO_RAM;
    }
    if (bank >= trcart.rambanks) {
        return TRPAK_ERR_INVALID_BANK;
    }

    /* Unbanked RAM on a cartridge without an MBC needs no register writes. */
    if (trcart.mapper == TRPAK_MAPPER_NONE) {
        return bank == 0u ? TRPAK_OK : TRPAK_ERR_INVALID_BANK;
    }
    if (!trpak_mapper_is_supported(trcart.mapper)) {
        return TRPAK_ERR_UNSUPPORTED_CARTRIDGE;
    }

    /* MBC1 and HuC1 only have a two-bit RAM bank register. */
    if ((trcart.mapper == TRPAK_MAPPER_MBC1 || trcart.mapper == TRPAK_MAPPER_HUC1) &&
        bank > 0x03u) {
        return TRPAK_ERR_INVALID_BANK;
    }

    /* Slice 0, GB 0x0000: 0x0A is the magic value that unlocks cartridge RAM. */
    if ((result = write_filled(TP_REGISTER_ADDRESS, 0x00u)) != 0 ||
        (result = write_filled(0xC000u, 0x0Au)) != 0) {
        return result;
    }

    /* MBC2's RAM is a single internal bank: enabling it is the whole job. */
    if (trcart.mapper == TRPAK_MAPPER_MBC2) {
        return TRPAK_OK;
    }
    if (trcart.mapper == TRPAK_MAPPER_MBC3 && bank > 0x07u) {
        /* Values above 7 address the RTC registers, not RAM. */
        return TRPAK_ERR_INVALID_BANK;
    }
    if (trcart.mapper == TRPAK_MAPPER_MBC5 && trcart.rumble && bank > 0x07u) {
        /* Bit 3 controls the motor on rumble cartridges. Never set it as
         * part of a RAM bank number. */
        return TRPAK_ERR_INVALID_BANK;
    }
    if ((trcart.mapper == TRPAK_MAPPER_MBC5 || trcart.mapper == TRPAK_MAPPER_CAMERA) &&
        bank > 0x0Fu) {
        return TRPAK_ERR_INVALID_BANK;
    }

    /* Slice 1, so the following write lands on GB 0x4000. */
    if ((result = write_filled(TP_REGISTER_ADDRESS, 0x01u)) != 0) {
        return result;
    }

    if (trcart.mapper == TRPAK_MAPPER_MBC1) {
        /* GB 0x6000: switch to banking mode 1 (RAM banking). */
        if ((result = write_filled(0xE000u, 0x01u)) != 0) {
            return result;
        }
    }

    return write_filled(0xC000u, (uint8_t)bank);
}

/**
 * @copydoc trpak_disable_ram
 *
 * Writing `0x00` to GB `0x0000`-`0x1FFF` latches the RAM again, which is what
 * protects a battery-backed save from stray writes once an operation ends.
 */
int trpak_disable_ram(void)
{
    int result;

    if (!trcart.ram || trcart.mapper == TRPAK_MAPPER_NONE) {
        return TRPAK_OK;
    }

    if (trcart.mapper == TRPAK_MAPPER_HUC1) {
        /* HuC1 cannot disable RAM. Any value other than 0x0E keeps its
         * A000-BFFF window in RAM mode rather than infrared mode.
         * Note: a failure of the first write is reported as TRPAK_ERR_IO
         * rather than the original code, which is always TRPAK_ERR_IO here
         * anyway since write_filled() has no other failure mode. */
        return write_filled(TP_REGISTER_ADDRESS, 0x00u) == TRPAK_OK
            ? write_filled(0xC000u, 0x00u)
            : TRPAK_ERR_IO;
    }

    result = write_filled(TP_REGISTER_ADDRESS, 0x00u);
    if (result != TRPAK_OK) {
        return result;
    }
    result = write_filled(0xC000u, 0x00u);
    if (result != TRPAK_OK || trcart.mapper != TRPAK_MAPPER_MBC1) {
        return result;
    }

    /* MBC1 shares one register between RAM banking (mode 1) and ROM banking
     * (mode 0), and trpak_select_ram_bank() had to switch it to mode 1. Left
     * that way, the fixed GB 0x0000-0x3FFF region keeps showing bank
     * BANK2 << 5 instead of bank 0, so the next bank-0 access — a header read
     * in trpak_init(), for instance — would silently return another bank. */
    result = write_filled(TP_REGISTER_ADDRESS, 0x01u);
    if (result != TRPAK_OK) {
        return result;
    }
    return write_filled(0xE016u, 0x00u);
}

/**
 * @brief Checks that a window address is in range and 32-byte aligned.
 *
 * The upper bound is ::TP_WINDOW_END, the last address at which a whole block
 * still fits inside the window. Alignment is tested with a mask because the
 * block size is a power of two.
 *
 * @param address Transfer Pak address to validate.
 * @param start   First legal address for this window (ROM or RAM).
 * @return `true` when the address may be used for a 32-byte transfer.
 */
static bool block_address_is_valid(uint16_t address, uint16_t start)
{
    return address >= start && address <= TP_WINDOW_END &&
        (address & (TRPAK_TRANSFER_BLOCK_SIZE - 1u)) == 0u;
}

/** @copydoc trpak_read_rom_block */
int trpak_read_rom_block(uint16_t address, uint8_t *data)
{
    if (data == NULL || !block_address_is_valid(address, TP_ROM_WINDOW_START)) {
        return TRPAK_ERR_INVALID_ARGUMENT;
    }
    return transport_read(address, data);
}

/**
 * @copydoc trpak_read_ram_block
 *
 * Selecting slice 2 on every call keeps the function self-contained: callers
 * can interleave ROM and RAM reads without tracking the window themselves.
 */
int trpak_read_ram_block(uint16_t address, uint8_t *data)
{
    int result;

    if (data == NULL || !block_address_is_valid(address, TP_RAM_WINDOW_START)) {
        return TRPAK_ERR_INVALID_ARGUMENT;
    }

    result = write_filled(TP_REGISTER_ADDRESS, 0x02u);
    if (result != TRPAK_OK) {
        return result;
    }

    /* Clear first so a backend that reports success without writing cannot
     * leave stale bytes from a previous block in the caller's buffer. */
    memset(data, 0, TRPAK_TRANSFER_BLOCK_SIZE);
    return transport_read(address, data);
}

/** @copydoc trpak_write_ram_block */
int trpak_write_ram_block(uint16_t address, const uint8_t *data)
{
    int result;

    if (data == NULL || !block_address_is_valid(address, TP_RAM_WINDOW_START)) {
        return TRPAK_ERR_INVALID_ARGUMENT;
    }

    result = write_filled(TP_REGISTER_ADDRESS, 0x02u);
    if (result != TRPAK_OK) {
        return result;
    }
    return transport_write(address, data);
}

/**
 * @brief Delivers one freshly read block to its destination.
 *
 * The single point where the buffer-based and DMA-based read paths diverge,
 * which is what lets read_rom_internal() and read_save_internal() serve both
 * public APIs unchanged.
 *
 * @param destination Caller buffer; unused (and typically NULL) in DMA mode.
 * @param capacity    Size of that buffer; unused in DMA mode.
 * @param offset      Running byte offset within the whole transfer.
 * @param block       Block just read from the cartridge.
 * @param use_dma     Route through trpak_io::dma_store instead of memcpy.
 * @retval TRPAK_OK                   Block stored.
 * @retval TRPAK_ERR_IO               DMA callback missing or failed.
 * @retval TRPAK_ERR_BUFFER_TOO_SMALL Block would not fit in the buffer.
 */
static int store_output(
    uint8_t *destination,
    size_t capacity,
    size_t offset,
    const uint8_t *block,
    bool use_dma
)
{
    if (use_dma) {
        if (runtime.io.dma_store == NULL) {
            return TRPAK_ERR_IO;
        }
        return runtime.io.dma_store(
            runtime.io.user,
            block,
            runtime.dma_base + offset,
            TRPAK_TRANSFER_BLOCK_SIZE
        ) == 0 ? TRPAK_OK : TRPAK_ERR_IO;
    }

    /* Re-checked per block, not just once up front, so a wrong capacity can
     * never turn into an overflow. */
    if (destination == NULL || offset + TRPAK_TRANSFER_BLOCK_SIZE > capacity) {
        return TRPAK_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(destination + offset, block, TRPAK_TRANSFER_BLOCK_SIZE);
    return TRPAK_OK;
}

/**
 * @brief Fetches the next block to be written to the cartridge.
 *
 * Mirror image of store_output() for the restore path.
 *
 * @param source  Caller buffer; unused (and typically NULL) in DMA mode.
 * @param size    Size of that buffer; unused in DMA mode.
 * @param offset  Running byte offset within the whole transfer.
 * @param block   Scratch block to fill.
 * @param use_dma Route through trpak_io::dma_load instead of memcpy.
 * @retval TRPAK_OK                   Block loaded.
 * @retval TRPAK_ERR_IO               DMA callback missing or failed.
 * @retval TRPAK_ERR_BUFFER_TOO_SMALL Block would read past the source buffer.
 */
static int load_input(
    const uint8_t *source,
    size_t size,
    size_t offset,
    uint8_t *block,
    bool use_dma
)
{
    if (use_dma) {
        if (runtime.io.dma_load == NULL) {
            return TRPAK_ERR_IO;
        }
        /* Poison the block so a callback that returns success without filling
         * it writes an obvious pattern instead of the previous block. */
        memset(block, 0xFF, TRPAK_TRANSFER_BLOCK_SIZE);
        return runtime.io.dma_load(
            runtime.io.user,
            block,
            runtime.dma_base + offset,
            TRPAK_TRANSFER_BLOCK_SIZE
        ) == 0 ? TRPAK_OK : TRPAK_ERR_IO;
    }

    if (source == NULL || offset + TRPAK_TRANSFER_BLOCK_SIZE > size) {
        return TRPAK_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(block, source + offset, TRPAK_TRANSFER_BLOCK_SIZE);
    return TRPAK_OK;
}

/**
 * @brief Shared ROM dump loop behind trpak_read_rom() and trpak_read_rom_dma().
 *
 * Two nested loops: banks on the outside, 32-byte blocks across the window on
 * the inside. Presence is re-checked before every single block, so a cartridge
 * pulled out mid-dump aborts with ::TRPAK_ERR_NO_CARTRIDGE instead of silently
 * recording garbage. Whatever happens, the window is put back on bank 0 before
 * returning, and `bytes_read` reports the bytes that really made it out.
 *
 * @param destination Caller buffer, or NULL in DMA mode.
 * @param capacity    Size of that buffer, or 0 in DMA mode.
 * @param bytes_read  Optional byte counter, valid even on partial failure.
 * @param use_dma     Select the DMA output path.
 * @return ::TRPAK_OK or the first error encountered.
 */
static int read_rom_internal(
    uint8_t *destination,
    size_t capacity,
    size_t *bytes_read,
    bool use_dma
)
{
    uint8_t block[TRPAK_TRANSFER_BLOCK_SIZE];
    size_t offset = 0u;
    uint16_t bank;
    int result = TRPAK_OK;

    if (!use_dma && (destination == NULL || capacity < trcart.romsize)) {
        return TRPAK_ERR_BUFFER_TOO_SMALL;
    }
    if (trcart.mapper == TRPAK_MAPPER_MBC1 && trcart.rombanks > 128u) {
        /* BANK2 is two bits wide, so MBC1 tops out at 128 banks. A header
         * claiming more would silently wrap and duplicate banks. */
        return TRPAK_ERR_UNSUPPORTED_CARTRIDGE;
    }
    if (trcart.mapper == TRPAK_MAPPER_HUC1 && trcart.rombanks > 64u) {
        /* The HuC1 bank register is six bits wide; anything larger would
         * silently wrap and produce a duplicated dump. */
        return TRPAK_ERR_UNSUPPORTED_CARTRIDGE;
    }
    if (ensure_io() != TRPAK_OK) {
        return TRPAK_ERR_IO;
    }

    for (bank = 0u; bank < trcart.rombanks; bank++) {
        uint32_t address;

        result = check_cartridge_ready();
        if (result != TRPAK_OK) {
            break;
        }
        result = trpak_select_rom_bank(bank);
        if (result != TRPAK_OK) {
            break;
        }

        /* The loop counter is 32-bit on purpose: a uint16_t would wrap at
         * TP_WINDOW_END + 32 == 0x10000 and never terminate. */
        for (address = TP_ROM_WINDOW_START;
             address <= TP_WINDOW_END;
             address += TRPAK_TRANSFER_BLOCK_SIZE) {
            result = check_cartridge_ready();
            if (result != TRPAK_OK) {
                break;
            }
            result = trpak_read_rom_block((uint16_t)address, block);
            if (result != TRPAK_OK) {
                break;
            }
            result = store_output(
                destination, capacity, offset, block, use_dma);
            if (result != TRPAK_OK) {
                break;
            }
            offset += TRPAK_TRANSFER_BLOCK_SIZE;
        }

        if (result != TRPAK_OK) {
            break;
        }
    }

    if (bytes_read != NULL) {
        *bytes_read = offset;
    }

    /* Leave the cartridge on bank 0. The cleanup error only surfaces when the
     * dump itself succeeded, so a real failure is never masked. */
    {
        int reset_result = trpak_select_rom_bank(0u);
        if (result == TRPAK_OK && reset_result != TRPAK_OK) {
            result = reset_result;
        }
    }
    return result;
}

/** @copydoc trpak_read_rom */
int trpak_read_rom(uint8_t *destination, size_t capacity, size_t *bytes_read)
{
    return read_rom_internal(destination, capacity, bytes_read, false);
}

/**
 * @brief Number of RAM bytes actually present in a given bank.
 *
 * Handles the partially populated cases: a 2 KiB cartridge occupies only a
 * quarter of its single bank, and MBC2 only 512 bytes, so the loops must not
 * read or write a full 8 KiB there.
 *
 * @param bank RAM bank index.
 * @return Byte count for that bank, or `0` when it lies past the RAM size.
 */
static size_t ram_bytes_for_bank(uint16_t bank)
{
    size_t offset = (size_t)bank * TRPAK_RAM_BANK_SIZE;
    size_t remaining;

    if (offset >= trcart.ramsize) {
        return 0u;
    }
    remaining = trcart.ramsize - offset;
    return remaining < TRPAK_RAM_BANK_SIZE ? remaining : TRPAK_RAM_BANK_SIZE;
}

/**
 * @brief Masks a block down to MBC2's 4-bit cells.
 *
 * MBC2 RAM stores nibbles; the upper bits read back as undefined. Masking both
 * on read and on write keeps backups reproducible and stops verification from
 * failing on bits the cartridge never stored.
 *
 * @param block Block modified in place.
 */
static void normalize_mbc2_block(uint8_t *block)
{
    size_t i;
    for (i = 0u; i < TRPAK_TRANSFER_BLOCK_SIZE; i++) {
        block[i] &= 0x0Fu;
    }
}

/**
 * @brief Disables RAM after a bulk operation, preserving the original error.
 *
 * Cleanup always runs, but a cleanup failure must not overwrite the error that
 * explains why the operation went wrong.
 *
 * @param operation_result Result of the operation that just finished.
 * @return `operation_result` when it is an error, otherwise the cleanup result.
 */
static int finish_ram_operation(int operation_result)
{
    int cleanup_result = trpak_disable_ram();
    return operation_result == TRPAK_OK ? cleanup_result : operation_result;
}

/**
 * @brief Shared save backup loop behind trpak_read_save() and
 *        trpak_read_save_dma().
 *
 * Same two-level structure as read_rom_internal(), but bounded by
 * ram_bytes_for_bank() so partially populated banks are not over-read, and
 * with MBC2 normalization applied to each block. RAM is always disabled again
 * through finish_ram_operation().
 *
 * @param destination Caller buffer, or NULL in DMA mode.
 * @param capacity    Size of that buffer, or 0 in DMA mode.
 * @param bytes_read  Optional byte counter, valid even on partial failure.
 * @param use_dma     Select the DMA output path.
 * @return ::TRPAK_OK or the first error encountered.
 */
static int read_save_internal(
    uint8_t *destination,
    size_t capacity,
    size_t *bytes_read,
    bool use_dma
)
{
    uint8_t block[TRPAK_TRANSFER_BLOCK_SIZE];
    size_t offset = 0u;
    uint16_t bank;
    int result = TRPAK_OK;

    if (!trcart.ram || trcart.ramsize == 0u) {
        return TRPAK_ERR_NO_RAM;
    }
    if (!use_dma && (destination == NULL || capacity < trcart.ramsize)) {
        return TRPAK_ERR_BUFFER_TOO_SMALL;
    }

    for (bank = 0u; bank < trcart.rambanks; bank++) {
        size_t bank_size = ram_bytes_for_bank(bank);
        size_t bank_offset;

        result = check_cartridge_ready();
        if (result != TRPAK_OK) {
            break;
        }
        result = trpak_select_ram_bank(bank);
        if (result != TRPAK_OK) {
            break;
        }

        for (bank_offset = 0u; bank_offset < bank_size;
             bank_offset += TRPAK_TRANSFER_BLOCK_SIZE) {
            uint16_t address = (uint16_t)(TP_RAM_WINDOW_START + bank_offset);
            result = check_cartridge_ready();
            if (result != TRPAK_OK) {
                break;
            }
            result = trpak_read_ram_block(address, block);
            if (result != TRPAK_OK) {
                break;
            }
            if (trcart.mapper == TRPAK_MAPPER_MBC2) {
                normalize_mbc2_block(block);
            }
            result = store_output(
                destination, capacity, offset, block, use_dma);
            if (result != TRPAK_OK) {
                break;
            }
            offset += TRPAK_TRANSFER_BLOCK_SIZE;
        }

        if (result != TRPAK_OK) {
            break;
        }
    }

    if (bytes_read != NULL) {
        *bytes_read = offset;
    }
    return finish_ram_operation(result);
}

/** @copydoc trpak_read_save */
int trpak_read_save(uint8_t *destination, size_t capacity, size_t *bytes_read)
{
    return read_save_internal(destination, capacity, bytes_read, false);
}

/**
 * @brief Shared save restore loop behind trpak_write_save() and
 *        trpak_write_save_dma().
 *
 * The size check is an equality, not a lower bound: a save that is not exactly
 * `trcart.ramsize` bytes is refused outright rather than written partially.
 * With verification enabled, each block is read back immediately after it is
 * written, so the operation stops at the first mismatch instead of at the end.
 *
 * @param source            Save image, or NULL in DMA mode.
 * @param size              Must equal `trcart.ramsize`.
 * @param verify_after_write Read back and compare every block.
 * @param use_dma           Select the DMA input path.
 * @return ::TRPAK_OK or the first error encountered.
 */
static int write_save_internal(
    const uint8_t *source,
    size_t size,
    bool verify_after_write,
    bool use_dma
)
{
    uint8_t block[TRPAK_TRANSFER_BLOCK_SIZE];
    uint8_t verification[TRPAK_TRANSFER_BLOCK_SIZE];
    size_t offset = 0u;
    uint16_t bank;
    int result = TRPAK_OK;

    if (!trcart.ram || trcart.ramsize == 0u) {
        return TRPAK_ERR_NO_RAM;
    }
    if (!use_dma && source == NULL) {
        return TRPAK_ERR_INVALID_ARGUMENT;
    }
    if (size != trcart.ramsize) {
        return TRPAK_ERR_INVALID_ARGUMENT;
    }

    for (bank = 0u; bank < trcart.rambanks; bank++) {
        size_t bank_size = ram_bytes_for_bank(bank);
        size_t bank_offset;

        result = check_cartridge_ready();
        if (result != TRPAK_OK) {
            break;
        }
        result = trpak_select_ram_bank(bank);
        if (result != TRPAK_OK) {
            break;
        }

        for (bank_offset = 0u; bank_offset < bank_size;
             bank_offset += TRPAK_TRANSFER_BLOCK_SIZE) {
            uint16_t address = (uint16_t)(TP_RAM_WINDOW_START + bank_offset);
            result = check_cartridge_ready();
            if (result != TRPAK_OK) {
                break;
            }
            result = load_input(source, size, offset, block, use_dma);
            if (result != TRPAK_OK) {
                break;
            }
            if (trcart.mapper == TRPAK_MAPPER_MBC2) {
                /* Mask the source too, so verification compares what the
                 * cartridge can actually store. */
                normalize_mbc2_block(block);
            }

            result = trpak_write_ram_block(address, block);
            if (result != TRPAK_OK) {
                break;
            }

            if (verify_after_write) {
                result = check_cartridge_ready();
                if (result != TRPAK_OK) {
                    break;
                }
                result = trpak_read_ram_block(address, verification);
                if (result != TRPAK_OK) {
                    break;
                }
                if (trcart.mapper == TRPAK_MAPPER_MBC2) {
                    normalize_mbc2_block(verification);
                }
                if (memcmp(block, verification, sizeof(block)) != 0) {
                    result = TRPAK_ERR_VERIFY_FAILED;
                    break;
                }
            }
            offset += TRPAK_TRANSFER_BLOCK_SIZE;
        }

        if (result != TRPAK_OK) {
            break;
        }
    }

    return finish_ram_operation(result);
}

/** @copydoc trpak_write_save */
int trpak_write_save(
    const uint8_t *source,
    size_t size,
    bool verify_after_write
)
{
    return write_save_internal(source, size, verify_after_write, false);
}

/** @copydoc trpak_read_rom_dma */
int trpak_read_rom_dma(size_t *bytes_read)
{
    return read_rom_internal(NULL, 0u, bytes_read, true);
}

/** @copydoc trpak_read_save_dma */
int trpak_read_save_dma(size_t *bytes_read)
{
    return read_save_internal(NULL, 0u, bytes_read, true);
}

/**
 * @copydoc trpak_write_save_dma
 *
 * The size is taken from ::trcart, since in DMA mode there is no caller buffer
 * whose length could be checked against it.
 */
int trpak_write_save_dma(bool verify_after_write)
{
    return write_save_internal(
        NULL,
        trcart.ramsize,
        verify_after_write,
        true
    );
}

/**
 * @brief Powers the accessory down after a failed bring-up.
 *
 * @param result Error that caused initialization to fail.
 * @return `result`, unchanged.
 */
static int fail_initialization(int result)
{
    /* Best-effort cleanup. Preserve the error that explains why init failed. */
    (void)trpak_disable_ram();
    (void)trpak_set_access_state(false);
    (void)trpak_set_power(false);
    return result;
}

/**
 * @copydoc trpak_init
 *
 * The bring-up deliberately starts by powering the accessory *off* and
 * confirming it: that guarantees a known state even when a previous run left
 * the Transfer Pak powered, and it proves the accessory answers reads before
 * anything else is attempted. Note that this very first failure path returns
 * directly, without cleanup, since nothing was powered on yet.
 *
 * The header is assembled from three window reads because a single transaction
 * only moves 32 bytes: `0xC100` and `0xC120` supply Game Boy `0x0100`-`0x013F`,
 * and the last read contributes only its first 16 bytes to reach `0x014F`.
 */
int trpak_init(void)
{
    uint8_t header[TRPAK_HEADER_SIZE];
    bool power;
    int result;

    if (ensure_io() != TRPAK_OK) {
        return TRPAK_ERR_IO;
    }

    memset(&trcart, 0, sizeof(trcart));
    memset(transfer_data, 0, sizeof(transfer_data));
    memset(header, 0, sizeof(header));

    /* Step 1: force a known-off state and verify the accessory answers. */
    result = trpak_set_power(false);
    if (result != TRPAK_OK) {
        return result;
    }
    result = trpak_get_power(&power);
    if (result != TRPAK_OK) {
        return result;
    }
    if (power) {
        return TRPAK_ERR_POWER_STATE;
    }

    /* Step 2: power up and confirm. From here on failures need cleanup. */
    result = trpak_set_power(true);
    if (result != TRPAK_OK) {
        return fail_initialization(result);
    }
    result = trpak_get_power(&power);
    if (result != TRPAK_OK) {
        return fail_initialization(result);
    }
    if (!power) {
        return fail_initialization(TRPAK_ERR_POWER_STATE);
    }

    /* Step 3: enable access mode and wait for the cartridge to settle. */
    result = trpak_set_access_state(true);
    if (result != TRPAK_OK) {
        return fail_initialization(result);
    }
    result = wait_for_cartridge_ready();
    if (result != TRPAK_OK) {
        return fail_initialization(result);
    }

    /* Step 4: the header lives in bank 0, so point the window there. This
     * runs before trcart is populated, which is why trpak_select_rom_bank()
     * tolerates a zero bank count and a zero mapper. */
    result = trpak_select_rom_bank(0u);
    if (result != TRPAK_OK) {
        return fail_initialization(result);
    }

    /* Step 5: read Game Boy 0x0100-0x014F in three window transactions. */
    result = trpak_read_rom_block(0xC100u, transfer_data);
    if (result != TRPAK_OK) {
        return fail_initialization(result);
    }
    memcpy(&header[0x00], transfer_data, TRPAK_TRANSFER_BLOCK_SIZE);

    result = trpak_read_rom_block(0xC120u, transfer_data);
    if (result != TRPAK_OK) {
        return fail_initialization(result);
    }
    memcpy(&header[0x20], transfer_data, TRPAK_TRANSFER_BLOCK_SIZE);

    result = trpak_read_rom_block(0xC140u, transfer_data);
    if (result != TRPAK_OK) {
        return fail_initialization(result);
    }
    /* Only 16 of the 32 bytes read belong to the header. */
    memcpy(&header[0x40], transfer_data, 16u);

    /* Step 6: decode, then refuse cartridges whose mapper has no banking
     * implementation rather than letting a later dump produce garbage. */
    result = trpak_parse_cartridge_header(header, sizeof(header), &trcart);
    if (result != TRPAK_OK) {
        return fail_initialization(result);
    }
    if (!trpak_mapper_is_supported(trcart.mapper)) {
        return fail_initialization(TRPAK_ERR_UNSUPPORTED_CARTRIDGE);
    }

    return TRPAK_OK;
}

/**
 * @copydoc trpak_shutdown
 *
 * All three steps run unconditionally, because stopping at the first failure
 * could leave the cartridge powered with RAM writable. The first error wins,
 * as it is the most likely explanation of what went wrong.
 */
int trpak_shutdown(void)
{
    int result = TRPAK_OK;
    int current;

    if (trcart.ram) {
        current = trpak_disable_ram();
        if (current != TRPAK_OK) {
            result = current;
        }
    }

    current = trpak_set_access_state(false);
    if (result == TRPAK_OK && current != TRPAK_OK) {
        result = current;
    }
    current = trpak_set_power(false);
    if (result == TRPAK_OK && current != TRPAK_OK) {
        result = current;
    }
    return result;
}

/** @copydoc trpak_error_string */
const char *trpak_error_string(int result)
{
    switch (result) {
    case TRPAK_OK: return "success";
    case TRPAK_ERR_IO: return "Transfer Pak I/O error";
    case TRPAK_ERR_POWER_STATE: return "unexpected Transfer Pak power state";
    case TRPAK_ERR_ACCESS_STATE: return "unexpected Transfer Pak access state";
    case TRPAK_ERR_POWER_OFF: return "Transfer Pak is not powered";
    case TRPAK_ERR_INVALID_ARGUMENT: return "invalid argument";
    case TRPAK_ERR_BUFFER_TOO_SMALL: return "buffer is too small";
    case TRPAK_ERR_UNSUPPORTED_CARTRIDGE: return "unsupported cartridge";
    case TRPAK_ERR_INVALID_BANK: return "invalid ROM or RAM bank";
    case TRPAK_ERR_NO_RAM: return "cartridge has no RAM";
    case TRPAK_ERR_VERIFY_FAILED: return "save verification failed";
    case TRPAK_ERR_INVALID_HEADER: return "invalid cartridge header";
    case TRPAK_ERR_NO_CARTRIDGE: return "no Game Boy cartridge detected";
    case TRPAK_ERR_TRANSFER_TIMEOUT: return "Transfer Pak readiness timeout";
    default: return "unknown libtrpak error";
    }
}
