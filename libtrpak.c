/*
 * libtrpak - Nintendo 64 Transfer Pak access for libdragon
 *
 * Public operations use the trpak_* namespace declared in libtrpak.h.
 */

#include "libtrpak.h"

#include <string.h>

#ifndef TRPAK_NO_DEFAULT_IO
#include <libdragon.h>
#endif

#ifdef TRPAK_ENABLE_ED64_DMA
#include "sys.h"
#endif

#define TP_POWER_ADDRESS       0x8000u
#define TP_REGISTER_ADDRESS    0xA000u
#define TP_ACCESS_ADDRESS      0xB000u
#define TP_ROM_WINDOW_START    0xC000u
#define TP_RAM_WINDOW_START    0xE000u
#define TP_WINDOW_END          0xFFE0u
#define TP_READY_POLL_ATTEMPTS 50u
#define TP_READY_POLL_DELAY_MS 10u

typedef struct trpak_runtime {
    trpak_io io;
    int controller;
    uintptr_t dma_base;
    bool configured;
} trpak_runtime;

static trpak_runtime runtime;
static uint8_t transfer_data[TRPAK_TRANSFER_BLOCK_SIZE];

trpak_cart trcart;

#ifndef TRPAK_NO_DEFAULT_IO
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

static void platform_delay(void *user, unsigned int milliseconds)
{
    (void)user;
    wait_ms(milliseconds);
}

#ifdef TRPAK_ENABLE_ED64_DMA
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
    runtime.configured = true;
#endif
}

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

int trpak_configure_io(const trpak_io *io, int controller, uintptr_t dma_base)
{
    if (io == NULL || io->read_block == NULL || io->write_block == NULL ||
        controller < 0 || controller > 3) {
        return TRPAK_ERR_INVALID_ARGUMENT;
    }

    runtime.io = *io;
    runtime.controller = controller;
    runtime.dma_base = dma_base;
    runtime.configured = true;
    return TRPAK_OK;
}

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

static int write_filled(uint16_t address, uint8_t value)
{
    uint8_t data[TRPAK_TRANSFER_BLOCK_SIZE];
    memset(data, value, sizeof(data));
    return transport_write(address, data);
}

int trpak_set_power(bool enabled)
{
    int result = write_filled(TP_POWER_ADDRESS, enabled ? 0x84u : 0xFEu);

    if (result == TRPAK_OK && runtime.io.delay != NULL) {
        runtime.io.delay(runtime.io.user, 200u);
    }

    return result;
}

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
        return TRPAK_ERR_POWER_STATE;
    }

    return TRPAK_OK;
}

int trpak_set_access_state(bool enabled)
{
    return write_filled(TP_ACCESS_ADDRESS, enabled ? 0x01u : 0x00u);
}

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

static int decode_rom_size(uint8_t code, uint16_t *banks)
{
    if (banks == NULL) {
        return TRPAK_ERR_INVALID_ARGUMENT;
    }

    switch (code) {
    case 0x00u: *banks = 2u; break;
    case 0x01u: *banks = 4u; break;
    case 0x02u: *banks = 8u; break;
    case 0x03u: *banks = 16u; break;
    case 0x04u: *banks = 32u; break;
    case 0x05u: *banks = 64u; break;
    case 0x06u: *banks = 128u; break;
    case 0x07u: *banks = 256u; break;
    case 0x08u: *banks = 512u; break;
    case 0x52u: *banks = 72u; break;
    case 0x53u: *banks = 80u; break;
    case 0x54u: *banks = 96u; break;
    default: return TRPAK_ERR_INVALID_HEADER;
    }

    return TRPAK_OK;
}

