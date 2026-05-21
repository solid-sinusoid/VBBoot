#!/usr/bin/env python3

import argparse
import binascii
import os
from pathlib import Path
import select
import struct
import sys
import termios
import time
import tty


APP_START_ADDR = 0x08003000
APP_END_ADDR = 0x08020000
BOOT_CMD_START = 1
BOOT_CMD_DATA = 2
BOOT_CMD_DONE = 3
BL_STATUS_DONE = 0xD0
BL_STATUS_ERR = 0xE0
SOF = 0xA5


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Flash VBBoot target over UART")
    parser.add_argument("--hex", required=True, help="Path to Intel HEX firmware file")
    parser.add_argument("--port", default="/dev/ttyACM0", help="UART device")
    parser.add_argument("--baud", type=int, default=19200, help="UART baud rate")
    parser.add_argument("--ack-timeout", type=float, default=3.0, help="ACK timeout in seconds")
    parser.add_argument("--chunk-size", type=int, default=63, help="DATA bytes per frame")
    parser.add_argument("--inter-frame-delay-ms", type=float, default=0.0)
    return parser.parse_args()


def load_intel_hex(path: Path) -> bytes:
    if not path.exists():
        raise FileNotFoundError(path)

    addr_base = 0
    data_map: dict[int, int] = {}
    for line_no, raw in enumerate(path.read_text(encoding="ascii").splitlines(), start=1):
        line = raw.strip()
        if not line:
            continue
        if not line.startswith(":"):
            raise ValueError(f"Invalid HEX line {line_no}: missing ':'")
        record = bytes.fromhex(line[1:])
        count = record[0]
        offset = (record[1] << 8) | record[2]
        rec_type = record[3]
        payload = record[4 : 4 + count]
        checksum = record[4 + count] if len(record) > (4 + count) else None
        if checksum is None or ((sum(record[:-1]) + checksum) & 0xFF) != 0:
            raise ValueError(f"Invalid HEX line {line_no}: checksum mismatch")

        if rec_type == 0x00:
            abs_addr = addr_base + offset
            for i, value in enumerate(payload):
                current = abs_addr + i
                if APP_START_ADDR <= current < APP_END_ADDR:
                    data_map[current] = value
        elif rec_type == 0x01:
            break
        elif rec_type == 0x04:
            if count != 2:
                raise ValueError(f"Invalid HEX line {line_no}: bad type-04 length")
            addr_base = ((payload[0] << 8) | payload[1]) << 16

    if not data_map:
        raise ValueError("HEX resolved to empty image inside app flash window")

    max_addr = max(data_map.keys())
    image = bytearray([0xFF] * ((max_addr - APP_START_ADDR) + 1))
    for addr, value in data_map.items():
        image[addr - APP_START_ADDR] = value
    return bytes(image)


def baud_constant(baud: int) -> int:
    name = f"B{baud}"
    if not hasattr(termios, name):
        raise ValueError(f"unsupported baud: {baud}")
    return getattr(termios, name)


def open_uart(port: str, baud: int) -> int:
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    tty.setraw(fd)
    attrs = termios.tcgetattr(fd)
    bconst = baud_constant(baud)
    attrs[4] = bconst
    attrs[5] = bconst
    attrs[2] |= termios.CLOCAL | termios.CREAD
    attrs[2] &= ~termios.CSTOPB
    attrs[2] &= ~termios.PARENB
    attrs[2] &= ~termios.CSIZE
    attrs[2] |= termios.CS8
    attrs[3] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    return fd


def make_frame(payload: bytes) -> bytes:
    if not 0 < len(payload) <= 64:
        raise ValueError("payload length must be 1..64")
    crc = len(payload)
    for byte in payload:
        crc ^= byte
    return bytes([SOF, len(payload)]) + payload + bytes([crc])


def send_wait_ack(fd: int, payload: bytes, timeout_s: float, inter_frame_delay_s: float) -> None:
    write_all(fd, make_frame(payload))
    deadline = time.monotonic() + timeout_s
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"ACK timeout for payload {payload[:8].hex()}")
        readable, _, _ = select.select([fd], [], [], remaining)
        if not readable:
            continue
        data = os.read(fd, 256)
        if bytes([BL_STATUS_DONE]) in data:
            if inter_frame_delay_s > 0.0:
                time.sleep(inter_frame_delay_s)
            return
        if bytes([BL_STATUS_ERR]) in data:
            raise RuntimeError(f"bootloader returned BL_STATUS_ERR for payload {payload[:8].hex()}")


def write_all(fd: int, data: bytes) -> None:
    view = memoryview(data)
    while view:
        _, writable, _ = select.select([], [fd], [], 1.0)
        if not writable:
            continue
        written = os.write(fd, view)
        view = view[written:]


def main() -> int:
    args = parse_args()
    if not 1 <= args.chunk_size <= 63:
        raise ValueError("--chunk-size must be in range 1..63")

    image = load_intel_hex(Path(args.hex))
    crc32 = binascii.crc32(image) & 0xFFFFFFFF
    inter_frame_delay_s = max(args.inter_frame_delay_ms, 0.0) / 1000.0

    print(f"port={args.port} baud={args.baud}")
    print(f"image_size={len(image)} crc32=0x{crc32:08X} chunk={args.chunk_size}")

    fd = open_uart(args.port, args.baud)
    try:
        send_wait_ack(
            fd,
            bytes([BOOT_CMD_START, 0]) + struct.pack("<I", len(image)) + struct.pack("<H", crc32 & 0xFFFF),
            max(args.ack_timeout, 8.0),
            inter_frame_delay_s,
        )
        send_wait_ack(
            fd,
            bytes([BOOT_CMD_START, 1]) + struct.pack("<H", (crc32 >> 16) & 0xFFFF),
            max(args.ack_timeout, 8.0),
            inter_frame_delay_s,
        )

        offset = 0
        frame_index = 0
        while offset < len(image):
            chunk = image[offset : offset + args.chunk_size]
            send_wait_ack(
                fd,
                bytes([BOOT_CMD_DATA]) + chunk,
                args.ack_timeout,
                inter_frame_delay_s,
            )
            offset += len(chunk)
            frame_index += 1
            if (frame_index % 128) == 0 or offset == len(image):
                print(f"data_progress={offset}/{len(image)}")

        send_wait_ack(fd, bytes([BOOT_CMD_DONE]), max(args.ack_timeout, 8.0), inter_frame_delay_s)
    finally:
        os.close(fd)

    print("flash_complete")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"flash_failed: {exc}", file=sys.stderr)
        raise
