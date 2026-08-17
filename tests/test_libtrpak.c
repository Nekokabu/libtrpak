#include "../libtrpak.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MOCK_ROM_SIZE (2u * TRPAK_ROM_BANK_SIZE)
#define MOCK_RAM_SIZE TRPAK_RAM_BANK_SIZE
#define MOCK_DMA_BASE ((uintptr_t)0x10000000u)

typedef struct mock_transfer_pak {
    uint8_t rom[MOCK_ROM_SIZE];
    uint8_t ram[MOCK_RAM_SIZE];
    uint8_t dma[MOCK_ROM_SIZE];
    uint8_t power;
    uint8_t access;
    uint8_t window;
    bool removed;
    unsigned int delay_calls;
} mock_transfer_pak;

static mock_transfer_pak mock;

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

static void mock_delay(void *user, unsigned int milliseconds)
{
    mock_transfer_pak *state = user;
    assert(milliseconds > 0u);
    state->delay_calls++;
}

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

static void finish_checksum(uint8_t header[TRPAK_HEADER_SIZE])
{
    uint8_t checksum = 0u;
    size_t i;

    for (i = 0x34u; i <= 0x4Cu; i++) {
        checksum = (uint8_t)(checksum - header[i] - 1u);
    }
    header[0x4Du] = checksum;
}

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

static void test_lifecycle_buffers_and_dma(void)
{
    uint8_t rom_copy[MOCK_ROM_SIZE];
    uint8_t save_copy[MOCK_RAM_SIZE];
    size_t bytes_read = 0u;
    size_t i;

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

int main(void)
{
    test_header_parser();
    test_configuration_validation();
    test_lifecycle_buffers_and_dma();
    test_api_boundaries();
    puts("libtrpak tests: OK");
    return 0;
}
