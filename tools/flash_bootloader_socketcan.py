#!/usr/bin/env python3

import argparse
import binascii
import socket
import struct
import sys
import time
from pathlib import Path


APP_START_ADDR = 0x08003000
APP_END_ADDR = 0x0801F800
APP_MANIFEST_ADDR = 0x0801F7C0
APP_MANIFEST_FORMAT = "<IHHIIIIII"
APP_MANIFEST_MAGIC = 0x50414256
APP_MANIFEST_VERSION = 1
APP_MANIFEST_BOARD_ID = 0x31444256
APP_CONFIG_ABI = 0x44AAABFF
APP_BOOT_PROTOCOL = 1
BOOT_CMD_START = 1
BOOT_CMD_DATA = 2
BOOT_CMD_DONE = 3
BL_STATUS_DONE = 0xD0
BL_STATUS_ERR = 0xE0
BL_ACK_ID_MASK = 0x400
CAN_EFF_FLAG = 0x80000000
CAN_EFF_MASK = 0x1FFFFFFF
CAN_SFF_MASK = 0x7FF
CAN_RAW_FILTER = getattr(socket, "CAN_RAW_FILTER", 1)
CAN_RAW_FD_FRAMES = getattr(socket, "CAN_RAW_FD_FRAMES", 5)
CANFD_BRS = 0x01
CANFD_FDF = 0x04  # required to mark frame as CAN FD in SocketCAN
CAN_FRAME_FORMAT = "=IB3x8s"
CAN_FRAME_SIZE = struct.calcsize(CAN_FRAME_FORMAT)
CANFD_FRAME_FORMAT = "=IBBBB64s"
CANFD_FRAME_SIZE = struct.calcsize(CANFD_FRAME_FORMAT)
# VBBoot commits data to flash in 8-byte doublewords.  An exact 8-byte payload
# avoids carrying a partial internal buffer across CAN transfers.
FD_DATA_CHUNK_SIZES = (7, 11, 15, 19, 23, 31, 47, 63)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Flash VBBoot target over raw SocketCAN CAN FD")
    parser.add_argument("--hex", required=True, help="Path to Intel HEX firmware file")
    parser.add_argument("--channel", default="can0", help="SocketCAN interface name")
    parser.add_argument("--node-id", default="0x444", help="Bootloader command CAN ID")
    parser.add_argument(
        "--id-format",
        choices=("standard", "extended"),
        default="standard",
        help="CAN ID format used by the bootloader",
    )
    parser.add_argument("--ack-timeout", type=float, default=1.5, help="ACK timeout in seconds")
    parser.add_argument(
        "--start-ack-timeout",
        type=float,
        default=0.08,
        help="ACK timeout for each START probe; use a short value to catch the cold-start recovery window",
    )
    parser.add_argument(
        "--start-retry-delay",
        type=float,
        default=0.02,
        help="Delay between failed START probes in seconds",
    )
    parser.add_argument(
        "--data-chunk-size",
        type=int,
        default=63,
        help="DATA bytes per command frame, excluding the command byte",
    )
    parser.add_argument(
        "--inter-frame-delay-ms",
        type=float,
        default=0.0,
        help="Delay after each successful ACK before sending the next frame",
    )
    parser.add_argument("--start-retries", type=int, default=200, help="Retries for the START handshake")
    parser.add_argument("--brs", action="store_true", help="Enable CAN FD bitrate switching")
    parser.add_argument("--dry-run", action="store_true", help="Validate the image and print the transfer plan without opening CAN")
    parser.add_argument("--progress-interval", type=float, default=2.0, help="Progress reporting interval in seconds")
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
        if len(record) < 5:
            raise ValueError(f"Invalid HEX line {line_no}: too short")

        count = record[0]
        offset = (record[1] << 8) | record[2]
        rec_type = record[3]
        payload = record[4 : 4 + count]
        checksum = record[4 + count] if len(record) > (4 + count) else None

        if checksum is None:
            raise ValueError(f"Invalid HEX line {line_no}: missing checksum")
        if ((sum(record[:-1]) + checksum) & 0xFF) != 0:
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
    size = (max_addr - APP_START_ADDR) + 1
    image = bytearray([0xFF] * size)
    for addr, value in data_map.items():
        image[addr - APP_START_ADDR] = value
    return bytes(image)