static int decode_cartridge_type(uint8_t type, trpak_cart *out)
{
    out->cartridge_type = type;

    switch (type) {
    case 0x00u:
        out->mapper = TRPAK_MAPPER_NONE;
        break;
    case 0x01u:
        out->mapper = TRPAK_MAPPER_MBC1;
        break;
    case 0x02u:
        out->mapper = TRPAK_MAPPER_MBC1;
        out->ram = true;
        break;
    case 0x03u:
        out->mapper = TRPAK_MAPPER_MBC1;
        out->ram = true;
        out->battery = true;
        break;
    case 0x05u:
        out->mapper = TRPAK_MAPPER_MBC2;
        out->ram = true;
        break;
    case 0x06u:
        out->mapper = TRPAK_MAPPER_MBC2;
        out->ram = true;
        out->battery = true;
        break;
    case 0x08u:
        out->mapper = TRPAK_MAPPER_NONE;
        out->ram = true;
        break;
    case 0x09u:
        out->mapper = TRPAK_MAPPER_NONE;
        out->ram = true;
        out->battery = true;
        break;
    case 0x0Bu:
        out->mapper = TRPAK_MAPPER_MMM01;
        break;
    case 0x0Cu:
        out->mapper = TRPAK_MAPPER_MMM01;
        out->ram = true;
        break;
    case 0x0Du:
        out->mapper = TRPAK_MAPPER_MMM01;
        out->ram = true;
        out->battery = true;
        break;
    case 0x0Fu:
        out->mapper = TRPAK_MAPPER_MBC3;
        out->battery = true;
        out->rtc = true;
        break;
    case 0x10u:
        out->mapper = TRPAK_MAPPER_MBC3;
        out->ram = true;
        out->battery = true;
        out->rtc = true;
        break;
    case 0x11u:
        out->mapper = TRPAK_MAPPER_MBC3;
        break;
    case 0x12u:
        out->mapper = TRPAK_MAPPER_MBC3;
        out->ram = true;
        break;
    case 0x13u:
        out->mapper = TRPAK_MAPPER_MBC3;
        out->ram = true;
        out->battery = true;
        break;
    case 0x15u:
        out->mapper = TRPAK_MAPPER_MBC4;
        break;
    case 0x16u:
        out->mapper = TRPAK_MAPPER_MBC4;
        out->ram = true;
        break;
    case 0x17u:
        out->mapper = TRPAK_MAPPER_MBC4;
        out->ram = true;
        out->battery = true;
        break;
    case 0x19u:
        out->mapper = TRPAK_MAPPER_MBC5;
        break;
    case 0x1Au:
        out->mapper = TRPAK_MAPPER_MBC5;
        out->ram = true;
        break;
    case 0x1Bu:
        out->mapper = TRPAK_MAPPER_MBC5;
        out->ram = true;
        out->battery = true;
        break;
    case 0x1Cu:
        out->mapper = TRPAK_MAPPER_MBC5;
        out->rumble = true;
        break;
    case 0x1Du:
        out->mapper = TRPAK_MAPPER_MBC5;
        out->ram = true;
        out->rumble = true;
        break;
    case 0x1Eu:
        out->mapper = TRPAK_MAPPER_MBC5;
        out->ram = true;
        out->battery = true;
        out->rumble = true;
        break;
    case 0xFCu:
        out->mapper = TRPAK_MAPPER_CAMERA;
        out->ram = true;
        out->battery = true;
        break;
    case 0xFFu:
        out->mapper = TRPAK_MAPPER_HUC1;
        out->ram = true;
        out->battery = true;
        break;
    default:
        return TRPAK_ERR_UNSUPPORTED_CARTRIDGE;
    }

    return TRPAK_OK;
}

