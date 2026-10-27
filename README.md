# VBBoot

`VBBoot` is a minimal CAN bootloader for STM32G431.

The bootloader:
- checks application validity at startup;
- exposes a 1-second CAN recovery window before starting a valid application;
- otherwise stays in boot mode and accepts firmware frames using the configured
  Classic CAN or CAN FD/BRS transport;
- writes firmware to flash at `0x08003000..0x0801F7FF`;
- reserves `0x0801F800..0x0801FFFF` for persistent boot transport metadata.

## Recovery window

On a normal reset with a valid application and no explicit boot request, VBBoot
listens on the drive's bootloader CAN ID for one second. A valid
`BOOT_CMD_START` part 0 keeps the device in the bootloader so a broken
application can be replaced without ST-Link.

If no update begins, VBBoot stores a one-shot launch marker and performs
`NVIC_SystemReset()`. On the clean reset it consumes the marker and starts the
application before initializing FDCAN. This avoids carrying bootloader FDCAN
state into VBDrive.

When VBDrive explicitly requests the bootloader, the requested node ID and CAN
timing are stored with value/inverse validation in the final flash page. Future
recovery windows therefore use the drive's own boot ID even after a cold power
cycle. The shared `0x444` fallback is used only before a drive has been
provisioned once.

## Boot Transport (CAN)

- CAN ID is the resolved bootloader command ID.
- CAN ID format is selected at build time with `BOOTLOADER_FDCAN_ID_FORMAT=standard|extended`.
- ACK ID is `command_id | 0x400`, masked to the selected identifier width.
- ACK payload is 1 byte: `0xD0` (done) or `0xE0` (error).

### Supported commands

- `BOOT_CMD_START` (`0x01`)
  - Part 0 frame: `[0x01, 0x00, size_u32_le(4), crc32_low16_le(2)]`
  - Part 1 frame: `[0x01, 0x01, crc32_high16_le(2)]`
- `BOOT_CMD_DATA` (`0x02`)
  - Frame: `[0x02, data...]`
  - Data is buffered and written to flash in 8-byte aligned chunks.
- `BOOT_CMD_DONE` (`0x03`)
  - Finalize, verify size + CRC32, ACK and perform a system reset on success.
  - The reset prevents pending CAN interrupts and peripheral state from leaking
    from the bootloader into the newly flashed application.

`BOOT_CMD_GET_ID` enum value exists in code (`0x05`) but is not handled in transport state machine.

## Transport Config Sources

VBBoot resolves CAN transport config in this order:

1. Backup register transport config, if present and valid.
2. EEPROM config, if `BOOTLOADER_USE_EEPROM_CONFIG=ON`, EEPROM is readable, `type_id` matches and fields are valid.
3. Defaults.

The backup register transport config is kept for future/debug flows. If it is present and valid, it overrides EEPROM.

### EEPROM Config

The VBDrive parent build supplies `LIBVOLTBRO_DIR` automatically. Standalone
builds use `Drivers/libvoltbro` by default; it must contain the shared header,
or supply an explicit `-DLIBVOLTBRO_DIR=...`.

When compiled with `BOOTLOADER_USE_EEPROM_CONFIG=ON`, VBBoot reads the EEPROM config prefix from external I2C EEPROM:

- I2C bus: `I2C2` (`hi2c2`)
- EEPROM device address: `0x50`
- EEPROM offset: CMake `VB_CONFIG_ADDRESS`, default `0x0000`
- Required common `type_id`: `0x01234567`
- Format: the 28-byte `BaseConfigData` in `voltbro/config/base_config.h`
- `node_id`, CAN baud indexes, `serial_baud`, and `name` belong to this common block.

VBBoot includes that header directly. It does not inspect the application config
or its type ID. UART diagnostics load `serial_baud` before the first message;
missing or invalid common data falls back to 115200. The supported Serial rates
are 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, and 1000000.

Supported nominal baud enum values map to prescalers as in libvoltbro:

- `0`: 62.5 kbit/s -> `16`
- `1`: 125 kbit/s -> `8`
- `2`: 250 kbit/s -> `4`
- `3`: 500 kbit/s -> `2`
- `4`: 1000 kbit/s -> `1`

Supported data baud enum values:

- `0`: 1000 kbit/s -> `8`
- `1`: 2000 kbit/s -> `4`
- `2`: 4000 kbit/s -> `2`
- `3`: 8000 kbit/s -> `1`

If EEPROM support is disabled, EEPROM is unavailable, read fails, `type_id` mismatches, `node_id` is out of range, or baud enum values are invalid, VBBoot falls back to defaults.

Defaults:

- CAN command ID: `DEFAULT_NODE_ID` (`0x444`)
- nominal prescaler: `1`
- data prescaler: `1`
- frame format: always `FDCAN_FRAME_FD_BRS`

## Build

Use CMake presets (toolchain is configured in `cmake/gcc-arm-none-eabi.cmake`):

```bash
cmake --preset RelWithDebInfo
cmake --build build/RelWithDebInfo
```

Or explicitly:

```bash
cmake -S . -B build/RelWithDebInfo -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake \
  -DBOOTLOADER_FDCAN_ID_FORMAT=extended \
  -DBOOTLOADER_USE_EEPROM_CONFIG=ON \
  -DLIBVOLTBRO_DIR=/path/to/libvoltbro \
  -DVB_CONFIG_ADDRESS=0
cmake --build build/RelWithDebInfo
```

### STM32G474 target

The default target remains STM32G431. Integrators can select STM32G474 without
changing the VBDrive build by passing target-specific cache values:

```bash
cmake -S . -B build/g474 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake \
  -DBOOTLOADER_MCU=STM32G474xx \
  -DBOOTLOADER_STARTUP_FILE=/path/to/startup_stm32g474xx.s \
  -DBOOTLOADER_APP_END=0x08080000UL \
  -DBOOTLOADER_RAM_END=0x20020000UL \
  -DBOOTLOADER_FDCAN_CLOCK_SOURCE=PLL \
  -DBOOTLOADER_USE_EEPROM_CONFIG=OFF
```

The G474 selection generates a 512 KiB/128 KiB bootloader linker layout while
the default G431 layout, clocks, startup file, and validation bounds remain
unchanged.

Post-build artifacts:
- `VBBoot.elf`
- `VBBoot.hex`
- `VBBoot.bin`

## Flash layout

- Bootloader flash region: `0x08000000`, length `12K` (see linker script).
- Application start: `0x08003000` (`APP_START_ADDR`).
- Application end: `0x0801F800` (exclusive).
- Boot metadata page: `0x0801F800`, length `2K`.

## Python CAN test

Test script is in `tests/test_bootloader_fdcan.py`.

Example:

```bash
pytest tests/test_bootloader_fdcan.py \
  --hex=/path/to/app.hex \
  --can-iface=socketcan \
  --can-channel=can0 \
  --ack-timeout=0.8 \
  --node-id=0x444
```

The SocketCAN uploader uses a 47-byte DATA chunk by default (one command byte
plus 47 firmware bytes in a 48-byte CAN FD frame). Other supported chunks,
including the maximum 63-byte DATA payload, can
be selected with `--data-chunk-size` when the complete transport path supports
64-byte CAN FD payloads reliably.

Streaming DATA without per-frame ACKs is the default. START and DONE remain
acknowledged, and DONE validates the complete size and CRC32. The default 3 ms
inter-frame delay prevents receive FIFO overflow while internal flash is being
programmed. Use `--ack-each-frame` for per-frame acknowledgement.
