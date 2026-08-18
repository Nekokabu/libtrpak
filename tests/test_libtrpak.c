/**
 * @file test_libtrpak.c
 * @brief Host test suite for libtrpak, run with `make test`.
 *
 * The library is compiled with `TRPAK_NO_DEFAULT_IO`, so no libdragon backend
 * exists and every accessory access must go through the mock installed with
 * trpak_configure_io(). That mock (::mock_transfer_pak) emulates enough of the
 * Transfer Pak to exercise the real code paths on a PC:
 *
 * - a power register whose read-back mirrors what was written;
 * - a status byte synthesized from power, access mode, and a removal flag;
 * - the bank register at `0xA000`, kept in mock_transfer_pak::window;
 * - a 32 KiB ROM image and an 8 KiB RAM image behind the data window;
 * - a block of memory standing in for flashcart SDRAM, for the DMA callbacks.
 *
 * The mock only implements the slices the library actually uses, notably
 * slice 2 for RAM; MBC register writes land in the catch-all branch of
 * mock_write() and are accepted without side effects, which is enough because
 * the test cartridge is a plain `ROM + RAM` type with no banking.
 *
 * Failures abort through `assert()`, so the suite is meant to be built without
 * `NDEBUG`. Exiting with status `0` and printing the final line means every
 * assertion held.
 */

#include "../libtrpak.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Simulated cartridge ROM: two 16 KiB banks. */
#define MOCK_ROM_SIZE (2u * TRPAK_ROM_BANK_SIZE)
/** Simulated cartridge save RAM: one 8 KiB bank. */
#define MOCK_RAM_SIZE TRPAK_RAM_BANK_SIZE
/** Base address the DMA callbacks translate into ::mock_transfer_pak::dma. */
#define MOCK_DMA_BASE ((uintptr_t)0x10000000u)

/**
 * @brief State of the simulated Transfer Pak and cartridge.
 */
typedef struct mock_transfer_pak {
    uint8_t rom[MOCK_ROM_SIZE]; /**< Cartridge ROM image. */
    uint8_t ram[MOCK_RAM_SIZE]; /**< Cartridge save RAM image. */
    uint8_t dma[MOCK_ROM_SIZE]; /**< Stand-in for flashcart SDRAM. */
    uint8_t power;              /**< Last value written to `0x8000`. */
    uint8_t access;             /**< Last value written to `0xB000`. */
    uint8_t window;             /**< Current slice from the `0xA000` register. */
    bool removed;               /**< Simulates yanking the cartridge out. */
    unsigned int delay_calls;   /**< Counts delay callback invocations. */
} mock_transfer_pak;

/** Single mock instance, passed to the callbacks as the user pointer. */
static mock_transfer_pak mock;

/**
 * @brief Builds the status byte the library reads from `0xB000`.
 *
 * Mirrors the real accessory closely enough for check_cartridge_ready():
 * removal masks readiness, and readiness requires both power and access mode.
 *
 * @param state Mock state to describe.
 * @return A combination of `TRPAK_STATUS_*` bits.
 */
static uint8_t mock_status(const mock_transfer_pak *state)
{
    uint8_t status = state->power == 0x84u ? TRPAK_STATUS_POWERED : 0u;

    if (state->removed) {
        status |= TRPAK_STATUS_REMOVED;
    } else if (state->access != 0u && state->power == 0x84u) {
        status |= TRPAK_STATUS_READY;
    }
    return status;
}

/**
 * @brief Mock implementation of ::trpak_read_block_fn.
 *
 * Decodes the control registers first, then the data window. The ROM branch
 * applies the same slice arithmetic as real hardware and returns a failure
 * when a read would fall outside the simulated cartridge, which is what makes
 * an out-of-range bank selection observable in the tests.
 */
static int mock_read(
    void *user,
    int controller,
    uint16_t address,
    uint8_t data[TRPAK_TRANSFER_BLOCK_SIZE]
)
{
    mock_transfer_pak *state = user;
    size_t offset;

    assert(controller == 0);
    memset(data, 0, TRPAK_TRANSFER_BLOCK_SIZE);

    if (address == 0x8000u) {
        data[0] = state->power;
        return 0;
    }
    if (address == 0xB000u) {
        data[0] = mock_status(state);
        return 0;
    }
    /* Slice 2 maps cartridge RAM at 0xE000-0xFFFF. */
    if (state->window == 0x02u && address >= 0xE000u) {
        offset = (size_t)(address - 0xE000u);
        memcpy(data, state->ram + offset, TRPAK_TRANSFER_BLOCK_SIZE);
        return 0;
    }
    if (address >= 0xC000u) {
        offset = (size_t)state->window * TRPAK_ROM_BANK_SIZE +
            (size_t)(address - 0xC000u);
        if (offset + TRPAK_TRANSFER_BLOCK_SIZE > sizeof(state->rom)) {
            return -1;
        }
        memcpy(data, state->rom + offset, TRPAK_TRANSFER_BLOCK_SIZE);
        return 0;
    }
    return -1;
}

/**
 * @brief Mock implementation of ::trpak_write_block_fn.
 *
 * Only the first byte of the block matters, matching how write_filled() pokes
 * single-byte registers. Writes that are neither a control register nor the
 * RAM window — that is, MBC register writes — are accepted and ignored.
 */
