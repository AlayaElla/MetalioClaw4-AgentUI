#!/usr/bin/env python3
"""Create a deterministic Metalio .eapp USTAR package."""

from __future__ import annotations

import argparse
import io
import json
from pathlib import Path
import re
import struct
import tarfile


HOST_IMPORTS_DEF = (
    Path(__file__).resolve().parents[1] / "sdk" / "metalio_app_host_imports.def"
)
IMPORT_PATTERN = re.compile(
    r"^\s*METALIO_APP_IMPORT\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,"
)
ELF32_HEADER_SIZE = 52
ELF32_SECTION_HEADER_SIZE = 40
ELF32_SYMBOL_SIZE = 16
ELF_MACHINE_RISCV = 243
SECTION_TYPE_DYNSYM = 11
SECTION_INDEX_UNDEFINED = 0


def add_bytes(archive: tarfile.TarFile, name: str, data: bytes) -> None:
    info = tarfile.TarInfo(name)
    info.size = len(data)
    info.mode = 0o644
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = 0
    archive.addfile(info, io.BytesIO(data))


def load_host_imports(path: Path = HOST_IMPORTS_DEF) -> frozenset[str]:
    imports: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = IMPORT_PATTERN.match(line)
        if match is not None:
            imports.append(match.group(1))
    if len(imports) != len(set(imports)):
        raise ValueError("external App host import allowlist contains duplicates")
    return frozenset(imports)


def unpack_from(format_string: str, data: bytes, offset: int, label: str) -> tuple:
    size = struct.calcsize(format_string)
    if offset < 0 or offset + size > len(data):
        raise ValueError(f"external App ELF has truncated {label}")
    return struct.unpack_from(format_string, data, offset)


def read_string(table: bytes, offset: int) -> str:
    if offset < 0 or offset >= len(table):
        raise ValueError("external App ELF has an invalid symbol name offset")
    end = table.find(b"\0", offset)
    if end < 0:
        raise ValueError("external App ELF has an unterminated symbol name")
    return table[offset:end].decode("ascii", errors="strict")


def find_undefined_symbols(elf_bytes: bytes) -> frozenset[str]:
    if len(elf_bytes) < ELF32_HEADER_SIZE or elf_bytes[:4] != b"\x7fELF":
        raise ValueError("external App entry is not a valid ELF file")
    if elf_bytes[4] != 1 or elf_bytes[5] != 1:
        raise ValueError("external App entry must be a little-endian ELF32 file")

    machine, = unpack_from("<H", elf_bytes, 18, "ELF machine field")
    if machine != ELF_MACHINE_RISCV:
        raise ValueError("external App entry must target ESP32-P4 RISC-V")

    section_offset, = unpack_from("<I", elf_bytes, 32, "section table offset")
    section_entry_size, section_count = unpack_from(
        "<HH", elf_bytes, 46, "section table dimensions"
    )
    if section_entry_size < ELF32_SECTION_HEADER_SIZE:
        raise ValueError("external App ELF has an invalid section header size")
    if section_count == 0:
        raise ValueError("external App ELF has no section table")
    if section_offset + section_entry_size * section_count > len(elf_bytes):
        raise ValueError("external App ELF has a truncated section table")

    sections: list[tuple[int, ...]] = []
    for index in range(section_count):
        sections.append(
            unpack_from(
                "<IIIIIIIIII",
                elf_bytes,
                section_offset + index * section_entry_size,
                f"section header {index}",
            )
        )

    undefined: set[str] = set()
    for index, section in enumerate(sections):
        section_type = section[1]
        if section_type != SECTION_TYPE_DYNSYM:
            continue
        symbol_offset = section[4]
        symbol_bytes = section[5]
        string_section_index = section[6]
        symbol_entry_size = section[9]
        if string_section_index >= len(sections):
            raise ValueError(f"external App ELF dynsym {index} has an invalid string table")
        if symbol_entry_size < ELF32_SYMBOL_SIZE or symbol_bytes % symbol_entry_size != 0:
            raise ValueError(f"external App ELF dynsym {index} has an invalid entry size")
        if symbol_offset + symbol_bytes > len(elf_bytes):
            raise ValueError(f"external App ELF dynsym {index} is truncated")

        string_section = sections[string_section_index]
        string_offset = string_section[4]
        string_bytes = string_section[5]
        if string_offset + string_bytes > len(elf_bytes):
            raise ValueError(f"external App ELF dynsym {index} string table is truncated")
        strings = elf_bytes[string_offset:string_offset + string_bytes]

        for symbol_index in range(symbol_bytes // symbol_entry_size):
            name_offset, _, _, _, _, section_index = unpack_from(
                "<IIIBBH",
                elf_bytes,
                symbol_offset + symbol_index * symbol_entry_size,
                f"dynamic symbol {symbol_index}",
            )
            if section_index == SECTION_INDEX_UNDEFINED and name_offset != 0:
                undefined.add(read_string(strings, name_offset))
    return frozenset(undefined)


def validate_elf(elf_bytes: bytes) -> None:
    undefined = find_undefined_symbols(elf_bytes)
    unexpected = sorted(undefined - load_host_imports())
    if unexpected:
        raise ValueError(
            "external App ELF imports unsupported symbols: " + ", ".join(unexpected)
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument(
        "--asset",
        type=Path,
        action="append",
        default=[],
        help=(
            "asset to package; a sole asset follows the manifest icon path, "
            "otherwise each value is stored as assets/<filename>. May be "
            "supplied more than once"
        ),
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    manifest_bytes = args.manifest.read_bytes()
    manifest = json.loads(manifest_bytes)
    if manifest.get("entry") != "elf/esp32p4.elf":
        raise ValueError("manifest entry must be elf/esp32p4.elf")
    if manifest.get("target") != "esp32p4" or manifest.get("api_version") != 1:
        raise ValueError("sample package must target esp32p4 ABI 1")

    icon = manifest.get("icon")
    if icon is not None and (
        not isinstance(icon, str)
        or not icon.startswith("assets/")
        or "\\" in icon
        or "//" in icon
        or any(part in ("", ".", "..") for part in icon.split("/"))
    ):
        raise ValueError("manifest icon must be a safe relative assets/ path")

    packaged_assets: dict[str, Path] = {}
    for asset in args.asset:
        if not asset.is_file():
            raise ValueError(f"asset does not exist: {asset}")
        archive_name = (
            icon
            if len(args.asset) == 1 and isinstance(icon, str)
            else f"assets/{asset.name}"
        )
        if archive_name in packaged_assets:
            raise ValueError(f"duplicate packaged asset: {archive_name}")
        packaged_assets[archive_name] = asset

    if icon is not None:
        if icon not in packaged_assets:
            raise ValueError(f"manifest icon is not packaged: {icon}")

    elf_bytes = args.elf.read_bytes()
    validate_elf(elf_bytes)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, "w", format=tarfile.USTAR_FORMAT) as archive:
        add_bytes(archive, "manifest.json", manifest_bytes)
        add_bytes(archive, "elf/esp32p4.elf", elf_bytes)
        for archive_name in sorted(packaged_assets):
            add_bytes(
                archive,
                archive_name,
                packaged_assets[archive_name].read_bytes(),
            )
    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
