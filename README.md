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
- Recovery from an accessory reset mid-transfer, which silently invalidates the
  selected bank and re-locks cartridge RAM.
- Refusal of headers declaring more banks than the mapper can select, before
  any data moves, rather than a partial or wrapped image.
- RAM kept locked when a bank is rejected, so a refused call cannot leave a
  battery-backed save writable.
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
├── CHANGELOG.md           # notable changes, newest first
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

Do not remove the cartridge or Transfer Pak during an operation; removal is
detected before each block and aborts with `TRPAK_ERR_NO_CARTRIDGE`. An
accessory reset is recovered from instead, since it leaves a usable cartridge
behind. Bulk routines attempt to disable RAM even if an intermediate failure
occurs.

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
| `mapper` | One of the `TRPAK_MAPPER_*` constants. May name a mapper that is only detected; check `trpak_mapper_is_supported()`. |
| `ram` | Indicates usable save RAM. Cleared when the cartridge type claims RAM but the size code at `0x0149` is `0`. |
| `battery` | Indicates battery presence, straight from the cartridge type. |
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
| `rambanks` | Number of 8 KiB RAM banks, or `1` for MBC2's internal RAM. |
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

`bytes_read` is optional, and always written when supplied: it holds the number
of bytes that reached the destination, including on a partial failure, and `0`
when the operation is refused before any transfer starts.

Every bulk routine re-reads the accessory status before each 32-byte block. That
is what aborts a transfer as soon as the cartridge is pulled out, and what
detects an accessory reset — after a reset the mapper is back at its power-on
state, so the routine re-selects the bank (which also re-unlocks RAM) before
trusting the window again.

### Power, access, and banking

| Function | Purpose |
| --- | --- |
| `trpak_set_power(enabled)` | Powers the Transfer Pak on or off. |
| `trpak_get_power(&enabled)` | Queries power with validation of the returned value. |
| `trpak_set_access_state(enabled)` | Enables or disables access mode. |
| `trpak_get_status(&status)` | Gets the raw Transfer Pak status bitfield. |
| `trpak_get_access_state(&state)` | Interprets combinations of readiness, reset, and removal bits. |
| `trpak_select_rom_bank(bank)` | Validates and selects a ROM bank. |
| `trpak_select_ram_bank(bank)` | Validates the bank, then enables RAM and selects it. The order matters: a rejected bank leaves RAM locked. |
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

## Addressing model

Every transaction moves exactly 32 bytes to or from a 16-bit *Transfer Pak
address*, which is not a Game Boy bus address. The accessory exposes four
regions:

| Transfer Pak address | Purpose |
| --- | --- |
| `0x8000` | Power control: write `0x84` to switch the cartridge on, read the state back. |
| `0xA000` | Bank register: which 16 KiB slice of the Game Boy address space the window shows. |
| `0xB000` | Status bitfield on read, access-mode control on write. |
| `0xC000–0xFFFF` | 16 KiB data window onto the selected slice. |

The bank register picks the slice, so the Game Boy address actually reached is
`slice * 0x4000 + (address - 0xC000)`:

| Slice | Game Boy range | Typical use |
| --- | --- | --- |
| `0` | `0x0000–0x3FFF` | Fixed ROM bank 0, and the MBC write registers. |
| `1` | `0x4000–0x7FFF` | Switchable ROM bank, used for dumping. |
| `2` | `0x8000–0xBFFF` | Cartridge RAM, which appears at `0xE000–0xFFFF`. |

That formula is why the low-level helpers accept `0xC000–0xFFE0` for ROM but
only `0xE000–0xFFE0` for RAM: save RAM exists solely in the upper half of
slice 2. MBC2's 512 half-bytes occupy `0xE000–0xE1FF` of that window.

The same formula places every MBC register the library writes:

| Slice | Transfer Pak address | Game Boy address | Register |
| --- | --- | --- | --- |
| `0` | `0xC000` | `0x0000` | RAM enable (`0x0A` unlocks). |
| `0` | `0xE000` | `0x2000` | ROM bank — MBC5 low 8 bits, HuC1 six bits. |
| `0` | `0xE100` | `0x2100` | ROM bank, with address bit 8 set as MBC2 requires. |
| `0` | `0xF000` | `0x3000` | ROM bank bit 8 (MBC5). |
| `1` | `0xC000` | `0x4000` | RAM bank, or MBC1's upper ROM bank bits. |
| `1` | `0xE000` | `0x6000` | MBC1 banking mode. |

`0xB2000000` is the default SDRAM base used by the DMA helpers.

## Status bits

`trpak_get_status()` returns the raw byte read from `0xB000`:

| Bit | Constant | Meaning |
| --- | --- | --- |
| `0x01` | `TRPAK_STATUS_READY` | Cartridge present and accessible. |
| `0x04` | `TRPAK_STATUS_WAS_RESET` | A reset finished since the previous status read. Read-and-clear, so each read reports only new resets. |
| `0x08` | `TRPAK_STATUS_IS_RESETTING` | A reset is still in progress; wait. |
| `0x40` | `TRPAK_STATUS_REMOVED` | No cartridge, or it was pulled out. |
| `0x80` | `TRPAK_STATUS_POWERED` | Accessory power is on. |