static int mock_write(
    void *user,
    int controller,
    uint16_t address,
    const uint8_t data[TRPAK_TRANSFER_BLOCK_SIZE]
)
{
    mock_transfer_pak *state = user;
    size_t offset;

    assert(controller == 0);
    if (address == 0x8000u) {
        /* Real hardware only recognizes 0x84 as "on"; anything else is off. */
        state->power = data[0] == 0x84u ? 0x84u : 0u;
        return 0;
    }
    if (address == 0xA000u) {
        state->window = data[0];
        return 0;
    }
    if (address == 0xB000u) {
        state->access = data[0];
        return 0;
    }
    if (state->window == 0x02u && address >= 0xE000u) {
        offset = (size_t)(address - 0xE000u);
        memcpy(state->ram + offset, data, TRPAK_TRANSFER_BLOCK_SIZE);
        return 0;
    }
    return 0;
}

/**
 * @brief Mock implementation of ::trpak_delay_fn; counts calls instead of
 *        sleeping.
 *
 * The assertion documents an invariant of the library: it never asks for a
 * zero-length delay.
 */
static void mock_delay(void *user, unsigned int milliseconds)
{
    mock_transfer_pak *state = user;
    assert(milliseconds > 0u);
    state->delay_calls++;
}

/**
 * @brief Mock implementation of ::trpak_dma_store_fn.
 *
 * Validates that the library always presents an absolute address at or above
 * the configured base, and that the running offset stays inside the simulated
 * SDRAM.
 */
static int mock_dma_store(
    void *user,
    const uint8_t *source,
    uintptr_t destination,
    size_t size
)
{
    mock_transfer_pak *state = user;
    size_t offset;

    if (destination < MOCK_DMA_BASE) {
        return -1;
    }
    offset = (size_t)(destination - MOCK_DMA_BASE);
    if (offset + size > sizeof(state->dma)) {
        return -1;
    }
    memcpy(state->dma + offset, source, size);
    return 0;
}

/**
 * @brief Mock implementation of ::trpak_dma_load_fn, the restore counterpart
 *        of mock_dma_store().
 */
static int mock_dma_load(
    void *user,
    uint8_t *destination,
    uintptr_t source,
    size_t size
)
{
    mock_transfer_pak *state = user;
    size_t offset;

    if (source < MOCK_DMA_BASE) {
        return -1;
    }
    offset = (size_t)(source - MOCK_DMA_BASE);
    if (offset + size > sizeof(state->dma)) {
        return -1;
    }
    memcpy(destination, state->dma + offset, size);
    return 0;
}

/**
 * @brief Writes the Game Boy header checksum into a synthetic header.
 *
 * Intentionally an independent reimplementation of the algorithm in
 * trpak_check_header_checksum(), so a change to either side shows up as a test
 * failure instead of being silently mirrored.
 *
 * @param header Buffer for Game Boy addresses `0x0100`-`0x014F`.
 */
static void finish_checksum(uint8_t header[TRPAK_HEADER_SIZE])
{
    uint8_t checksum = 0u;
    size_t i;

    for (i = 0x34u; i <= 0x4Cu; i++) {
        checksum = (uint8_t)(checksum - header[i] - 1u);
    }
    header[0x4Du] = checksum;
}

/**
 * @brief Builds a valid, minimal cartridge header.
 *
 * @param header         Buffer for Game Boy addresses `0x0100`-`0x014F`.
 * @param cartridge_type Value for Game Boy `0x0147`.
 * @param rom_size       Value for Game Boy `0x0148`.
 * @param ram_size       Value for Game Boy `0x0149`.
 */
static void create_header(
    uint8_t header[TRPAK_HEADER_SIZE],
    uint8_t cartridge_type,
    uint8_t rom_size,
    uint8_t ram_size
)
{
    memset(header, 0, TRPAK_HEADER_SIZE);
    memcpy(header + 0x34u, "LIBTRPAK TEST", 13u);
    header[0x47u] = cartridge_type;
    header[0x48u] = rom_size;
    header[0x49u] = ram_size;
    finish_checksum(header);
}

/**
 * @brief Installs the mock backend on port 0 with the test DMA base.
 */
static void configure_mock(void)
{
    trpak_io io;

    memset(&io, 0, sizeof(io));
    io.read_block = mock_read;
    io.write_block = mock_write;
    io.delay = mock_delay;
    io.dma_store = mock_dma_store;
    io.dma_load = mock_dma_load;
    io.user = &mock;
    assert(trpak_configure_io(&io, 0, MOCK_DMA_BASE) == TRPAK_OK);
}

/**
 * @brief Header decoding: sizes, checksum enforcement, argument validation.
 *
 * Uses an 8 MiB MBC5 cartridge with 128 KiB of RAM — the largest combination
 * the size decoders recognize — then flips a title bit to prove that a broken
 * checksum is rejected rather than parsed.
 */