static int decode_ram_size(trpak_cart *out)
{
    if (!out->ram) {
        out->rambanks = 0u;
        out->ramsize = 0u;
        return TRPAK_OK;
    }

    if (out->mapper == TRPAK_MAPPER_MBC2) {
        out->rambanks = 1u;
        out->ramsize = 512u;
        return TRPAK_OK;
    }

    switch (out->_ramsize) {
    case 0x01u:
        out->rambanks = 1u;
        out->ramsize = 2u * 1024u;
        break;
    case 0x02u:
        out->rambanks = 1u;
        out->ramsize = 8u * 1024u;
        break;
    case 0x03u:
        out->rambanks = 4u;
        out->ramsize = 32u * 1024u;
        break;
    case 0x04u:
        out->rambanks = 16u;
        out->ramsize = 128u * 1024u;
        break;
    case 0x05u:
        out->rambanks = 8u;
        out->ramsize = 64u * 1024u;
        break;
    default:
        return TRPAK_ERR_INVALID_HEADER;
    }

    return TRPAK_OK;
}

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

    for (i = 0x34u; i <= 0x4Cu; i++) {
        checksum = (uint8_t)(checksum - header[i] - 1u);
    }

    return checksum == header[0x4Du];
}

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
    out->gbc = (header[0x43] == 0x80u || header[0x43] == 0xC0u)
        ? header[0x43]
        : 0u;
    out->sgb = header[0x46];
    out->_romsize = header[0x48];
    out->_ramsize = header[0x49];

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
        return false;
    }
}

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

    if (trcart.mapper == TRPAK_MAPPER_NONE ||
        (bank == 0u && trcart.mapper != TRPAK_MAPPER_MBC1 &&
         trcart.mapper != TRPAK_MAPPER_HUC1 && trcart.mapper != TRPAK_MAPPER_MBC5)) {
        result = write_filled(TP_REGISTER_ADDRESS, (uint8_t)bank);
        if (result == TRPAK_OK) {
            trcart.bank = original_bank;
        }
        return result;
    }

    switch (trcart.mapper) {
    case TRPAK_MAPPER_MBC1: {
        uint8_t lower = (uint8_t)(bank & 0x1Fu);
        uint8_t upper = (uint8_t)((bank >> 5) & 0x03u);

        if (bank > 0x7Fu || (lower == 0u && bank != 0u)) {
            return TRPAK_ERR_INVALID_BANK;
        }

        if ((result = write_filled(TP_REGISTER_ADDRESS, 0x01u)) != 0 ||
            (result = write_filled(0xE016u, 0x00u)) != 0 ||
            (result = write_filled(0xC000u, upper)) != 0) {
            return result;
        }
        if (bank == 0u) {
            result = write_filled(TP_REGISTER_ADDRESS, 0x00u);
            if (result != TRPAK_OK) {
                return result;
            }
        } else {
            if ((result = write_filled(TP_REGISTER_ADDRESS, 0x00u)) != 0 ||
                (result = write_filled(0xE100u, lower)) != 0 ||
                (result = write_filled(TP_REGISTER_ADDRESS, 0x01u)) != 0) {
                return result;
            }
        }
        break;
    }
    case TRPAK_MAPPER_HUC1:
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
        uint8_t lower = (uint8_t)(bank & 0xFFu);
        uint8_t upper = (uint8_t)((bank >> 8) & 0x01u);

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

int trpak_select_ram_bank(uint16_t bank)
{
    int result;

    if (!trcart.ram || trcart.rambanks == 0u) {
        return TRPAK_ERR_NO_RAM;
    }
    if (bank >= trcart.rambanks) {
        return TRPAK_ERR_INVALID_BANK;
    }

    if (trcart.mapper == TRPAK_MAPPER_NONE) {
        return bank == 0u ? TRPAK_OK : TRPAK_ERR_INVALID_BANK;
    }
    if (!trpak_mapper_is_supported(trcart.mapper)) {
        return TRPAK_ERR_UNSUPPORTED_CARTRIDGE;
    }

    if ((trcart.mapper == TRPAK_MAPPER_MBC1 || trcart.mapper == TRPAK_MAPPER_HUC1) &&
        bank > 0x03u) {
        return TRPAK_ERR_INVALID_BANK;
    }

    if ((result = write_filled(TP_REGISTER_ADDRESS, 0x00u)) != 0 ||
        (result = write_filled(0xC000u, 0x0Au)) != 0) {
        return result;
    }

    if (trcart.mapper == TRPAK_MAPPER_MBC2) {
        return TRPAK_OK;
    }
    if (trcart.mapper == TRPAK_MAPPER_MBC3 && bank > 0x07u) {
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

    if ((result = write_filled(TP_REGISTER_ADDRESS, 0x01u)) != 0) {
        return result;
    }

    if (trcart.mapper == TRPAK_MAPPER_MBC1) {
        if ((result = write_filled(0xE000u, 0x01u)) != 0) {
            return result;
        }
    }

    return write_filled(0xC000u, (uint8_t)bank);
}

int trpak_disable_ram(void)
{
    int result;

    if (!trcart.ram || trcart.mapper == TRPAK_MAPPER_NONE) {
        return TRPAK_OK;
    }

    if (trcart.mapper == TRPAK_MAPPER_HUC1) {
        /* HuC1 cannot disable RAM. Any value other than 0x0E keeps its
         * A000-BFFF window in RAM mode rather than infrared mode. */
        return write_filled(TP_REGISTER_ADDRESS, 0x00u) == TRPAK_OK
            ? write_filled(0xC000u, 0x00u)
            : TRPAK_ERR_IO;
    }

    result = write_filled(TP_REGISTER_ADDRESS, 0x00u);
    if (result != TRPAK_OK) {
        return result;
    }
    return write_filled(0xC000u, 0x00u);
}

static bool block_address_is_valid(uint16_t address, uint16_t start)
{
    return address >= start && address <= TP_WINDOW_END &&
        (address & (TRPAK_TRANSFER_BLOCK_SIZE - 1u)) == 0u;
}

int trpak_read_rom_block(uint16_t address, uint8_t *data)
{
    if (data == NULL || !block_address_is_valid(address, TP_ROM_WINDOW_START)) {
        return TRPAK_ERR_INVALID_ARGUMENT;
    }
    return transport_read(address, data);
}

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

    memset(data, 0, TRPAK_TRANSFER_BLOCK_SIZE);
    return transport_read(address, data);
}

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

    if (destination == NULL || offset + TRPAK_TRANSFER_BLOCK_SIZE > capacity) {
        return TRPAK_ERR_BUFFER_TOO_SMALL;
    }
    memcpy(destination + offset, block, TRPAK_TRANSFER_BLOCK_SIZE);
    return TRPAK_OK;
}

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
    if (trcart.mapper == TRPAK_MAPPER_MBC1 && trcart.rombanks > 32u) {
        /* Banks 0x20/0x40/0x60 require reading through MBC1 mode 1's
         * fixed window. Refuse an incomplete dump until that path is
         * implemented and validated on hardware. */
        return TRPAK_ERR_UNSUPPORTED_CARTRIDGE;
    }
    if (trcart.mapper == TRPAK_MAPPER_HUC1 && trcart.rombanks > 64u) {
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

    {
        int reset_result = trpak_select_rom_bank(0u);
        if (result == TRPAK_OK && reset_result != TRPAK_OK) {
            result = reset_result;
        }
    }
    return result;
}

