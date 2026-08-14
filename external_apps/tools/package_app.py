#!/usr/bin/env python3
"""Create a deterministic Metalio .eapp USTAR package."""

from __future__ import annotations

import argparse
import io
import json
from pathlib import Path
import tarfile


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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--elf", type=Path, required=True)
    parser.add_argument("--asset", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    manifest_bytes = args.manifest.read_bytes()
    manifest = json.loads(manifest_bytes)
    if manifest.get("entry") != "elf/esp32p4.elf":
        raise ValueError("manifest entry must be elf/esp32p4.elf")
    if manifest.get("icon") != "assets/demo.png":
        raise ValueError("sample manifest icon must be assets/demo.png")
    if manifest.get("target") != "esp32p4" or manifest.get("api_version") != 1:
        raise ValueError("sample package must target esp32p4 ABI 1")

    elf_bytes = args.elf.read_bytes()
    if len(elf_bytes) < 20 or elf_bytes[:4] != b"\x7fELF":
        raise ValueError("external app entry is not a valid ELF file")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(args.output, "w", format=tarfile.USTAR_FORMAT) as archive:
        add_bytes(archive, "manifest.json", manifest_bytes)
        add_bytes(archive, "elf/esp32p4.elf", elf_bytes)
        add_bytes(archive, "assets/demo.png", args.asset.read_bytes())
    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