static void test_header_parser(void)
{
    uint8_t header[TRPAK_HEADER_SIZE];
    trpak_cart parsed;

    create_header(header, 0x1Bu, 0x08u, 0x04u);
    assert(trpak_check_header_checksum(header, sizeof(header)));
    assert(trpak_parse_cartridge_header(
        header, sizeof(header), &parsed) == TRPAK_OK);
    assert(parsed.mapper == TRPAK_MAPPER_MBC5);
    assert(parsed.romsize == 8u * 1024u * 1024u);
    assert(parsed.ramsize == 128u * 1024u);
    assert(parsed.rombanks == 512u);
    assert(parsed.rambanks == 16u);

    header[0x34u] ^= 1u;
    assert(!trpak_check_header_checksum(header, sizeof(header)));
    assert(trpak_parse_cartridge_header(
        header, sizeof(header), &parsed) == TRPAK_ERR_INVALID_HEADER);
    assert(trpak_parse_cartridge_header(
        NULL, sizeof(header), &parsed) == TRPAK_ERR_INVALID_ARGUMENT);
    assert(trpak_parse_cartridge_header(
        header, sizeof(header), NULL) == TRPAK_ERR_INVALID_ARGUMENT);
}

/**
 * @brief Header decoding of the cases that used to be rejected outright.
 *
 * Three separate rules, none of which needs an accessory:
 *
 * - a type byte claiming RAM alongside a `0` size code decodes as RAM-less
 *   rather than failing, so such a cartridge stays dumpable;
 * - MBC2 ignores the size code entirely, since its RAM is part of the mapper;
 * - TAMA5 and HuC3 are decoded so a caller can report what is inserted, even
 *   though neither has a banking path.
 */
static void test_header_edge_cases(void)
{
    uint8_t header[TRPAK_HEADER_SIZE];
    trpak_cart parsed;

    create_header(header, 0x03u, 0x04u, 0x00u); /* MBC1 + RAM + BATTERY */
    assert(trpak_parse_cartridge_header(
        header, sizeof(header), &parsed) == TRPAK_OK);
    assert(parsed.mapper == TRPAK_MAPPER_MBC1);
    assert(parsed.ram == 0u);
    assert(parsed.rambanks == 0u);
    assert(parsed.ramsize == 0u);
    /* The type byte's claim stays visible even though the size code won. */
    assert(parsed.battery != 0u);
    assert(parsed.romsize == 512u * 1024u);

    create_header(header, 0x06u, 0x03u, 0x00u); /* MBC2 + BATTERY */
    assert(trpak_parse_cartridge_header(
        header, sizeof(header), &parsed) == TRPAK_OK);
    assert(parsed.mapper == TRPAK_MAPPER_MBC2);
    assert(parsed.ram != 0u);
    assert(parsed.ramsize == 512u);

    create_header(header, 0xFDu, 0x03u, 0x00u); /* BANDAI TAMA5 */
    assert(trpak_parse_cartridge_header(
        header, sizeof(header), &parsed) == TRPAK_OK);
    assert(parsed.mapper == TRPAK_MAPPER_TAMA5);
    assert(!trpak_mapper_is_supported(parsed.mapper));

    create_header(header, 0xFEu, 0x05u, 0x03u); /* HuC3 */
    assert(trpak_parse_cartridge_header(
        header, sizeof(header), &parsed) == TRPAK_OK);
    assert(parsed.mapper == TRPAK_MAPPER_HUC3);
    assert(parsed.rtc != 0u);
    assert(!trpak_mapper_is_supported(parsed.mapper));

    /* A genuinely unknown type byte is still refused. */
    create_header(header, 0x22u, 0x05u, 0x00u); /* MBC7 */
    assert(trpak_parse_cartridge_header(
        header, sizeof(header), &parsed) == TRPAK_ERR_UNSUPPORTED_CARTRIDGE);
}

/**
 * @brief trpak_configure_io() rejects incomplete backends and bad ports.
 *
 * Covers the three failure modes: no structure at all, a structure missing the
 * mandatory callbacks, and a controller index past port 4.
 */
static void test_configuration_validation(void)
{
    trpak_io io;

    memset(&io, 0, sizeof(io));
    assert(trpak_configure_io(NULL, 0, MOCK_DMA_BASE) ==
        TRPAK_ERR_INVALID_ARGUMENT);
    assert(trpak_configure_io(&io, 0, MOCK_DMA_BASE) ==
        TRPAK_ERR_INVALID_ARGUMENT);
    io.read_block = mock_read;
    io.write_block = mock_write;
    assert(trpak_configure_io(&io, 4, MOCK_DMA_BASE) ==
        TRPAK_ERR_INVALID_ARGUMENT);
}

/**
 * @brief End-to-end run over the mock: init, dumps, restore, removal, shutdown.
 *
 * The simulated cartridge is type `0x08` (ROM + RAM) with 32 KiB of ROM and
 * 8 KiB of RAM, so no MBC sequence is required and the byte-for-byte
 * comparisons below check the traversal itself. In order, the test asserts
 * that:
 *
 * - trpak_init() finds the cartridge and decodes the header from the mock ROM;
 * - the delay callback was used at least for the two power transitions;
 * - a buffer one byte too small is refused instead of partially filled;
 * - ROM and RAM copies match the mock images exactly;
 * - the DMA dump lands in SDRAM identically to the buffer dump;
 * - a verified DMA restore writes the save back byte for byte;
 * - setting the removal flag aborts a dump with ::TRPAK_ERR_NO_CARTRIDGE;
 * - trpak_shutdown() leaves the accessory unpowered and out of access mode.
 */
