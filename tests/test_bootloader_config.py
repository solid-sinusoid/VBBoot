from pathlib import Path
import re


def test_default_node_id_avoids_joint_collision() -> None:
    header = Path(__file__).resolve().parents[1] / "App" / "app.h"
    text = header.read_text(encoding="utf-8")
    match = re.search(r"#define\s+DEFAULT_NODE_ID\s+(0x[0-9A-Fa-f]+|\d+)U?", text)

    assert match is not None
    assert int(match.group(1), 0) == 0x444


def test_nominal_can_sample_point_matches_can0() -> None:
    source = Path(__file__).resolve().parents[1] / "App" / "state_manager.c"
    text = source.read_text(encoding="utf-8")

    seg1 = _assigned_int(text, "NominalTimeSeg1")
    seg2 = _assigned_int(text, "NominalTimeSeg2")

    assert (1 + seg1) / (1 + seg1 + seg2) == 0.75


def test_app_validity_checks_stack_and_reset_ranges() -> None:
    source = Path(__file__).resolve().parents[1] / "App" / "app.c"
    text = source.read_text(encoding="utf-8")

    assert "0x20000000UL" in text
    assert "app_sp <= 0x20008000UL" in text
    assert "APP_START_ADDR" in text
    assert "APP_END_ADDR" in text


def test_fdcan_dlc_uses_hal_dlc_indexes() -> None:
    source = Path(__file__).resolve().parents[1] / "App" / "communications.c"
    text = source.read_text(encoding="utf-8")

    assert "switch (dlc)" in text
    assert "dlc >> 16U" not in text
    assert "return (uint32_t)len;" in text
    assert "len << 16U" not in text


def test_successful_update_resets_before_starting_application() -> None:
    source = Path(__file__).resolve().parents[1] / "App" / "communications.c"
    text = source.read_text(encoding="utf-8")
    done_case = text.split("case BOOT_CMD_DONE:", maxsplit=1)[1].split(
        "default:", maxsplit=1
    )[0]

    assert "NVIC_SystemReset();" in done_case
    assert "boot_jump_to_application();" not in done_case


def test_successful_update_verifies_crc_from_flash() -> None:
    source = Path(__file__).resolve().parents[1] / "App" / "app.c"
    text = source.read_text(encoding="utf-8")
    done_function = text.split("bool boot_on_done(void)", maxsplit=1)[1].split(
        "bool is_application_valid(void)", maxsplit=1
    )[0]

    assert "(const uint8_t*)APP_START_ADDR" in done_function
    assert "flash_crc == boot_session.expected_crc32" in done_function
    assert "received_crc == boot_session.expected_crc32" in done_function
    assert "is_application_valid()" in done_function


def test_app_manifest_is_required_before_booting() -> None:
    header = Path(__file__).resolve().parents[1] / "App" / "app.h"
    source = Path(__file__).resolve().parents[1] / "App" / "app.c"
    header_text = header.read_text(encoding="utf-8")
    app_text = source.read_text(encoding="utf-8")
    validity = app_text.split("bool is_application_valid(void)", maxsplit=1)[1].split(
        "void app(void)", maxsplit=1
    )[0]

    assert "#define APP_MANIFEST_ADDR 0x0801F7C0UL" in header_text
    assert "manifest->config_abi == VBDRIVE_CONFIG_TYPE_ID" in validity
    assert "manifest->board_id == APP_MANIFEST_BOARD_ID" in validity
    assert "manifest_ok" in validity


def test_recovery_window_resets_before_starting_application() -> None:
    header = Path(__file__).resolve().parents[1] / "App" / "app.h"
    source = Path(__file__).resolve().parents[1] / "App" / "app.c"
    header_text = header.read_text(encoding="utf-8")
    app_text = source.read_text(encoding="utf-8")

    assert "#define BOOT_RECOVERY_WINDOW_MS 1000UL" in header_text
    assert "boot_update_activity_seen()" in app_text
    assert "boot_launch_app_marker_set();" in app_text
    assert "NVIC_SystemReset();" in app_text
    assert app_text.index("boot_launch_app_marker_set();") < app_text.index(
        "NVIC_SystemReset();"
    )


def test_recovery_start_metadata_is_validated_before_latching() -> None:
    source = Path(__file__).resolve().parents[1] / "App" / "communications.c"
    text = source.read_text(encoding="utf-8")
    start_case = text.split("case BOOT_CMD_START:", maxsplit=1)[1].split(
        "case BOOT_CMD_DATA:", maxsplit=1
    )[0]

    assert "g_start_size == 0U" in start_case
    assert "APP_END_ADDR - APP_START_ADDR" in start_case
    assert start_case.index("g_start_size == 0U") < start_case.index(
        "g_boot_update_activity_seen = true;"
    )


def test_boot_transport_metadata_is_outside_application_range() -> None:
    header = Path(__file__).resolve().parents[1] / "App" / "app.h"
    runtime = Path(__file__).resolve().parents[1] / "App" / "bootloader_runtime.c"
    header_text = header.read_text(encoding="utf-8")
    runtime_text = runtime.read_text(encoding="utf-8")

    assert "#define APP_END_ADDR 0x0801F800UL" in header_text
    assert "#define BOOT_METADATA_ADDR 0x0801F800UL" in header_text
    assert "packed_transport_inverse" in runtime_text
    assert "magic_inverse" in runtime_text


def _assigned_int(text: str, field_name: str) -> int:
    match = re.search(rf"Init\.{field_name}\s*=\s*(\d+)\s*;", text)
    assert match is not None
    return int(match.group(1))
