# libtrpak

A C library for accessing Game Boy and Game Boy Color cartridges on Nintendo 64 through the **Transfer Pak (NUS-019)**.

`libtrpak` is a new library developed from the
[libgbpak](https://github.com/saturnu/libgbpak) project. It is not a simple
rename and does not provide a compatibility layer for the base project's API:
its files, types, constants, and public functions belong to the `trpak`
namespace.

The project uses [libdragon](https://github.com/DragonMinded/libdragon) to control the accessory, read cartridge headers, select ROM/RAM banks, and back up or restore saves. The default backend retains the EverDrive 64 DMA workflow, while a configurable I/O layer supports other environments and hardware-free testing.

> **Status:** the logic can be compiled and tested on a computer, but hardware paths and experimental mappers still need validation on a real Nintendo 64 before being considered stable.

## Features

- Safe Transfer Pak initialization and shutdown.
- Detection of missing cartridges and unsupported types.
- Isolated, testable Game Boy header parsing.
- Header checksum validation before accepting mapper and size data.
- Mapper, ROM, RAM, battery, RTC, rumble, GBC, and SGB detection.
- ROM and RAM bank selection with bounds validation.
- ROM dumping to RDRAM or flashcart SDRAM.
- Save backup to RDRAM or SDRAM.
- Save restoration with mandatory size validation and read-after-write verification.
- Normalization of the MBC2 internal RAM's 4-bit values.
- RAM and accessory-state cleanup after operations and failures.
- Cartridge-removal detection before each block of a bulk operation.
- Injectable transport backend for tests and alternative platforms.
- Native `trpak`-namespace API.
- Host tests with Transfer Pak mocks.

## Repository layout

```text
.
├── libtrpak.c             # implementation, mappers, and default backend
├── libtrpak.h             # public API, types, and error codes
├── Makefile               # builds and runs host tests
├── tests/
│   ├── stubs/             # minimal contract for compiling the default backend
│   └── test_libtrpak.c    # tests with a simulated Transfer Pak
└── README.md
```

## Hardware and dependencies

### Default backend

Using the default backend directly requires:

- a Nintendo 64;
- a controller with a Transfer Pak;
- a Game Boy or Game Boy Color cartridge;
- a libdragon-compatible toolchain;
- libdragon with `joybus_accessory_read`, `joybus_accessory_write`, and `wait_ms`.

The backend uses the current Joybus API and aligned addresses. The deprecated `read_mempak_address()` and `write_mempak_address()` functions are not used.

To also enable EverDrive 64-specific DMA, compile `libtrpak.c` with `TRPAK_ENABLE_ED64_DMA` and provide a `sys.h` declaring `dma_write_s`, `dma_read_s`, and the related routines. Without this macro, the buffer-based API works normally and the project does not depend on `sys.h`.

By default, the library uses controller index `0`—port 1—and the SDRAM area beginning at `0xB2000000`.

### Custom backend

`trpak_configure_io()` lets you replace the read, write, delay, and DMA operations. This allows an application to:

- select any of the four controller ports;
- select a different SDRAM address;
- integrate another flashcart;
- transfer data through a custom implementation;
- simulate the accessory in tests.

The `read_block` and `write_block` callbacks are required. `delay`, `dma_store`, and `dma_load` are optional when the application only uses the buffer-based API.

## Integrating with an N64 project

The repository does not impose a build system for an N64 ROM. Add `libtrpak.c` and `libtrpak.h` to your project sources. Provide `sys.h` only when compiling with `TRPAK_ENABLE_ED64_DMA`.

The header includes the required standard types and can be included directly:

```c
#include <libdragon.h>
#include "libtrpak.h"
```

### Minimal SDRAM example

This example requires `TRPAK_ENABLE_ED64_DMA` and writes the ROM directly to the configured SDRAM.

```c
#include <stdio.h>
#include <libdragon.h>
#include "libtrpak.h"

int main(void)
{
    int result = trpak_init();

    if (result != TRPAK_OK) {
        printf("Failure: %s (%d)\n", trpak_error_string(result), result);
        return result;
    }

    printf("Title: %s\n", trcart.title);
    printf("ROM: %lu bytes, %u banks\n",
           (unsigned long)trcart.romsize,
           (unsigned int)trcart.rombanks);

    result = trpak_read_rom_dma(NULL);
    if (result != TRPAK_OK) {
        printf("Dump failed: %s\n", trpak_error_string(result));
    }

    {
        int shutdown_result = trpak_shutdown();
        if (result == TRPAK_OK) {
            result = shutdown_result;
        }
    }

    return result;
}
```

### Buffer-based example

Modern functions receive the buffer capacity and reject the operation if there is not enough space:

```c
uint8_t *rom_buffer = /* buffer with at least trcart.romsize bytes */;
size_t bytes_read = 0;

int result = trpak_read_rom(
    rom_buffer,
    trcart.romsize,
    &bytes_read
);
```

The destination pointer cannot be `NULL` in this API. For direct DMA, use `trpak_read_rom_dma()` and provide DMA callbacks to the backend.

## Save backup and restoration

### Backup

```c
uint8_t *save_buffer = /* buffer with trcart.ramsize bytes */;
size_t bytes_read;

int result = trpak_read_save(
    save_buffer,
    trcart.ramsize,
    &bytes_read
);
```

### Verified restoration

```c
int result = trpak_write_save(
    save_buffer,
    trcart.ramsize,
    true
);
```

Restoration requires `size` to be exactly equal to `trcart.ramsize`. When `verify_after_write` is `true`, each written block is read back and compared before the operation continues.

> Always create and persist a backup before restoring a save. Verification detects write differences, but it does not replace an external backup or recover previously overwritten data.

Do not remove the cartridge or Transfer Pak during an operation. Bulk routines attempt to disable RAM even if an intermediate failure occurs.

## Recommended flow

1. Initialize the application's required subsystems.
2. Optionally configure a backend with `trpak_configure_io()`.
3. Insert the cartridge and call `trpak_init()`.
4. Read `trcart` only after initialization returns `TRPAK_OK`.
5. Perform the required dump, backup, or restoration.
6. Call `trpak_shutdown()`, including after application errors.

If initialization fails after the Transfer Pak is powered on, the library itself attempts cleanup, disables access mode, and powers off the accessory. Initialization interprets status as a bitfield and waits up to 500 ms for reset completion and readiness.

## Cartridge information

The global `trcart` variable, declared in the header, receives the parsed metadata:

| Field | Description |
| --- | --- |
| `mapper` | One of the `TRPAK_MAPPER_*` constants. |
| `ram` | Indicates RAM presence according to the cartridge type. |
| `battery` | Indicates battery presence. |
| `rtc` | Indicates RTC presence. Clock control is not implemented yet. |
| `rumble` | Indicates rumble presence. Motor control is not implemented yet. |
| `sgb` | Super Game Boy compatibility byte. |
| `gbc` | `0x80` for GB/GBC, `0xC0` for GBC-only, or `0` otherwise. |
| `title[17]` | Title of up to 15 bytes for GBC cartridges or 16 bytes for older cartridges, always null-terminated. |
| `_romsize` | Raw ROM-size code. |
| `_ramsize` | Raw RAM-size code. |
| `cartridge_type` | Raw cartridge-type byte (`0x0147`). |
| `romsize` | Calculated ROM size in bytes. |
| `ramsize` | Calculated RAM size in bytes. |
| `rombanks` | Number of 16 KiB ROM banks. |
| `rambanks` | Number of RAM banks. |
| `bank` | Last selected ROM bank. |
| `cpld` | Reserved field; remains zero. |

## Modern API

### Configuration and lifecycle

| Function | Purpose |
| --- | --- |
| `trpak_configure_io(io, controller, dma_base)` | Installs a backend and selects the controller port and DMA address. |
| `trpak_use_default_io()` | Restores the libdragon/EverDrive backend, when compiled in. |
| `trpak_init()` | Powers the accessory, enables access, reads, and validates the header. |
| `trpak_shutdown()` | Disables RAM, access, and power. |
| `trpak_error_string(result)` | Returns a static description of an error code. |

### Parsing and capabilities

| Function | Purpose |
| --- | --- |
| `trpak_parse_cartridge_header(header, size, out)` | Parses ROM bytes `0x0100–0x014F` without hardware. |
| `trpak_check_header_checksum(header, size)` | Validates the checksum stored at `0x014D`. |
| `trpak_mapper_is_supported(mapper)` | Indicates whether an implemented banking path exists. |

The parser buffer represents addresses `0x0100–0x014F`; therefore, the cartridge type appears at index `0x47` of that buffer.

### Bulk operations

| Function | Purpose |
| --- | --- |
| `trpak_read_rom(destination, capacity, bytes_read)` | Copies the ROM to a buffer with capacity validation. |
| `trpak_read_save(destination, capacity, bytes_read)` | Copies RAM to a buffer and disables RAM when finished. |
| `trpak_write_save(source, size, verify)` | Writes an exactly sized save and optionally verifies each block. |
| `trpak_read_rom_dma(bytes_read)` | Copies the ROM to the configured DMA area. |
| `trpak_read_save_dma(bytes_read)` | Copies RAM to the configured DMA area. |
| `trpak_write_save_dma(verify)` | Restores the save from the configured DMA area. |

### Power, access, and banking

| Function | Purpose |
| --- | --- |
| `trpak_set_power(enabled)` | Powers the Transfer Pak on or off. |
| `trpak_get_power(&enabled)` | Queries power with validation of the returned value. |
| `trpak_set_access_state(enabled)` | Enables or disables access mode. |
| `trpak_get_status(&status)` | Gets the raw Transfer Pak status bitfield. |
| `trpak_get_access_state(&state)` | Interprets combinations of readiness, reset, and removal bits. |
| `trpak_select_rom_bank(bank)` | Validates and selects a ROM bank. |
| `trpak_select_ram_bank(bank)` | Enables RAM and selects a valid RAM bank. |
| `trpak_disable_ram()` | Disables and protects RAM again when the mapper permits it. HuC1 remains in RAM mode because it cannot disable RAM. |

### Low-level access

| Function | Accepted window |
| --- | --- |
| `trpak_read_rom_block(address, data)` | `0xC000–0xFFE0`. |
| `trpak_read_ram_block(address, data)` | `0xE000–0xFFE0`. |
| `trpak_write_ram_block(address, data)` | `0xE000–0xFFE0`. |

Addresses must be 32-byte aligned. Each call transfers exactly 32 bytes, and addresses belong to the Transfer Pak window rather than directly to the Game Boy bus.

## Error codes

| Code | Constant | Meaning |
| ---: | --- | --- |
| `0` | `TRPAK_OK` | Success. |
| `-1` | `TRPAK_ERR_IO` | Communication failure. |
| `-2` | `TRPAK_ERR_POWER_STATE` | Unexpected power state. |
| `-3` | `TRPAK_ERR_ACCESS_STATE` | Unexpected access state. |
| `-4` | `TRPAK_ERR_POWER_OFF` | Power was turned off during an operation. |
| `-10` | `TRPAK_ERR_INVALID_ARGUMENT` | Invalid pointer, size, port, or argument. |
| `-11` | `TRPAK_ERR_BUFFER_TOO_SMALL` | Buffer is too small. |
| `-12` | `TRPAK_ERR_UNSUPPORTED_CARTRIDGE` | Cartridge type or mapper lacks a safe implementation. |
| `-13` | `TRPAK_ERR_INVALID_BANK` | Bank is out of range or inaccessible. |
| `-14` | `TRPAK_ERR_NO_RAM` | Cartridge has no usable RAM. |
| `-15` | `TRPAK_ERR_VERIFY_FAILED` | Read-back data differs from written data. |
| `-16` | `TRPAK_ERR_INVALID_HEADER` | Invalid size code or cartridge header. |
| `-17` | `TRPAK_ERR_NO_CARTRIDGE` | No cartridge was detected. |
| `-18` | `TRPAK_ERR_TRANSFER_TIMEOUT` | The accessory did not become ready before the timeout. |

`trpak_init()` only returns the named codes above. Use `trpak_error_string()` to display a message.

## DMA transfers

`trpak_read_rom_dma()`, `trpak_read_save_dma()`, and `trpak_write_save_dma()` use the base configured through `trpak_configure_io()`. They require DMA callbacks; with the default backend, compile with `TRPAK_ENABLE_ED64_DMA`. Restoration can verify each block after it is written.

## Memory map

| Region | Purpose |
| --- | --- |
| `0x8000` | Transfer Pak power. |
| `0xA000` | Selects the 16 KiB bank exposed in the window. |
| `0xB000` | Status bitfield and access control. |
| `0xC000–0xFFFF` | 16 KiB ROM window. |
| `0xE000–0xFFFF` | 8 KiB RAM window. |
| `0xE000–0xE1FF` | 512 bytes of MBC2 internal RAM. |
| `0xB2000000` | Default SDRAM base for DMA functions. |

The largest currently recognized ROM size code represents 8 MiB. The largest recognized RAM size represents 128 KiB.

## Mapper support

| Family | Status | Notes |
| --- | --- | --- |
| ROM without MBC | Implemented | Non-banked ROM and RAM are supported. |
| MBC1 | Implemented | Full 128 banks (2 MiB). Banks `0x20`, `0x40`, and `0x60` are read through the fixed window in advanced banking mode, since the 5-bit register cannot select them. Multicart (MBC1M) wiring is not detected. |
| MBC2 | Implemented | 512-byte internal RAM is normalized to the lower nibble. |
| MMM01 | Detected, not implemented | Initialization returns an unsupported-cartridge error. |
| MBC3/MBC30 | ROM/RAM implemented | MBC30 supports all eight banks of a 64 KiB save. RTC is not controlled yet. |
| MBC4 | Detected, not implemented | Explicitly rejected; it does not reuse the MBC5 sequence. |
| MBC5 | ROM/RAM implemented | Writes the lower eight bits to `0x2000` and the 9th bit to `0x3000`; on rumble cartridges, RAM is limited to eight banks. There is no rumble API. |
| Game Boy Camera | Experimental | Basic ROM/RAM support; camera registers have no API. |
| HuC1 | Partial and experimental | Uses its own six-bit ROM register and an independent RAM bank; dumps above 64 banks are rejected. |
| TAMA5 and HuC3 | Not implemented | Constants exist, but there is no functional parsing or banking. |

## Host tests

Run:

```sh
make test
```

The tests compile `libtrpak.c` with `TRPAK_NO_DEFAULT_IO`, install a simulated backend, and verify:

- parsing, checksums, and rejection of corrupted headers;
- ROM and RAM sizes for MBC5 cartridges;
- invalid arguments and controller ports;
- Transfer Pak initialization and shutdown;
- ROM reads and save backups to buffers;
- buffer-capacity validation;
- DMA ROM dumping and verified save restoration;
- interruption when a cartridge is removed;
- alignment and bounds of 32-byte accesses;
- explicit rejection of an unimplemented mapper;
- a full 2 MiB dump against a mock that emulates the real MBC1 registers,
  covering banks `0x20`, `0x40`, and `0x60`, RAM banking, and the rejection of
  headers claiming more banks than MBC1 can address.

The target also compiles the default backend against stubs containing the current Joybus, delay, cache, and DMA signatures, preventing name or type regressions in that layer.

Host tests do not validate electrical timing, physical cartridge behavior, or the DMA compatibility of each flashcart.

## Limitations and next steps

- Validate each mapper's command sequence on real hardware.
- Detect MBC1 multicart (MBC1M) wiring, whose 4-bit bank register makes the
  standard sequence produce a wrong dump.
- Add automatic backup through a callback before restoration.
- Add progress and cancellation callbacks.
- Implement MBC3 RTC, MBC5 rumble, and Game Boy Camera registers.
- Implement MMM01, HuC3, and TAMA5.
- Gradually replace global `trcart` state with independent contexts.
- Test backends for other flashcarts.
- Add Nintendo 64 tests to a compatibility matrix.

## Origin, credits, and references

This library was created from the technical work published in
[libgbpak](https://github.com/saturnu/libgbpak). The current implementation has
its own API, validations, tests, and organization, while recognizing the base
project and its authors as the origin of the initial Transfer Pak code and research.

- [libdragon](https://github.com/DragonMinded/libdragon)
- [Pan Docs — Game Boy technical specification](https://gbdev.io/pandocs/)
- [Pan Docs — Memory Bank Controllers](https://gbdev.io/pandocs/MBCs.html)
- [Base project: libgbpak](https://github.com/saturnu/libgbpak)

## License

Original `libtrpak` contributions are licensed under the [MIT License](LICENSE).