static void test_lifecycle_buffers_and_dma(void)
{
    uint8_t rom_copy[MOCK_ROM_SIZE];
    uint8_t save_copy[MOCK_RAM_SIZE];
    size_t bytes_read = 0u;
    size_t i;

    /* Fill ROM and RAM with distinct patterns so a mis-ordered or duplicated
     * block would break the comparisons below. */
    memset(&mock, 0, sizeof(mock));
    for (i = 0u; i < sizeof(mock.rom); i++) {
        mock.rom[i] = (uint8_t)i;
    }
    for (i = 0u; i < sizeof(mock.ram); i++) {
        mock.ram[i] = (uint8_t)(i ^ 0xA5u);
    }
    create_header(mock.rom + 0x100u, 0x08u, 0x00u, 0x02u);
    configure_mock();

    assert(trpak_init() == TRPAK_OK);
    assert(strcmp(trcart.title, "LIBTRPAK TEST") == 0);
    assert(trcart.romsize == MOCK_ROM_SIZE);
    assert(trcart.ramsize == MOCK_RAM_SIZE);
    assert(mock.delay_calls >= 2u);

    assert(trpak_read_rom(rom_copy, sizeof(rom_copy) - 1u, NULL) ==
        TRPAK_ERR_BUFFER_TOO_SMALL);
    assert(trpak_read_rom(rom_copy, sizeof(rom_copy), &bytes_read) == TRPAK_OK);
    assert(bytes_read == sizeof(mock.rom));
    assert(memcmp(rom_copy, mock.rom, sizeof(mock.rom)) == 0);

    assert(trpak_read_save(save_copy, sizeof(save_copy), &bytes_read) ==
        TRPAK_OK);
    assert(bytes_read == sizeof(mock.ram));
    assert(memcmp(save_copy, mock.ram, sizeof(mock.ram)) == 0);

    memset(mock.dma, 0, sizeof(mock.dma));
    assert(trpak_read_rom_dma(&bytes_read) == TRPAK_OK);
    assert(bytes_read == sizeof(mock.rom));
    assert(memcmp(mock.dma, mock.rom, sizeof(mock.rom)) == 0);

    /* Stage a different save image in SDRAM, restore it with verification,
     * and confirm the cartridge RAM now matches it. */
    for (i = 0u; i < sizeof(mock.ram); i++) {
        mock.dma[i] = (uint8_t)(i ^ 0x5Au);
    }
    assert(trpak_write_save_dma(true) == TRPAK_OK);
    assert(memcmp(mock.ram, mock.dma, sizeof(mock.ram)) == 0);

    mock.removed = true;
    assert(trpak_read_rom(rom_copy, sizeof(rom_copy), NULL) ==
        TRPAK_ERR_NO_CARTRIDGE);
    mock.removed = false;

    assert(trpak_shutdown() == TRPAK_OK);
    assert(mock.power == 0u);
    assert(mock.access == 0u);
}

/* ------------------------------------------------------------------------ */
/* Second mock: a faithful MBC1 cartridge                                    */
/* ------------------------------------------------------------------------ */

/** Full MBC1 address space: 128 banks of 16 KiB. */
#define MBC1_BANKS 128u
#define MBC1_ROM_SIZE ((size_t)MBC1_BANKS * TRPAK_ROM_BANK_SIZE)
/** Four RAM banks, i.e. header RAM size code 0x03. */
#define MBC1_RAM_BANKS 4u
#define MBC1_RAM_SIZE ((size_t)MBC1_RAM_BANKS * TRPAK_RAM_BANK_SIZE)

/**
 * @brief Simulated MBC1 cartridge, registers included.
 *
 * Unlike ::mock_transfer_pak, which ignores mapper writes, this mock decodes
 * the real MBC1 registers and resolves every read through them. That is what
 * makes the ROM dump test meaningful: a wrong banking sequence produces the
 * wrong bytes here exactly as it would on hardware.
 */
typedef struct mock_mbc1 {
    uint8_t rom[MBC1_ROM_SIZE]; /**< 2 MiB ROM image. */
    uint8_t ram[MBC1_RAM_SIZE]; /**< 32 KiB save RAM. */
    uint8_t power;              /**< Last value written to `0x8000`. */
    uint8_t access;             /**< Last value written to `0xB000`. */
    uint8_t window;             /**< Current slice from the `0xA000` register. */
    uint8_t bank1;              /**< GB `0x2000`-`0x3FFF`, five bits. */
    uint8_t bank2;              /**< GB `0x4000`-`0x5FFF`, two bits. */
    uint8_t mode;               /**< GB `0x6000`-`0x7FFF`, banking mode. */
    bool ram_enabled;           /**< Set by writing `0x0A` to GB `0x0000`. */
    unsigned int blocks_to_reset; /**< Window reads left before a simulated
                                       reset fires; `0` disables it. */
    bool was_reset;             /**< Pending `TRPAK_STATUS_WAS_RESET` latch. */
    unsigned int resets;        /**< How many simulated resets have fired. */
} mock_mbc1;

static mock_mbc1 mbc1;

/** @brief Status byte, same rules as mock_status() plus the reset latch. */
static uint8_t mbc1_status(const mock_mbc1 *state)
{
    uint8_t status = state->power == 0x84u ? TRPAK_STATUS_POWERED : 0u;

    if (state->access != 0u && state->power == 0x84u) {
        status |= TRPAK_STATUS_READY;
    }
    if (state->was_reset) {
        status |= TRPAK_STATUS_WAS_RESET;
    }
    return status;
}

