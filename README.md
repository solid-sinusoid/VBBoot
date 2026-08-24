# VBBoot

`VBBoot` is a minimal CAN bootloader for STM32G431.

The bootloader:
- checks application validity at startup;
- перед запуском валидного приложения открывает CAN recovery-окно на 10 секунд;
- otherwise stays in boot mode and accepts firmware frames using the configured
  Classic CAN or CAN FD/BRS transport;
- writes firmware to flash at `0x08003000..0x0801F7FF`;
- reserves `0x0801F800..0x0801FFFF` for persistent boot transport metadata.

## Recovery window

При обычном reset с валидным приложением и без явного boot request VBBoot
в течение 10 секунд слушает bootloader CAN ID привода. Валидная первая часть
`BOOT_CMD_START` оставляет устройство в bootloader, чтобы повреждённое
приложение можно было заменить без ST-Link.

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

When compiled with `BOOTLOADER_USE_EEPROM_CONFIG=ON`, VBBoot reads the EEPROM config prefix from external I2C EEPROM:

- I2C bus: `I2C2` (`hi2c2`)
- EEPROM device address: `0x50`
- EEPROM offset: `0x0000`
- Required `type_id`: `0x44AAABFF`
- Prefix format is same as libvoltbro BaseConfig:

```c
typedef struct __attribute__((packed)) {
    uint8_t was_configured;
    uint8_t node_id;
    uint8_t fdcan_nominal_baud;
    uint8_t fdcan_data_baud;
    uint32_t type_id;
} BootEepromConfigPrefix;
```

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
  -DBOOTLOADER_USE_EEPROM_CONFIG=ON
cmake --build build/RelWithDebInfo
```

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
