import importlib.util
from pathlib import Path
import struct


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "flash_bootloader_socketcan.py"
SPEC = importlib.util.spec_from_file_location("flash_bootloader_socketcan", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


def test_stable_chunk_sizes_match_can_fd_dlc_payloads() -> None:
    assert MODULE.FD_DATA_CHUNK_SIZES == (7, 11, 15, 19, 23, 31, 47, 63)


def test_default_uses_full_can_fd_payload(monkeypatch) -> None:
    monkeypatch.setattr(MODULE.sys, "argv", ["flash", "--hex", "candidate.hex"])
    assert MODULE.parse_args().data_chunk_size == 63


def test_default_start_probe_retries_fit_inside_legacy_recovery_window(monkeypatch) -> None:
    monkeypatch.setattr(MODULE.sys, "argv", ["flash", "--hex", "candidate.hex"])
    args = MODULE.parse_args()

    assert args.start_ack_timeout <= 0.08
    assert args.start_retry_delay <= 0.02
    assert args.start_retries >= 200


def test_final_flash_page_is_reserved_for_transaction_metadata() -> None:
    assert MODULE.APP_END_ADDR == 0x0801F800


def test_hex_loader_rejects_image_outside_application(tmp_path: Path) -> None:
    image = tmp_path / "boot.hex"
    image.write_text(":020000040800F2\n:0400000001020304F2\n:00000001FF\n", encoding="ascii")
    try:
        MODULE.load_intel_hex(image)
    except ValueError as exc:
        assert "empty image" in str(exc)
    else:
        raise AssertionError("bootloader-region image was accepted as an application")


def test_success_message_requires_runtime_verification() -> None:
    source = SCRIPT.read_text(encoding="utf-8")

    assert 'print("flash_complete")' in source
    assert 'print("post_flash_verification_required=application_node_and_safe_state")' in source


def test_recovery_probe_acks_cannot_leak_into_transfer(monkeypatch, tmp_path: Path) -> None:
    image = tmp_path / "candidate.hex"
    image.write_text(
        ":020000040800F2\n"
        ":0130000001CE\n"
        ":00000001FF\n",
        encoding="ascii",
    )

    class FakeSocket:
        def __init__(self, name: str) -> None:
            self.name = name

        def __enter__(self):
            return self

        def __exit__(self, *_args) -> None:
            return None

    sockets = [FakeSocket("probe"), FakeSocket("transfer")]
    opened = []
    sent = []

    def fake_open(*_args):
        sock = sockets[len(opened)]
        opened.append(sock.name)
        return sock

    def fake_send(sock, _command_id, _ack_id, payload, *_args):
        sent.append((sock.name, payload[0], payload[1] if len(payload) > 1 else None))

    monkeypatch.setattr(MODULE, "open_can_socket", fake_open)
    monkeypatch.setattr(MODULE, "send_wait_ack", fake_send)
    monkeypatch.setattr(
        MODULE,
        "validate_application_manifest",
        lambda _image: {
            "board_id": MODULE.APP_MANIFEST_BOARD_ID,
            "config_abi": MODULE.APP_CONFIG_ABI,
            "boot_protocol": MODULE.APP_BOOT_PROTOCOL,
        },
    )
    monkeypatch.setattr(
        MODULE.sys,
        "argv",
        ["flash", "--hex", str(image), "--start-retries", "1"],
    )

    assert MODULE.main() == 0
    assert opened == ["probe", "transfer"]
    assert sent[0] == ("probe", MODULE.BOOT_CMD_START, 0)
    assert sent[1] == ("transfer", MODULE.BOOT_CMD_START, 0)
    assert sent[2] == ("transfer", MODULE.BOOT_CMD_START, 1)
    assert all(sock_name == "transfer" for sock_name, *_ in sent[1:])


def _manifest_image(**overrides: int) -> bytes:
    values = {
        "magic": MODULE.APP_MANIFEST_MAGIC,
        "format_version": MODULE.APP_MANIFEST_VERSION,
        "header_size": struct.calcsize(MODULE.APP_MANIFEST_FORMAT),
        "board_id": MODULE.APP_MANIFEST_BOARD_ID,
        "config_abi": MODULE.APP_CONFIG_ABI,
        "boot_protocol": MODULE.APP_BOOT_PROTOCOL,
        "app_start": MODULE.APP_START_ADDR,
        "app_end": MODULE.APP_END_ADDR,
        "flags": 0,
    }
    values.update(overrides)
    image = bytearray([0xFF] * (MODULE.APP_MANIFEST_ADDR - MODULE.APP_START_ADDR))
    image.extend(struct.pack(MODULE.APP_MANIFEST_FORMAT, *values.values()))
    return bytes(image)


def test_valid_application_manifest_is_accepted() -> None:
    manifest = MODULE.validate_application_manifest(_manifest_image())
    assert manifest["config_abi"] == MODULE.APP_CONFIG_ABI


def test_incompatible_config_abi_is_rejected() -> None:
    try:
        MODULE.validate_application_manifest(_manifest_image(config_abi=0x44AAABFE))
    except ValueError as exc:
        assert "config_abi" in str(exc)
    else:
        raise AssertionError("incompatible EEPROM ABI was accepted")


def test_missing_manifest_is_rejected_before_can_is_opened(monkeypatch, tmp_path: Path) -> None:
    image = tmp_path / "unversioned.hex"
    image.write_text(
        ":020000040800F2\n"
        ":0130000001CE\n"
        ":00000001FF\n",
        encoding="ascii",
    )
    can_opened = False

    def fake_open(*_args):
        nonlocal can_opened
        can_opened = True
        raise AssertionError("CAN must not be opened for an unversioned image")

    monkeypatch.setattr(MODULE, "open_can_socket", fake_open)
    monkeypatch.setattr(MODULE.sys, "argv", ["flash", "--hex", str(image)])

    try:
        MODULE.main()
    except ValueError as exc:
        assert "manifest is missing" in str(exc)
    else:
        raise AssertionError("unversioned firmware was accepted")
    assert not can_opened