/**
 * @brief Fires a simulated Transfer Pak reset once the countdown expires.
 *
 * A reset returns the mapper to its power-on state: the bank registers and the
 * banking mode go back to zero and cartridge RAM re-locks. The accessory stays
 * powered, present, and ready, so nothing but ::TRPAK_STATUS_WAS_RESET tells
 * the library that the bank it selected is no longer latched — which is
 * exactly the condition being tested.
 *
 * @param state Mock to disturb.
 */
static void mbc1_maybe_reset(mock_mbc1 *state)
{
    if (state->blocks_to_reset == 0u || --state->blocks_to_reset != 0u) {
        return;
    }
    state->bank1 = 0u;
    state->bank2 = 0u;
    state->mode = 0u;
    state->ram_enabled = false;
    state->was_reset = true;
    state->resets++;
}

/**
 * @brief Converts a Transfer Pak window address into a Game Boy address.
 *
 * The same `slice * 0x4000 + (address - 0xC000)` arithmetic the real accessory
 * performs.
 */
static uint16_t mbc1_gb_address(const mock_mbc1 *state, uint16_t address)
{
    return (uint16_t)((unsigned int)state->window * 0x4000u +
        (unsigned int)(address - 0xC000u));
}

/**
 * @brief Resolves a Game Boy ROM address to an offset in the image.
 *
 * The two rules that this whole test exists to exercise: BANK1 written as zero
 * behaves as one, and in advanced banking mode BANK2 also drives the otherwise
 * fixed `0x0000`-`0x3FFF` region.
 */
static size_t mbc1_rom_offset(const mock_mbc1 *state, uint16_t gb)
{
    unsigned int bank;

    if (gb < 0x4000u) {
        bank = state->mode != 0u ? (unsigned int)state->bank2 << 5 : 0u;
    } else {
        bank = ((unsigned int)state->bank2 << 5) |
            (state->bank1 == 0u ? 1u : state->bank1);
    }

    return (size_t)bank * TRPAK_ROM_BANK_SIZE + (size_t)(gb & 0x3FFFu);
}

/** @brief Resolves a Game Boy RAM address; only mode 1 sees banks above 0. */
static size_t mbc1_ram_offset(const mock_mbc1 *state, uint16_t gb)
{
    unsigned int bank = state->mode != 0u ? state->bank2 : 0u;

    return (size_t)bank * TRPAK_RAM_BANK_SIZE + (size_t)(gb - 0xA000u);
}

/** @brief Mock ::trpak_read_block_fn backed by the MBC1 state above. */
static int mbc1_read(
    void *user,
    int controller,
    uint16_t address,
    uint8_t data[TRPAK_TRANSFER_BLOCK_SIZE]
)
{
    mock_mbc1 *state = user;
    uint16_t gb;

    assert(controller == 0);
    memset(data, 0, TRPAK_TRANSFER_BLOCK_SIZE);

    if (address == 0x8000u) {
        data[0] = state->power;
        return 0;
    }
    if (address == 0xB000u) {
        data[0] = mbc1_status(state);
        /* Hardware clears the reset latch as a side effect of reading it. */
        state->was_reset = false;
        return 0;
    }
    if (address < 0xC000u) {
        return -1;
    }

    gb = mbc1_gb_address(state, address);
    if (gb < 0x8000u) {
        memcpy(data, state->rom + mbc1_rom_offset(state, gb),
            TRPAK_TRANSFER_BLOCK_SIZE);
        mbc1_maybe_reset(state);
        return 0;
    }
    if (gb >= 0xA000u && gb < 0xC000u) {
        /* Disabled RAM floats high on real cartridges. */
        if (!state->ram_enabled) {
            memset(data, 0xFF, TRPAK_TRANSFER_BLOCK_SIZE);
            return 0;
        }
        memcpy(data, state->ram + mbc1_ram_offset(state, gb),
            TRPAK_TRANSFER_BLOCK_SIZE);
        mbc1_maybe_reset(state);
        return 0;
    }
    return -1;
}

/** @brief Mock ::trpak_write_block_fn that decodes the MBC1 registers. */
static int mbc1_write(
    void *user,
    int controller,
    uint16_t address,
    const uint8_t data[TRPAK_TRANSFER_BLOCK_SIZE]
)
{
    mock_mbc1 *state = user;
    uint16_t gb;

    assert(controller == 0);
    if (address == 0x8000u) {
        state->power = data[0] == 0x84u ? 0x84u : 0u;
        return 0;
    }
    if (address == 0xA000u) {
        state->window = data[0];
        return 0;
    }
    if (address == 0xB000u) {
        state->access = data[0];
        return 0;
    }
    if (address < 0xC000u) {
        return -1;
    }

    gb = mbc1_gb_address(state, address);
    if (gb < 0x8000u) {
        /* Register writes move a whole 32-byte block. Modelling them by the
         * first byte is only faithful if every byte of the block lands in the
         * same 8 KiB register region, so assert that the library never pokes a
         * register close enough to a boundary to straddle it. */
        assert((gb >> 13) ==
            (uint16_t)((gb + TRPAK_TRANSFER_BLOCK_SIZE - 1u) >> 13));

        if (gb < 0x2000u) {
            state->ram_enabled = (data[0] & 0x0Fu) == 0x0Au;
        } else if (gb < 0x4000u) {
            state->bank1 = (uint8_t)(data[0] & 0x1Fu);
        } else if (gb < 0x6000u) {
            state->bank2 = (uint8_t)(data[0] & 0x03u);
        } else {
            state->mode = (uint8_t)(data[0] & 0x01u);
        }
        return 0;
    }
    if (gb >= 0xA000u && gb < 0xC000u) {
        if (state->ram_enabled) {
            memcpy(state->ram + mbc1_ram_offset(state, gb), data,
                TRPAK_TRANSFER_BLOCK_SIZE);
        }
        return 0;
    }
    return -1;
}

