# Changelog

All notable changes to `libtrpak` are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
The project carries no version number and no git tags yet, so the sections
below are anchored to the initial import rather than to releases. Adopting
[Semantic Versioning](https://semver.org/) means tagging a first release and
renaming `Unreleased` to that version.

## [Unreleased]

### Fixed

- **Dumps of cartridges without an MBC could be silently wrong.**
  `trpak_select_rom_bank()` wrote the bank number straight into the window's
  slice register for `TRPAK_MAPPER_NONE`. Any bank above 1 therefore pointed
  the window at Game Boy `0x8000` and beyond — VRAM and cartridge RAM, not ROM
  — and `trpak_read_rom()` returned `TRPAK_OK` over whatever came back. Such a
  header is now refused with `TRPAK_ERR_UNSUPPORTED_CARTRIDGE`.

- **An accessory reset mid-transfer corrupted the result without reporting it.**
  The public constants for reset-in-progress (`0x04`) and reset-detected
  (`0x08`) were swapped, and the latter was not consulted. In addition, checking
  status only before a block left a race in which the reset could happen before
  the data transaction. A reset leaves the accessory powered, present, and
  ready, while the mapper returns to its power-on state: the selected bank is
  gone and cartridge RAM re-locks. All bulk paths now poll through booting,
  check status on both sides of each transaction, restore mapper state, and
  retry the affected block. Repeated resets are bounded by a timeout.

- **MBC1M multicarts were dumped with conventional MBC1 wiring.** Their main
  bank register has four effective bits and the upper register supplies bits
  4-5, so banks above `0x0F` became duplicates of lower content. One MiB MBC1
  cartridges are now probed for the repeated valid header/logo at bank `0x10`,
  then traversed with the 64-bank MBC1M layout. Their RAM is limited to the one
  fixed bank the wiring can expose.

- **Modern CGB titles included the manufacturer code.** Headers with the
  four-byte field at `0x013F`-`0x0142` now expose only the 11-byte title through
  `trpak_cart.title`. Since the header has no layout-version marker, four
  printable bytes identify the newer form; zero-padded early CGB and legacy
  16-byte layouts retain their longer interpretation.

- **Bank counts beyond the mapper's reach aborted mid-transfer.** Only MBC1
  (above 128 banks) and HuC1 (above 64) were checked. MBC2 above 16 ROM banks,
  the Camera above 64, MBC3 above 8 RAM banks, and a rumble MBC5 above 8 all
  ran until the first unreachable bank and returned `TRPAK_ERR_INVALID_BANK`
  from the middle of the traversal, leaving a partial image behind. A measured
  case: an `MBC5+RUMBLE+RAM+BATTERY` header declaring 128 KiB of save produced
  a 64 KiB backup. Every mapper is now validated before any data moves.

- **A rejected RAM bank could leave a battery-backed save writable.**
  `trpak_select_ram_bank()` wrote the RAM-enable magic before checking the
  MBC3 and rumble-MBC5 limits, so those two rejections returned with cartridge
  RAM still unlocked. Every range check now runs before the enable write.

- **`bytes_read` was left untouched on early returns**, contradicting its
  documented contract, so a caller inspecting it after a refusal read whatever
  the variable held before the call. It is now always written, and set to `0`
  when the operation is refused before transferring anything.

- **A cartridge declaring RAM with a size code of `0` failed initialization
  entirely** with `TRPAK_ERR_INVALID_HEADER`, which also made its ROM
  undumpable. See *Changed* below for the new behavior.

- **TAMA5 (`0xFD`) and HuC3 (`0xFE`) were never decoded**, even though
  `TRPAK_MAPPER_TAMA5` and `TRPAK_MAPPER_HUC3` existed and the documentation of
  `trpak_mapper_is_supported()` claimed the parser recognized them. Their type
  bytes were rejected as unknown.

- **The MBC1 banking-mode register was written at two different window
  addresses** — Game Boy `0x6016` from the ROM path and `0x6000` from the RAM
  path. Both land inside the `0x6000`-`0x7FFF` region, so behavior was
  unaffected, but the two paths disagreed. Unified on `0x6000`.

### Changed

- A cartridge type byte that claims RAM while the size code at `0x0149` is `0`
  is now decoded as RAM-less — `ram`, `rambanks`, and `ramsize` all zero —
  instead of failing the parse. The ROM of such a cartridge stays dumpable, and
  the save entry points still refuse safely with `TRPAK_ERR_NO_RAM`. `battery`
  keeps reflecting the type byte, so the disagreement remains visible. MBC2 is
  unaffected: its 512 half-bytes come from the mapper, not from the size code.

- Over-large bank counts now fail with `TRPAK_ERR_UNSUPPORTED_CARTRIDGE` before
  the first transfer, where they previously failed with
  `TRPAK_ERR_INVALID_BANK` partway through.

- `trpak_parse_cartridge_header()` accepts type bytes `0xFD` and `0xFE`, filling
  the metadata so a caller can report what is inserted. `trpak_init()` still
  refuses both, since neither has a banking path.

### Added

- Regression tests for every fix above: header edge cases (RAM claim with a
  zero size code, MBC2's implicit RAM, TAMA5 and HuC3), recovery from a reset
  during a dump, a backup, and a restore (including the pre-flight race), a
  byte-exact MBC1M dump, modern CGB title decoding, refusal of over-large bank
  counts with `bytes_read` at zero, and the RAM-stays-locked guarantee. Each
  was confirmed to fail against the pre-fix library.

- The MBC1 test mock can now simulate a Transfer Pak reset: it clears the bank
  registers, the banking mode, and the RAM-enable latch, then reports
  `TRPAK_STATUS_WAS_RESET` once, matching the read-and-clear behavior of the
  hardware bit.

- README sections covering the addressing model — the
  `slice * 0x4000 + (address - 0xC000)` formula, the slice table, and the
  window address of every MBC register the library writes — the Transfer Pak
  status bits, header decoding rules, and the per-mapper bank limits.

### Internal

No behavior change in this group.

- Mapper limits moved into `mapper_max_rom_bank()`, `mapper_max_ram_bank()`,
  and `bank_count_fits_mapper()` as a single source of truth. They had been
  spread across three functions that disagreed with each other, which was the
  shared root cause of three of the bugs above.

- Named constants (`TP_REG_RAM_ENABLE`, `TP_REG_ROM_BANK`,
  `TP_REG_ROM_BANK_ALT`, `TP_REG_ROM_BANK_HIGH`, `TP_REG_RAM_BANK`,
  `TP_REG_MBC1_MODE`) replace the raw window addresses, and
  `select_window_slice()` replaces the open-coded writes to the bank register,
  so each MBC sequence reads as "select a slice, poke a named register".

- The MBC2, MBC3, and Camera ROM-banking sequences, which were byte-for-byte
  identical and differed only in their bank limit, collapsed into one case.

- Eight numbered section banners in `libtrpak.c`, matching the layering the
  file header describes.

- The `transfer_data` file-scope scratch buffer became a local in
  `trpak_init()`, and the redundant `original_bank` copy in
  `trpak_select_rom_bank()` was removed.

## Initial import - 2026-08-16

### Added

- Initial `libtrpak` implementation, developed from
  [libgbpak](https://github.com/saturnu/libgbpak) with its own `trpak` API,
  validations, tests, and organization.
- Transfer Pak power, access-mode, and status handling, with readiness polling.
- Game Boy header parsing and checksum validation, isolated from hardware.
- ROM and RAM bank selection for ROM-only, MBC1, MBC2, MBC3/MBC30, MBC5,
  Game Boy Camera, and HuC1 cartridges.
- ROM dumping and save backup/restore, into caller buffers or through DMA
  callbacks, with read-after-write verification.
- Injectable transport backend via `trpak_configure_io()`, plus the default
  libdragon backend and optional EverDrive 64 DMA.
- Host test suite with a simulated Transfer Pak, and a compile-only check of
  the default backend against stub headers.
