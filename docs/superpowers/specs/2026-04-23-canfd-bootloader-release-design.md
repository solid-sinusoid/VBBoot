# CAN FD Bootloader Release Design

**Goal:** make `VBBoot` reliably flash `VBDrive` over the current CAN FD bus on real hardware and keep bootloader entry reachable from a valid application.

**Confirmed facts:**
- `VBBoot` now enters bootloader from `VBDrive` via backup-register request and reset.
- Flash erase/program path works on hardware.
- UART diagnostics and `candump` both show the bootloader receiving commands and transmitting ACKs.
- The remaining instability is on the host flashing path, not in MCU flash programming.

**Release transport contract:**
- Command CAN ID: bootloader node ID.
- ACK CAN ID: `command_id | 0x400`.
- Accepted frame mode: `CAN FD`, `BRS off`.
- `DATA` payload size must map to a valid FD DLC with no implicit padding once the command byte is included.
- Host flashing for release must use raw SocketCAN rather than the current `python-can` receive loop.

**Implementation outline:**
- Update bootloader transport to derive and use a separate ACK ID.
- Preserve UART diagnostics while keeping the image under the bootloader flash limit.
- Add a dedicated raw SocketCAN flashing utility for hardware use.
- Keep the pytest harness as a secondary tool, but align it with the same command/ACK ID contract.
- Verify end-to-end on hardware by flashing `VBDrive.hex` through the bootloader repeatedly.

**Success criteria:**
- `VBBoot` builds within the configured bootloader flash budget.
- Release flasher can program `VBDrive.hex` over `can0` without ACK timeout.
- `DONE` causes a valid jump to application.
- A valid `VBDrive` can request reboot back into bootloader and be reflashed again.