/**
 * @brief Installs the MBC1 mock, deliberately without a delay callback.
 *
 * Leaving trpak_io::delay unset also covers the optional-callback path: the
 * library must complete initialization without ever sleeping.
 */
static void configure_mbc1_mock(void)
{
    trpak_io io;

    memset(&io, 0, sizeof(io));
    io.read_block = mbc1_read;
    io.write_block = mbc1_write;
    io.user = &mbc1;
    assert(trpak_configure_io(&io, 0, MOCK_DMA_BASE) == TRPAK_OK);
}

/**
 * @brief Full dump of a 2 MiB MBC1 cartridge, plus its four RAM banks.
 *
 * The point of the test is the set of banks BANK1 cannot express — `0x20`,
 * `0x40` and `0x60` — which are only reachable through the fixed region in
 * advanced banking mode. Each ROM byte encodes its own bank number, so a bank
 * served from the wrong window, or silently substituted by the mapper's
 * "written as 0, reads as 1" rule, breaks the comparison.
 *
 * It also covers RAM banking on MBC1, which shares the mode register with ROM
 * banking, and the rejection of a header claiming more banks than MBC1 can
 * address.
 */
static void test_mbc1_large_rom(void)
{
    static uint8_t rom_copy[MBC1_ROM_SIZE];
    static uint8_t save_copy[MBC1_RAM_SIZE];
    static uint8_t restored[MBC1_RAM_SIZE];
    size_t bytes_read = 0u;
    size_t i;

    memset(&mbc1, 0, sizeof(mbc1));
    for (i = 0u; i < sizeof(mbc1.rom); i++) {
        /* Bank index XOR a position-dependent value: unique per bank, and
         * varying within each bank. */
        mbc1.rom[i] = (uint8_t)((i / TRPAK_ROM_BANK_SIZE) ^ (i * 31u));
    }
    for (i = 0u; i < sizeof(mbc1.ram); i++) {
        mbc1.ram[i] = (uint8_t)(i * 7u + 1u);
    }
    /* MBC1 + RAM + BATTERY, 128 banks (2 MiB), 32 KiB of RAM. */
    create_header(mbc1.rom + 0x100u, 0x03u, 0x06u, 0x03u);
    configure_mbc1_mock();

    assert(trpak_init() == TRPAK_OK);
    assert(trcart.mapper == TRPAK_MAPPER_MBC1);
    assert(trcart.rombanks == MBC1_BANKS);
    assert(trcart.romsize == MBC1_ROM_SIZE);
    assert(trcart.rambanks == MBC1_RAM_BANKS);

    assert(trpak_read_rom(rom_copy, sizeof(rom_copy), &bytes_read) == TRPAK_OK);
    assert(bytes_read == sizeof(mbc1.rom));
    assert(memcmp(rom_copy, mbc1.rom, sizeof(mbc1.rom)) == 0);

    /* Redundant after the comparison above, but names the banks that used to
     * be unreachable and made this cartridge undumpable. */
    for (i = 0x20u; i <= 0x60u; i += 0x20u) {
        size_t base = i * TRPAK_ROM_BANK_SIZE;
        assert(memcmp(rom_copy + base, mbc1.rom + base,
            TRPAK_ROM_BANK_SIZE) == 0);
    }

    /* The dump must leave the cartridge in its resting configuration. */
    assert(trcart.bank == 0u);
    assert(mbc1.mode == 0u);

    assert(trpak_read_save(save_copy, sizeof(save_copy), &bytes_read) ==
        TRPAK_OK);
    assert(bytes_read == sizeof(mbc1.ram));
    assert(memcmp(save_copy, mbc1.ram, sizeof(mbc1.ram)) == 0);

    for (i = 0u; i < sizeof(restored); i++) {
        restored[i] = (uint8_t)(0xFFu - (i & 0xFFu));
    }
    assert(trpak_write_save(restored, sizeof(restored), true) == TRPAK_OK);
    assert(memcmp(mbc1.ram, restored, sizeof(restored)) == 0);
    /* RAM must be protected again once the operation ends, and the mode
     * register must be back to simple banking so that the fixed region shows
     * bank 0 again. */
    assert(!mbc1.ram_enabled);
    assert(mbc1.mode == 0u);

    /* A header claiming 4 MiB is beyond BANK2's two bits, so the dump is
     * refused instead of wrapping. The DMA entry point is used only because
     * it skips the buffer-capacity check, which would otherwise reject the
     * call first and hide the mapper limit being tested. */
    create_header(mbc1.rom + 0x100u, 0x03u, 0x07u, 0x03u);
    assert(trpak_init() == TRPAK_OK);
    assert(trcart.rombanks == 256u);
    assert(trpak_read_rom_dma(NULL) == TRPAK_ERR_UNSUPPORTED_CARTRIDGE);

    assert(trpak_shutdown() == TRPAK_OK);
    assert(mbc1.power == 0u);
    assert(mbc1.access == 0u);
}

