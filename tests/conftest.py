import pytest
from pathlib import Path


APP_START_ADDR = 0x08003000
APP_END_ADDR = 0x0801F800
BL_ACK_ID_MASK = 0x400
FD_DATA_CHUNK_SIZES = (7, 11, 15, 19, 23, 31, 47, 63)


def pytest_addoption(parser: pytest.Parser) -> None:
    parser.addoption(
        "--hex",
        action="store",
        default=None,
        help="Path to Intel HEX firmware file",
    )
    parser.addoption("--can-iface", action="store", default="socketcan", help="python-can interface")
    parser.addoption("--can-channel", action="store", default="can0", help="CAN channel, e.g. can0")
    parser.addoption("--can-fd", action="store", default="true", help="Use CAN FD frames: true/false")
    parser.addoption("--can-brs", action="store", default="false", help="Use bitrate switching for CAN FD: true/false")
    parser.addoption("--data-chunk-size", action="store", default="11", help="Bootloader DATA payload size without command byte")
    parser.addoption("--ack-timeout", action="store", default="0.8", help="ACK timeout in seconds")
    parser.addoption(
        "--node-id",
        action="store",
        default="0x444",
        help="Bootloader CAN node ID in range 0x01..0x7FF (dec or hex, e.g. 5 or 0x444)",
    )


@pytest.fixture(scope="session")
def fw_image(pytestconfig: pytest.Config) -> bytes:
    hex_path = pytestconfig.getoption("--hex")
    if not hex_path:
        pytest.skip("Pass firmware path via --hex=/path/to/firmware.hex")

    image = _load_intel_hex(Path(hex_path))
    if not image:
        pytest.fail("HEX file resolved to empty image within app flash range")
    return image


@pytest.fixture(scope="session")
def flashing_params(pytestconfig: pytest.Config) -> dict:
    ack_timeout = float(pytestconfig.getoption("--ack-timeout"))
    raw_node_id = str(pytestconfig.getoption("--node-id"))
    node_id = int(raw_node_id, 0)
    data_chunk_size = int(pytestconfig.getoption("--data-chunk-size"), 0)
    use_fd = _parse_bool(pytestconfig.getoption("--can-fd"), "--can-fd")
    use_brs = _parse_bool(pytestconfig.getoption("--can-brs"), "--can-brs")

    if ack_timeout <= 0:
        pytest.fail("--ack-timeout must be > 0")
    if node_id <= 0 or node_id > 0x7FF:
        pytest.fail("--node-id must be in range 1..0x7FF")
    if use_brs and not use_fd:
        pytest.fail("--can-brs=true requires --can-fd=true")
    if data_chunk_size <= 0 or data_chunk_size > (63 if use_fd else 7):
        pytest.fail("--data-chunk-size is out of range for the selected CAN mode")
    if use_fd and data_chunk_size not in FD_DATA_CHUNK_SIZES:
        pytest.fail(
            "--data-chunk-size for CAN FD must be one of "
            + ", ".join(str(size) for size in FD_DATA_CHUNK_SIZES)
            + " so cmd+payload matches a valid FD DLC without implicit padding"
        )

    return {
        "iface": pytestconfig.getoption("--can-iface"),
        "channel": pytestconfig.getoption("--can-channel"),
        "ack_timeout": ack_timeout,
        "node_id": node_id,
        "ack_id": (node_id | BL_ACK_ID_MASK) & 0x7FF,
        "use_fd": use_fd,
        "use_brs": use_brs,
        "data_chunk_size": data_chunk_size,
    }


def _parse_bool(raw_value: str, option_name: str) -> bool:
    value = str(raw_value).strip().lower()
    if value in ("1", "true", "yes", "on"):
        return True
    if value in ("0", "false", "no", "off"):
        return False
    pytest.fail(f"{option_name} must be true/false")


def _load_intel_hex(path: Path) -> bytes:
    if not path.exists():
        pytest.fail(f"HEX file not found: {path}")

    addr_base = 0
    data_map: dict[int, int] = {}

    for line_no, raw in enumerate(path.read_text(encoding="ascii").splitlines(), start=1):
        line = raw.strip()
        if not line:
            continue
        if not line.startswith(":"):
            pytest.fail(f"Invalid HEX line {line_no}: missing ':'")

        try:
            rec = bytes.fromhex(line[1:])
        except ValueError:
            pytest.fail(f"Invalid HEX line {line_no}: non-hex characters")

        if len(rec) < 5:
            pytest.fail(f"Invalid HEX line {line_no}: too short")

        count = rec[0]
        offset = (rec[1] << 8) | rec[2]
        rec_type = rec[3]
        payload = rec[4 : 4 + count]
        checksum = rec[4 + count] if len(rec) > (4 + count) else None

        if checksum is None:
            pytest.fail(f"Invalid HEX line {line_no}: missing checksum")
        if ((sum(rec[:-1]) + checksum) & 0xFF) != 0:
            pytest.fail(f"Invalid HEX line {line_no}: checksum mismatch")

        if rec_type == 0x00:  # data
            abs_addr = addr_base + offset
            for i, b in enumerate(payload):
                current = abs_addr + i
                if APP_START_ADDR <= current < APP_END_ADDR:
                    data_map[current] = b
        elif rec_type == 0x01:  # EOF
            break
        elif rec_type == 0x04:  # extended linear address
            if count != 2:
                pytest.fail(f"Invalid HEX line {line_no}: bad type-04 length")
            addr_base = ((payload[0] << 8) | payload[1]) << 16
        elif rec_type in (0x02, 0x03, 0x05):
            # Non-essential records for binary image extraction.
            continue
        else:
            pytest.fail(f"Unsupported HEX record type 0x{rec_type:02X} at line {line_no}")

    if not data_map:
        return b""

    max_addr = max(data_map.keys())
    size = (max_addr - APP_START_ADDR) + 1
    image = bytearray([0xFF] * size)
    for addr, val in data_map.items():
        image[addr - APP_START_ADDR] = val
    return bytes(image)

