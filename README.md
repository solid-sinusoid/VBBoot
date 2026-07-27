# VBBoot

`VBBoot` is a minimal CAN bootloader for STM32G431.

The bootloader:
- checks application validity at startup;
- exposes a 1-second CAN recovery window before starting a valid application;
- otherwise stays in boot mode and accepts firmware frames over **Classic CAN 2.0** (`FDCAN_FRAME_CLASSIC`, no FD/BRS);
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

- CAN ID is `node_id` (standard 11-bit identifier).
- RX/TX use the same CAN ID.
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

## node_id source (EEPROM)

`node_id` is read from external I2C EEPROM (same idea as VBDrive config backend):

- I2C bus: `I2C2` (`hi2c2`)
- EEPROM device address: `0x50`
- EEPROM offset: `0x0000`
- Record format:

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;      // must be NODE_ID_MAGIC (0x424C4E49)
    uint8_t node_id;     // valid range: 1..127
    uint8_t reserved[3];
} NodeIdRecord;
```

If EEPROM is not ready, read fails, `magic` is invalid, or `node_id` is out of range, bootloader uses `DEFAULT_NODE_ID` (`0x444`).

## Build

Use CMake presets (toolchain is configured in `cmake/gcc-arm-none-eabi.cmake`):

```bash
cmake --preset RelWithDebInfo
cmake --build build/RelWithDebInfo
```

Or explicitly:

```bash
cmake -S . -B build/RelWithDebInfo -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
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
