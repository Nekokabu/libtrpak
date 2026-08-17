#ifndef LIBTRPAK_H
#define LIBTRPAK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRPAK_MAPPER_NONE   0x00u
#define TRPAK_MAPPER_MBC1   0x01u
#define TRPAK_MAPPER_MBC2   0x02u
#define TRPAK_MAPPER_MMM01  0x03u
#define TRPAK_MAPPER_MBC3   0x04u
#define TRPAK_MAPPER_MBC5   0x05u
#define TRPAK_MAPPER_CAMERA 0x06u
#define TRPAK_MAPPER_TAMA5  0x07u
#define TRPAK_MAPPER_HUC3   0x08u
#define TRPAK_MAPPER_HUC1   0x09u
#define TRPAK_MAPPER_MBC4   0x10u /* Recognized but not implemented. */

#define TRPAK_ROM_BANK_SIZE      (16u * 1024u)
#define TRPAK_TRANSFER_BLOCK_SIZE 32u
#define TRPAK_RAM_BANK_SIZE       (8u * 1024u)
#define TRPAK_HEADER_SIZE         0x50u
#define TRPAK_DEFAULT_DMA_BASE    ((uintptr_t)0xB2000000u)

#define TRPAK_STATUS_READY        0x01u
#define TRPAK_STATUS_WAS_RESET    0x04u
#define TRPAK_STATUS_IS_RESETTING 0x08u
#define TRPAK_STATUS_REMOVED      0x40u
#define TRPAK_STATUS_POWERED      0x80u

typedef enum trpak_result {
    TRPAK_OK = 0,
    TRPAK_ERR_IO = -1,
    TRPAK_ERR_POWER_STATE = -2,
    TRPAK_ERR_ACCESS_STATE = -3,
    TRPAK_ERR_POWER_OFF = -4,
    TRPAK_ERR_INVALID_ARGUMENT = -10,
    TRPAK_ERR_BUFFER_TOO_SMALL = -11,
    TRPAK_ERR_UNSUPPORTED_CARTRIDGE = -12,
    TRPAK_ERR_INVALID_BANK = -13,
    TRPAK_ERR_NO_RAM = -14,
    TRPAK_ERR_VERIFY_FAILED = -15,
    TRPAK_ERR_INVALID_HEADER = -16,
    TRPAK_ERR_NO_CARTRIDGE = -17,
    TRPAK_ERR_TRANSFER_TIMEOUT = -18
} trpak_result;

typedef struct trpak_cart {
    uint8_t mapper;
    uint8_t ram;
    uint8_t battery;
    uint8_t rtc;
    uint8_t rumble;
    uint8_t sgb;
    uint8_t gbc;
    char title[17];
    uint8_t _romsize;
    uint8_t _ramsize;
    uint8_t cartridge_type;
    uint32_t romsize;
    uint32_t ramsize;
    uint16_t rombanks;
    uint16_t rambanks;
    uint16_t bank;
    uint16_t cpld;
} trpak_cart;

typedef int (*trpak_read_block_fn)(
    void *user,
    int controller,
    uint16_t address,
    uint8_t data[TRPAK_TRANSFER_BLOCK_SIZE]
);

typedef int (*trpak_write_block_fn)(
    void *user,
    int controller,
    uint16_t address,
    const uint8_t data[TRPAK_TRANSFER_BLOCK_SIZE]
);

typedef void (*trpak_delay_fn)(void *user, unsigned int milliseconds);

typedef int (*trpak_dma_store_fn)(
    void *user,
    const uint8_t *source,
    uintptr_t destination,
    size_t size
);

typedef int (*trpak_dma_load_fn)(
    void *user,
    uint8_t *destination,
    uintptr_t source,
    size_t size
);

typedef struct trpak_io {
    trpak_read_block_fn read_block;
    trpak_write_block_fn write_block;
    trpak_delay_fn delay;
    trpak_dma_store_fn dma_store;
    trpak_dma_load_fn dma_load;
    void *user;
} trpak_io;

extern trpak_cart trcart;

/* Configuration and lifecycle. */
int trpak_configure_io(const trpak_io *io, int controller, uintptr_t dma_base);
void trpak_use_default_io(void);
int trpak_init(void);
int trpak_shutdown(void);
const char *trpak_error_string(int result);

/* Header parsing can be tested without Nintendo 64 hardware. The supplied
 * buffer represents Game Boy addresses 0x0100 through 0x014F. */
int trpak_parse_cartridge_header(
    const uint8_t header[TRPAK_HEADER_SIZE],
    size_t header_size,
    trpak_cart *out
);
bool trpak_check_header_checksum(
    const uint8_t header[TRPAK_HEADER_SIZE],
    size_t header_size
);
bool trpak_mapper_is_supported(uint8_t mapper);

/* Safe buffer-based bulk operations. */
int trpak_read_rom(uint8_t *destination, size_t capacity, size_t *bytes_read);
int trpak_read_save(uint8_t *destination, size_t capacity, size_t *bytes_read);
int trpak_write_save(
    const uint8_t *source,
    size_t size,
    bool verify_after_write
);

/* Bulk operations using the configured DMA base and callbacks. */
int trpak_read_rom_dma(size_t *bytes_read);
int trpak_read_save_dma(size_t *bytes_read);
int trpak_write_save_dma(bool verify_after_write);

/* Power, access state and banking. */
int trpak_set_power(bool enabled);
int trpak_get_power(bool *enabled);
int trpak_set_access_state(bool enabled);
int trpak_get_status(uint8_t *status);
int trpak_get_access_state(int *state);
int trpak_select_rom_bank(uint16_t bank);
int trpak_select_ram_bank(uint16_t bank);
int trpak_disable_ram(void);

/* Low-level 32-byte accessors for the Transfer Pak data windows. */
int trpak_read_rom_block(uint16_t address, uint8_t *data);
int trpak_read_ram_block(uint16_t address, uint8_t *data);
int trpak_write_ram_block(uint16_t address, const uint8_t *data);

#ifdef __cplusplus
}
#endif

#endif
