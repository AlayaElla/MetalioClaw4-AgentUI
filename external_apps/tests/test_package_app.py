from __future__ import annotations

import importlib.util
from pathlib import Path
import struct
import unittest


ROOT = Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "external_apps" / "tools" / "package_app.py"
SPEC = importlib.util.spec_from_file_location("package_app", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
package_app = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package_app)


def make_elf(undefined_symbols: list[str]) -> bytes:
    strings = bytearray(b"\0")
    name_offsets: list[int] = []
    for name in undefined_symbols:
        name_offsets.append(len(strings))
        strings.extend(name.encode("ascii") + b"\0")

    symbols = bytearray(16)
    for name_offset in name_offsets:
        symbols.extend(struct.pack("<IIIBBH", name_offset, 0, 0, 0x10, 0, 0))

    data = bytearray(52)
    string_offset = len(data)
    data.extend(strings)
    while len(data) % 4:
        data.append(0)
    symbol_offset = len(data)
    data.extend(symbols)
    while len(data) % 4:
        data.append(0)
    section_offset = len(data)

    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (0, 3, 0, 0, string_offset, len(strings), 0, 0, 1, 0),
        (0, 11, 0, 0, symbol_offset, len(symbols), 1, 1, 4, 16),
    ]
    for section in sections:
        data.extend(struct.pack("<IIIIIIIIII", *section))

    data[:16] = b"\x7fELF\x01\x01\x01" + bytes(9)
    struct.pack_into(
        "<HHIIIIIHHHHHH",
        data,
        16,
        3,
        243,
        1,
        0,
        0,
        section_offset,
        0,
        52,
        0,
        0,
        40,
        len(sections),
        0,
    )
    return bytes(data)


class PackageAppElfValidationTests(unittest.TestCase):
    def test_ai_actions_are_validated_before_packaging(self) -> None:
        package_app.validate_manifest({
            "id": "com.metalio.demo",
            "entry": "elf/esp32p4.elf", "target": "esp32p4", "api_version": 1,
            "ai_actions": [{
                "id": "com.metalio.demo.next", "title": "Next", "description": "Advance",
                "args_schema": {"type": "object", "additionalProperties": False},
            }],
        })

    def test_ai_action_rejects_unsafe_ids_and_non_object_schema(self) -> None:
        manifest = {
            "entry": "elf/esp32p4.elf", "target": "esp32p4", "api_version": 1,
            "ai_actions": [{
                "id": "unsafe action", "title": "Bad", "description": "Bad",
                "args_schema": {"type": "array"},
            }],
        }
        with self.assertRaisesRegex(ValueError, "ai_action"):
            package_app.validate_manifest(manifest)

    def test_allowlist_is_loaded_from_shared_definition(self) -> None:
        imports = package_app.load_host_imports()
        self.assertIn("log", imports)
        self.assertIn("snprintf", imports)
        self.assertIn("__adddf3", imports)

    def test_action_cannot_claim_system_or_another_app_namespace(self) -> None:
        manifest = {
            "id": "com.metalio.demo", "entry": "elf/esp32p4.elf",
            "target": "esp32p4", "api_version": 1,
            "ai_actions": [{"id": "system.phone", "title": "Phone",
                            "description": "Call", "args_schema": {"type": "object"}}],
        }
        with self.assertRaisesRegex(ValueError, "namespace"):
            package_app.validate_manifest(manifest)

    def test_allowed_undefined_symbols_pass(self) -> None:
        package_app.validate_elf(make_elf(["log", "snprintf", "__adddf3"]))

    def test_unknown_undefined_symbol_fails(self) -> None:
        with self.assertRaisesRegex(ValueError, "unsupported symbols: system"):
            package_app.validate_elf(make_elf(["log", "system"]))

    def test_non_riscv_elf_fails(self) -> None:
        elf = bytearray(make_elf([]))
        struct.pack_into("<H", elf, 18, 62)
        with self.assertRaisesRegex(ValueError, "ESP32-P4 RISC-V"):
            package_app.validate_elf(bytes(elf))


if __name__ == "__main__":
    unittest.main()
