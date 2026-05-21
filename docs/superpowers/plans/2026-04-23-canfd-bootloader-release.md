# CAN FD Bootloader Release Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** make bootloader flashing of `VBDrive` over the current CAN FD bus reproducible on hardware using a release-safe protocol and host flasher.

**Architecture:** keep the MCU bootloader simple and deterministic: one command ID, one separate ACK ID, one fixed CAN FD no-BRS mode, and a dedicated raw SocketCAN host flasher. Keep the existing pytest flow only as a secondary harness, not the primary release flashing path.

**Tech Stack:** STM32G4 HAL, FDCAN, SocketCAN, Python raw sockets, pytest, ST-Link

---

### Task 1: Finalize MCU Transport Contract

**Files:**
- Modify: `App/app.h`
- Modify: `App/communications.c`
- Modify: `App/state_manager.c`

- [ ] Add a derived ACK ID helper and use it consistently for bootloader TX ACK frames.
- [ ] Keep command RX on the bootloader node ID only.
- [ ] Keep transport fixed to CAN FD without BRS for the release path.
- [ ] Rebuild and confirm the bootloader still fits in the flash budget.

### Task 2: Add Release Flasher

**Files:**
- Create: `tools/flash_bootloader_socketcan.py`

- [ ] Implement a raw SocketCAN flasher that sends command frames and waits only for ACK frames on the separate ACK ID.
- [ ] Enforce valid CAN FD chunk sizes that avoid DLC padding ambiguity.
- [ ] Print enough progress and failure context for hardware diagnosis.

### Task 3: Align Test Harness

**Files:**
- Modify: `tests/conftest.py`
- Modify: `tests/test_bootloader_fdcan.py`

- [ ] Update the pytest harness to use the same separate ACK ID contract.
- [ ] Keep the FD chunk-size validation aligned with the release flasher.

### Task 4: Hardware Verification

**Files:**
- Verify with: `build/RelWithDebInfo/VBBoot.hex`
- Verify with: `/home/vladimir/VBDrive/build/RelWithDebInfo/VBDrive.hex`

- [ ] Rebuild `VBBoot`.
- [ ] Flash `VBBoot.hex` with ST-Link.
- [ ] Run the new raw SocketCAN flasher against `VBDrive.hex`.
- [ ] Confirm application boot after `DONE`.
- [ ] Repeat the cycle to prove reflashing from a valid app remains available.