def validate_application_manifest(image: bytes) -> dict[str, int]:
    offset = APP_MANIFEST_ADDR - APP_START_ADDR
    manifest_size = struct.calcsize(APP_MANIFEST_FORMAT)
    if len(image) < offset + manifest_size:
        raise ValueError(
            "application compatibility manifest is missing; refusing to erase a drive with an unversioned image"
        )

    fields = struct.unpack_from(APP_MANIFEST_FORMAT, image, offset)
    names = (
        "magic",
        "format_version",
        "header_size",
        "board_id",
        "config_abi",
        "boot_protocol",
        "app_start",
        "app_end",
        "flags",
    )
    manifest = dict(zip(names, fields))
    expected = {
        "magic": APP_MANIFEST_MAGIC,
        "format_version": APP_MANIFEST_VERSION,
        "header_size": manifest_size,
        "board_id": APP_MANIFEST_BOARD_ID,
        "config_abi": APP_CONFIG_ABI,
        "boot_protocol": APP_BOOT_PROTOCOL,
        "app_start": APP_START_ADDR,
        "app_end": APP_END_ADDR,
    }
    mismatches = [
        f"{name}=0x{manifest[name]:X} expected=0x{value:X}"
        for name, value in expected.items()
        if manifest[name] != value
    ]
    if mismatches:
        raise ValueError("incompatible application manifest: " + ", ".join(mismatches))
    return manifest


def normalize_can_id(can_id: int, extended: bool) -> int:
    return can_id & (CAN_EFF_MASK if extended else CAN_SFF_MASK)


def wire_can_id(can_id: int, extended: bool) -> int:
    normalized = normalize_can_id(can_id, extended)
    return normalized | (CAN_EFF_FLAG if extended else 0)


def make_canfd_frame(can_id: int, payload: bytes, brs: bool, extended: bool) -> bytes:
    if len(payload) > 64:
        raise ValueError("payload too large for CAN FD")
    flags = CANFD_FDF | (CANFD_BRS if brs else 0)
    padded = payload + bytes(64 - len(payload))
    return struct.pack(CANFD_FRAME_FORMAT, wire_can_id(can_id, extended), len(payload), flags, 0, 0, padded)


def recv_canfd_frame(sock: socket.socket, timeout: float) -> tuple[int, bytes]:
    sock.settimeout(timeout)
    raw = sock.recv(CANFD_FRAME_SIZE)
    if len(raw) == CAN_FRAME_SIZE:
        can_id, dlc, payload = struct.unpack(CAN_FRAME_FORMAT, raw)
        return can_id & (CAN_EFF_MASK if (can_id & CAN_EFF_FLAG) else CAN_SFF_MASK), payload[:dlc]
    if len(raw) == CANFD_FRAME_SIZE:
        can_id, length, _flags, _res0, _res1, payload = struct.unpack(CANFD_FRAME_FORMAT, raw)
        return can_id & (CAN_EFF_MASK if (can_id & CAN_EFF_FLAG) else CAN_SFF_MASK), payload[:length]
    raise RuntimeError(f"unexpected CAN frame size: {len(raw)}")


def open_can_socket(channel: str, ack_id: int, extended: bool) -> socket.socket:
    sock = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    sock.setsockopt(socket.SOL_CAN_RAW, CAN_RAW_FD_FRAMES, 1)
    filter_id = wire_can_id(ack_id, extended)
    filter_mask = (CAN_EFF_FLAG | CAN_EFF_MASK) if extended else CAN_SFF_MASK
    sock.setsockopt(socket.SOL_CAN_RAW, CAN_RAW_FILTER, struct.pack("II", filter_id, filter_mask))
    sock.bind((channel,))
    return sock


def send_wait_ack(
    sock: socket.socket,
    command_id: int,
    ack_id: int,
    payload: bytes,
    timeout_s: float,
    brs: bool,
    extended: bool,
    inter_frame_delay_s: float,
) -> None:
    sock.send(make_canfd_frame(command_id, payload, brs, extended))
    deadline = time.monotonic() + timeout_s

    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"ACK timeout for payload {payload[:8].hex()}")
        frame_id, frame_payload = recv_canfd_frame(sock, remaining)
        if frame_id != ack_id:
            continue
        if not frame_payload:
            raise RuntimeError("empty ACK payload")
        if frame_payload[0] == BL_STATUS_DONE:
            if inter_frame_delay_s > 0.0:
                time.sleep(inter_frame_delay_s)
            return
        if frame_payload[0] == BL_STATUS_ERR:
            raise RuntimeError(f"bootloader returned BL_STATUS_ERR for payload {payload[:8].hex()}")


