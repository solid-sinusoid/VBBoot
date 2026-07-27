import importlib.util
from pathlib import Path


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