`TRPAK_STATUS_WAS_RESET` deserves attention when driving the low-level API
directly. A reset leaves the accessory powered, present, and ready, so nothing
else in the status distinguishes it — but the mapper has returned to its
power-on state, which means the bank selected earlier is gone and cartridge RAM
has re-locked. Any bank selection made before that bit appears must be redone.
The bulk routines handle this for you.

`trpak_get_access_state()` condenses the bitfield into `3` for removed, `2` for
resetting, `1` for ready, and `0` for powered but not ready.

## Header decoding

The largest recognized ROM size code represents 8 MiB, and the largest
recognized RAM size code 128 KiB. An unrecognized code in either field is
rejected with `TRPAK_ERR_INVALID_HEADER` rather than guessed at, and an
unrecognized cartridge type byte with `TRPAK_ERR_UNSUPPORTED_CARTRIDGE`.

One exception is deliberate: a cartridge type byte that claims RAM while the
RAM size code at `0x0149` is `0` is decoded as RAM-less instead of rejected, so
the ROM of such a cartridge stays dumpable. `battery` still reflects what the
type byte claimed, so the disagreement remains visible. MBC2 is unaffected —
its 512 half-bytes come from the mapper, not from the size code.

## Mapper limits

Each mapper can only select as many banks as its register is wide, and two RAM
limits are narrower still: MBC3 stops at eight banks because higher values
address the RTC registers, and a rumble MBC5 stops at eight because the next
bit of the same register drives the motor. On a cartridge with no MBC the bank
number is the window slice itself, so only the two slices that name ROM count.

| Mapper | ROM banks | RAM banks |
| --- | --- | --- |
| ROM without MBC | 2 | 1 |
| MBC1 | 128 | 4 |
| MBC2 | 16 | 1 (internal) |
| MBC3/MBC30 | 128 | 8 |
| MBC5 | 512 | 16, or 8 with rumble |
| Game Boy Camera | 64 | 16 |
| HuC1 | 64 | 4 |

Before any bulk transfer, the bank count declared by the header is checked
against this table, and a cartridge declaring more is refused with
`TRPAK_ERR_UNSUPPORTED_CARTRIDGE`. The alternative would be a partial image, or
— on a cartridge with no MBC — a silently wrong one, since the window would
have been pointed at Game Boy `0x8000` and beyond, which is not ROM.

## Mapper support

| Family | Status | Notes |
| --- | --- | --- |
| ROM without MBC | Implemented | Non-banked ROM and RAM are supported. |
| MBC1 | Implemented | Full 128 banks (2 MiB). Banks `0x20`, `0x40`, and `0x60` are read through the fixed window in advanced banking mode, since the 5-bit register cannot select them. Multicart (MBC1M) wiring is not detected. |
| MBC2 | Implemented | 512-byte internal RAM is normalized to the lower nibble. |
| MMM01 | Detected, not implemented | Initialization returns an unsupported-cartridge error. |
| MBC3/MBC30 | ROM/RAM implemented | MBC30 supports all eight banks of a 64 KiB save. RTC is not controlled yet. |
| MBC4 | Detected, not implemented | Explicitly rejected; it does not reuse the MBC5 sequence. |
| MBC5 | ROM/RAM implemented | Writes the lower eight bits to `0x2000` and the 9th bit to `0x3000`; on rumble cartridges, RAM is limited to eight banks, so a rumble header declaring 128 KiB of save is refused rather than backed up by halves. There is no rumble API. |
| Game Boy Camera | Experimental | Basic ROM/RAM support; camera registers have no API. |
| HuC1 | Partial and experimental | Uses its own six-bit ROM register and an independent RAM bank; dumps above 64 banks are rejected. |
| TAMA5 and HuC3 | Detected, not implemented | The header is decoded so a caller can report what is inserted; initialization returns an unsupported-cartridge error, and there is no banking. |
| MBC6 and MBC7 | Not recognized | Their type bytes are rejected as unknown; no identifier exists for them. |

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
- recovery from an accessory reset in the middle of a dump, backup, or
  restore, which clears the mapper's bank latch and re-locks cartridge RAM
  while the status still reports a healthy cartridge;
- alignment and bounds of 32-byte accesses;
- explicit rejection of an unimplemented mapper;
- a full 2 MiB dump against a mock that emulates the real MBC1 registers,
  covering banks `0x20`, `0x40`, and `0x60`, RAM banking, and the rejection of
  headers claiming more banks than MBC1 can address;
- decoding of the header edge cases: a RAM claim with a zero size code, MBC2's
  implicit RAM, and the detected-but-unsupported TAMA5 and HuC3 types;
- refusal of every mapper's over-large bank count before any data moves, with
  `bytes_read` left at zero;
- that a rejected RAM bank leaves cartridge RAM locked.

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
- Recognize MBC6 (`0x20`) and MBC7 (`0x22`), which currently parse as unknown
  cartridge types.
- Reduce the per-block readiness poll, which currently doubles the number of
  Joybus transactions a dump costs. The poll is also what detects an accessory
  reset, so any change has to keep that signal.
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