int trpak_read_rom(uint8_t *destination, size_t capacity, size_t *bytes_read)
{
    return read_rom_internal(destination, capacity, bytes_read, false);
}

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

static void normalize_mbc2_block(uint8_t *block)
{
    size_t i;
    for (i = 0u; i < TRPAK_TRANSFER_BLOCK_SIZE; i++) {
        block[i] &= 0x0Fu;
    }
}

static int finish_ram_operation(int operation_result)
{
    int cleanup_result = trpak_disable_ram();
    return operation_result == TRPAK_OK ? cleanup_result : operation_result;
}

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

int trpak_read_save(uint8_t *destination, size_t capacity, size_t *bytes_read)
{
    return read_save_internal(destination, capacity, bytes_read, false);
}

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

int trpak_write_save(
    const uint8_t *source,
    size_t size,
    bool verify_after_write
)
{
    return write_save_internal(source, size, verify_after_write, false);
}

int trpak_read_rom_dma(size_t *bytes_read)
{
    return read_rom_internal(NULL, 0u, bytes_read, true);
}

int trpak_read_save_dma(size_t *bytes_read)
{
    return read_save_internal(NULL, 0u, bytes_read, true);
}

int trpak_write_save_dma(bool verify_after_write)
{
    return write_save_internal(
        NULL,
        trcart.ramsize,
        verify_after_write,
        true
    );
}

static int fail_initialization(int result)
{
    /* Best-effort cleanup. Preserve the error that explains why init failed. */
    (void)trpak_disable_ram();
    (void)trpak_set_access_state(false);
    (void)trpak_set_power(false);
    return result;
}

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

    result = trpak_set_access_state(true);
    if (result != TRPAK_OK) {
        return fail_initialization(result);
    }
    result = wait_for_cartridge_ready();
    if (result != TRPAK_OK) {
        return fail_initialization(result);
    }
    result = trpak_select_rom_bank(0u);
    if (result != TRPAK_OK) {
        return fail_initialization(result);
    }

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
    memcpy(&header[0x40], transfer_data, 16u);

    result = trpak_parse_cartridge_header(header, sizeof(header), &trcart);
    if (result != TRPAK_OK) {
        return fail_initialization(result);
    }
    if (!trpak_mapper_is_supported(trcart.mapper)) {
        return fail_initialization(TRPAK_ERR_UNSUPPORTED_CARTRIDGE);
    }

    return TRPAK_OK;
}

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