def main() -> int:
    args = parse_args()
    extended = args.id_format == "extended"
    command_id = normalize_can_id(int(str(args.node_id), 0), extended)
    ack_id = normalize_can_id(command_id | BL_ACK_ID_MASK, extended)

    if args.data_chunk_size not in FD_DATA_CHUNK_SIZES:
        raise ValueError(
            "--data-chunk-size must be one of "
            + ", ".join(str(size) for size in FD_DATA_CHUNK_SIZES)
            + " so cmd+data matches a valid CAN FD DLC exactly"
        )

    image = load_intel_hex(Path(args.hex))
    manifest = validate_application_manifest(image)
    crc32 = binascii.crc32(image) & 0xFFFFFFFF
    total_size = len(image)

    id_width = 8 if extended else 3
    print(
        f"channel={args.channel} id_format={args.id_format} "
        f"cmd_id=0x{command_id:0{id_width}X} ack_id=0x{ack_id:0{id_width}X}"
    )
    print(
        f"image_size={total_size} crc32=0x{crc32:08X} chunk={args.data_chunk_size} "
        f"brs={int(args.brs)} pace_ms={args.inter_frame_delay_ms:.3f}"
    )
    print(
        f"manifest=ok board_id=0x{manifest['board_id']:08X} "
        f"config_abi=0x{manifest['config_abi']:08X} boot_protocol={manifest['boot_protocol']}"
    )

    total_frames = (total_size + args.data_chunk_size - 1) // args.data_chunk_size
    print(f"transfer_plan_frames={total_frames} dry_run={int(args.dry_run)}")
    if args.dry_run:
        print("dry_run_complete")
        return 0

    inter_frame_delay_s = max(args.inter_frame_delay_ms, 0.0) / 1000.0

    start_timeout = max(args.start_ack_timeout, 0.01)
    start_retry_delay = max(args.start_retry_delay, 0.0)
    start_part0 = (
        bytes([BOOT_CMD_START, 0]) + struct.pack("<I", total_size) + struct.pack("<H", crc32 & 0xFFFF)
    )
    start_part1 = bytes([BOOT_CMD_START, 1]) + struct.pack("<H", (crc32 >> 16) & 0xFFFF)
    last_start_error: Exception | None = None

    # Recovery probing intentionally uses a disposable socket.  A response can
    # arrive just after a short probe timeout and remain queued.  Reusing that
    # socket would let the stale ACK satisfy START[1] or a DATA request, putting
    # the host one frame ahead of the bootloader and silently dropping data.
    with open_can_socket(args.channel, ack_id, extended) as probe_sock:
        for attempt in range(max(args.start_retries, 1)):
            try:
                send_wait_ack(
                    probe_sock,
                    command_id,
                    ack_id,
                    start_part0,
                    start_timeout,
                    args.brs,
                    extended,
                    inter_frame_delay_s,
                )
                last_start_error = None
                if attempt > 0:
                    print(f"start_retry_success={attempt + 1}")
                break
            except Exception as exc:
                last_start_error = exc
                print(f"start_retry={attempt + 1}/{max(args.start_retries, 1)} failed: {exc}")
                if start_retry_delay > 0.0:
                    time.sleep(start_retry_delay)
    if last_start_error is not None:
        raise last_start_error

    # Closing probe_sock discards all delayed probe ACKs.  Repeat both START
    # parts on a fresh socket before erasing/writing the application.
    with open_can_socket(args.channel, ack_id, extended) as sock:
        send_wait_ack(
            sock,
            command_id,
            ack_id,
            start_part0,
            args.ack_timeout,
            args.brs,
            extended,
            inter_frame_delay_s,
        )
        send_wait_ack(
            sock,
            command_id,
            ack_id,
            start_part1,
            args.ack_timeout,
            args.brs,
            extended,
            inter_frame_delay_s,
        )

        offset = 0
        frame_index = 0
        transfer_started = time.monotonic()
        next_progress = transfer_started
        while offset < total_size:
            chunk = image[offset : offset + args.data_chunk_size]
            send_wait_ack(
                sock,
                command_id,
                ack_id,
                bytes([BOOT_CMD_DATA]) + chunk,
                args.ack_timeout,
                args.brs,
                extended,
                inter_frame_delay_s,
            )
            offset += len(chunk)
            frame_index += 1
            now = time.monotonic()
            if now >= next_progress or offset == total_size:
                elapsed = max(now - transfer_started, 1e-6)
                rate = offset / elapsed
                eta = (total_size - offset) / rate if rate > 0.0 else float("inf")
                print(
                    f"data_progress={offset}/{total_size} frames={frame_index}/{total_frames} "
                    f"rate_Bps={rate:.1f} eta_s={eta:.1f}",
                    flush=True,
                )
                next_progress = now + max(args.progress_interval, 0.1)

        send_wait_ack(
            sock,
            command_id,
            ack_id,
            bytes([BOOT_CMD_DONE]),
            args.ack_timeout,
            args.brs,
            extended,
            inter_frame_delay_s,
        )

    print("flash_complete")
    print("post_flash_verification_required=application_node_and_safe_state")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"flash_failed: {exc}", file=sys.stderr)
        raise