/**
 * @brief A reset in the middle of a transfer must not corrupt it silently.
 *
 * The accessory can reset while it is powered and a cartridge is inserted.
 * When that happens the mapper returns to its power-on state — bank registers
 * cleared, banking mode back to simple, cartridge RAM re-locked — while the
 * status keeps reporting powered, present, and ready. Only
 * ::TRPAK_STATUS_WAS_RESET distinguishes it, and it is a read-and-clear latch.
 *
 * Left unhandled, that is a silent-corruption bug rather than a failure: the
 * rest of the bank is served from whichever bank the mapper defaults to, and
 * the dump stores those bytes as if they were the ones asked for. On the save
 * paths it is worse — a backup reads the floating bus, and a restore is
 * dropped by the re-locked RAM while still reporting success.
 *
 * Each case below fires one reset mid-transfer and demands byte-exact data
 * anyway, which is only possible if the bank was re-selected in response.
 */
static void test_reset_during_transfer(void)
{
    static uint8_t rom_copy[MBC1_ROM_SIZE];
    static uint8_t save_copy[MBC1_RAM_SIZE];
    static uint8_t restored[MBC1_RAM_SIZE];
    size_t bytes_read = 0u;
    size_t i;

    memset(&mbc1, 0, sizeof(mbc1));
    for (i = 0u; i < sizeof(mbc1.rom); i++) {
        mbc1.rom[i] = (uint8_t)((i / TRPAK_ROM_BANK_SIZE) ^ (i * 31u));
    }
    for (i = 0u; i < sizeof(mbc1.ram); i++) {
        mbc1.ram[i] = (uint8_t)(i * 11u + 3u);
    }
    create_header(mbc1.rom + 0x100u, 0x03u, 0x06u, 0x03u);
    configure_mbc1_mock();
    assert(trpak_init() == TRPAK_OK);

    /* A bank is 512 blocks, so this lands deep inside bank 9 — far enough in
     * that an unrecovered reset would corrupt the rest of that bank and be
     * impossible to miss in the comparison. */
    mbc1.blocks_to_reset = 5000u;
    assert(trpak_read_rom(rom_copy, sizeof(rom_copy), &bytes_read) == TRPAK_OK);
    assert(mbc1.resets == 1u);
    assert(bytes_read == sizeof(mbc1.rom));
    assert(memcmp(rom_copy, mbc1.rom, sizeof(mbc1.rom)) == 0);

    /* Same for a backup, where the reset also re-locks RAM: every block after
     * it would otherwise read back as 0xFF. */
    mbc1.resets = 0u;
    mbc1.blocks_to_reset = 100u;
    assert(trpak_read_save(save_copy, sizeof(save_copy), &bytes_read) ==
        TRPAK_OK);
    assert(mbc1.resets == 1u);
    assert(bytes_read == sizeof(mbc1.ram));
    assert(memcmp(save_copy, mbc1.ram, sizeof(mbc1.ram)) == 0);

    /* And for a restore. Verification is on, so a write silently swallowed by
     * re-locked RAM would surface as TRPAK_ERR_VERIFY_FAILED. */
    for (i = 0u; i < sizeof(restored); i++) {
        restored[i] = (uint8_t)(i * 5u + 0x40u);
    }
    mbc1.resets = 0u;
    mbc1.blocks_to_reset = 100u;
    assert(trpak_write_save(restored, sizeof(restored), true) == TRPAK_OK);
    assert(mbc1.resets == 1u);
    assert(memcmp(mbc1.ram, restored, sizeof(restored)) == 0);

    mbc1.blocks_to_reset = 0u;
    assert(trpak_shutdown() == TRPAK_OK);
}

/**
 * @brief A rejected RAM bank must not leave cartridge RAM unlocked.
 *
 * Runs against the MBC1 mock the previous test installed, but describes other
 * cartridges in ::trcart. Those are the shapes that matter here: MBC3 and
 * rumble MBC5 both stop at eight RAM banks for reasons outside the register's
 * width — higher values select the RTC on one and drive the motor on the
 * other — so a header can legitimately declare a bank that the mapper must
 * still refuse. MBC1 and HuC1 cannot show the bug, as their limit is the
 * register width itself and was always checked early.
 *
 * The mock decodes the RAM-enable write at Game Boy `0x0000` regardless of
 * mapper, so mock_mbc1::ram_enabled records whether the unlock happened before
 * the rejection.
 */
static void test_ram_stays_locked_on_rejected_bank(void)
{
    assert(!mbc1.ram_enabled);

    memset(&trcart, 0, sizeof(trcart));
    trcart.ram = 1u;
    trcart.rambanks = 16u;
    trcart.ramsize = 16u * TRPAK_RAM_BANK_SIZE;

    trcart.mapper = TRPAK_MAPPER_MBC5;
    trcart.rumble = 1u;
    assert(trpak_select_ram_bank(8u) == TRPAK_ERR_INVALID_BANK);
    assert(!mbc1.ram_enabled);

    trcart.mapper = TRPAK_MAPPER_MBC3;
    trcart.rumble = 0u;
    assert(trpak_select_ram_bank(8u) == TRPAK_ERR_INVALID_BANK);
    assert(!mbc1.ram_enabled);

    /* A bank the mapper can reach still unlocks RAM, so the check above is
     * not simply refusing everything. */
    assert(trpak_select_ram_bank(7u) == TRPAK_OK);
    assert(mbc1.ram_enabled);
    assert(trpak_disable_ram() == TRPAK_OK);
    assert(!mbc1.ram_enabled);
}

/**
 * @brief Headers claiming more banks than the mapper can select are refused
 *        before any data moves.
 *
 * These guards all run ahead of the first accessory access, so ::trcart is
 * driven directly and no mock traffic is involved. The DMA entry points are
 * used because they skip the buffer-capacity check, which would otherwise
 * reject the call first and hide the limit being tested.
 *
 * `bytes_read` is pre-set to a sentinel every time: the documented contract is
 * that it always reflects what was copied, so a refusal must leave `0` behind
 * rather than the caller's previous value.
 */
static void test_mapper_bank_limits(void)
{
    size_t bytes_read;

    /* Mapper-less cartridge. trpak_select_rom_bank() writes the bank number
     * straight into the window's slice register there, so a header claiming
     * more than two banks would have pointed the window at Game Boy 0x8000 —
     * which is not ROM — and reported success over the resulting garbage. */
    memset(&trcart, 0, sizeof(trcart));
    trcart.mapper = TRPAK_MAPPER_NONE;
    trcart.rombanks = 8u;
    trcart.romsize = 8u * TRPAK_ROM_BANK_SIZE;
    bytes_read = 0xABCDu;
    assert(trpak_read_rom_dma(&bytes_read) == TRPAK_ERR_UNSUPPORTED_CARTRIDGE);
    assert(bytes_read == 0u);

    /* MBC2's bank register is four bits wide. */
    memset(&trcart, 0, sizeof(trcart));
    trcart.mapper = TRPAK_MAPPER_MBC2;
    trcart.rombanks = 512u;
    trcart.romsize = 512u * TRPAK_ROM_BANK_SIZE;
    bytes_read = 0xABCDu;
    assert(trpak_read_rom_dma(&bytes_read) == TRPAK_ERR_UNSUPPORTED_CARTRIDGE);
    assert(bytes_read == 0u);

    /* The Camera mapper's is six bits wide. */
    memset(&trcart, 0, sizeof(trcart));
    trcart.mapper = TRPAK_MAPPER_CAMERA;
    trcart.rombanks = 128u;
    trcart.romsize = 128u * TRPAK_ROM_BANK_SIZE;
    bytes_read = 0xABCDu;
    assert(trpak_read_rom_dma(&bytes_read) == TRPAK_ERR_UNSUPPORTED_CARTRIDGE);
    assert(bytes_read == 0u);

    /* Rumble MBC5: bit 3 of the RAM bank register drives the motor, so only
     * eight of the sixteen banks this header claims are reachable. Both save
     * directions must refuse rather than transfer half the file. */
    memset(&trcart, 0, sizeof(trcart));
    trcart.mapper = TRPAK_MAPPER_MBC5;
    trcart.rumble = 1u;
    trcart.ram = 1u;
    trcart.rambanks = 16u;
    trcart.ramsize = 16u * TRPAK_RAM_BANK_SIZE;
    bytes_read = 0xABCDu;
    assert(trpak_read_save_dma(&bytes_read) == TRPAK_ERR_UNSUPPORTED_CARTRIDGE);
    assert(bytes_read == 0u);
    assert(trpak_write_save_dma(false) == TRPAK_ERR_UNSUPPORTED_CARTRIDGE);

    /* And a cartridge with no RAM at all still reports that, with a defined
     * byte count. */
    memset(&trcart, 0, sizeof(trcart));
    bytes_read = 0xABCDu;
    assert(trpak_read_save_dma(&bytes_read) == TRPAK_ERR_NO_RAM);
    assert(bytes_read == 0u);
}

/**
 * @brief Argument checking in the low-level accessors.
 *
 * Covers a misaligned ROM address, one past the last full block, a RAM address
 * below the RAM window, a NULL buffer, and the supported/unsupported answers
 * of trpak_mapper_is_supported().
 */
static void test_api_boundaries(void)
{
    uint8_t block[TRPAK_TRANSFER_BLOCK_SIZE];

    assert(trpak_read_rom_block(0xC001u, block) ==
        TRPAK_ERR_INVALID_ARGUMENT);
    assert(trpak_read_rom_block(0xFFE1u, block) ==
        TRPAK_ERR_INVALID_ARGUMENT);
    assert(trpak_read_ram_block(0xD000u, block) ==
        TRPAK_ERR_INVALID_ARGUMENT);
    assert(trpak_write_ram_block(0xE000u, NULL) ==
        TRPAK_ERR_INVALID_ARGUMENT);
    assert(trpak_mapper_is_supported(TRPAK_MAPPER_MBC5));
    assert(!trpak_mapper_is_supported(TRPAK_MAPPER_MBC4));
}

/**
 * @brief Runs the suite; any failed assertion aborts the process.
 *
 * The order matters in two places: test_api_boundaries() runs after the
 * lifecycle test, reusing the backend it configured, and the last two tests
 * overwrite ::trcart, so nothing after them may depend on the cartridge the
 * earlier tests set up.
 *
 * @return `0` when every assertion held.
 */
int main(void)
{
    test_header_parser();
    test_header_edge_cases();
    test_configuration_validation();
    test_lifecycle_buffers_and_dma();
    test_mbc1_large_rom();
    test_reset_during_transfer();
    test_ram_stays_locked_on_rejected_bank();
    test_mapper_bank_limits();
    test_api_boundaries();
    puts("libtrpak tests: OK");
    return 0;
}
