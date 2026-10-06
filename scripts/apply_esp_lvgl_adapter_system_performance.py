#!/usr/bin/env python3
"""Apply the pinned, hash-checked ESP LVGL adapter performance patch."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import sys
import tempfile
from dataclasses import dataclass


ROOT = Path(__file__).resolve().parents[1]
PATCH_PATH = ROOT / "patches/esp-lvgl-adapter-v0.6.3-system-performance.patch"
PATCH_SHA256 = "0a241b77303ebfd195cb05f7a345acdcb567f0c625f2255b263cd09cece627b1"
PLANNER_PATH = ROOT / "main/display/display_cache_sync_plan.h"
PLANNER_SHA256 = "78eaff130948b9c59da8062fbf9acd31ae18664b117c6c6f0ec2d46c49dc703c"
EXPECTED_COMPONENT_HASH = (
    "74ad06b34ee7aec3f2a4f4fd8ed20852527a780d35d0154488624383d9e72972"
)
EXPECTED_VERSION = "0.6.3"


@dataclass(frozen=True)
class Target:
    pristine_sha256: str
    final_sha256: str


TARGETS = {
    "src/display/bridge/v9/lvgl_bridge_v9.c": Target(
        "424e7ef240aee67da6a42f9f065bcdc1114393ab7ea38e4f7e449edfcb33f2ed",
        "af51c163d2705b3b349ccc8e4406669bdc8d70915989ff03620c608cbcea6e25",
    ),
    "src/adapter/adapter_internal.h": Target(
        "b1d376275ff143aa121eaa7dd1cd72cbf50e18e5aba695e300f498fcbcc44327",
        "44f49246941a4a098769d6b26faa30eec12da87c8a00e5b5380aed5ce7cd08d3",
    ),
    "include/esp_lv_adapter.h": Target(
        "ae5520679cc1614267fdf0bc58685500b4db6816bdf9e747a43e32529ffbe22c",
        "680bcfb5a7117b50897cb8a8d9ad54a0e356625c839d1a0fc1883ec679716376",
    ),
    "src/adapter/esp_lv_adapter.c": Target(
        "3aaf0e1b79c8f89afb1224f5f08957aee3e76e1ae2d9d3ae626d717a6b42b2f5",
        "9d8c049e047cac9c09c699c0ec6c21211bddee060b2419d84d354f8afd100025",
    ),
}


class PatchError(RuntimeError):
    pass


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def normalized_artifact_bytes(data: bytes) -> bytes:
    """Normalize checkout line endings for tracked patch/planner integrity."""
    return data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")


def artifact_sha256(path: Path) -> str:
    return sha256(normalized_artifact_bytes(path.read_bytes()))


def parse_patch(text: str) -> dict[str, list[tuple[int, int, list[str]]]]:
    """Parse the small unified-diff subset emitted by Python difflib."""
    lines = text.splitlines(keepends=True)
    parsed: dict[str, list[tuple[int, int, list[str]]]] = {}
    index = 0
    while index < len(lines):
        if not lines[index].startswith("--- a/"):
            raise PatchError(f"unexpected patch line {index + 1}: {lines[index].rstrip()}")
        old_path = lines[index][6:].strip()
        index += 1
        if index >= len(lines) or not lines[index].startswith("+++ b/"):
            raise PatchError(f"missing new path header for {old_path}")
        new_path = lines[index][6:].strip()
        index += 1
        if old_path != new_path or old_path not in TARGETS:
            raise PatchError(f"unexpected patch target: {old_path} -> {new_path}")
        if old_path in parsed:
            raise PatchError(f"duplicate patch target: {old_path}")

        hunks: list[tuple[int, int, list[str]]] = []
        while index < len(lines) and lines[index].startswith("@@ "):
            match = re.match(r"@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", lines[index])
            if match is None:
                raise PatchError(f"malformed hunk header at line {index + 1}")
            old_start = int(match.group(1))
            old_count = int(match.group(2) or "1")
            index += 1
            body: list[str] = []
            consumed_old = 0
            while index < len(lines) and not lines[index].startswith(("@@ ", "--- a/")):
                line = lines[index]
                if line.startswith((" ", "-")):
                    consumed_old += 1
                elif not line.startswith("+"):
                    raise PatchError(f"unsupported hunk line {index + 1}")
                body.append(line)
                index += 1
            if consumed_old != old_count:
                raise PatchError(
                    f"hunk old-line count mismatch for {old_path}: "
                    f"expected {old_count}, found {consumed_old}"
                )
            hunks.append((old_start, old_count, body))
        if not hunks:
            raise PatchError(f"patch target has no hunks: {old_path}")
        parsed[old_path] = hunks

    if set(parsed) != set(TARGETS):
        missing = sorted(set(TARGETS) - set(parsed))
        extra = sorted(set(parsed) - set(TARGETS))
        raise PatchError(f"patch target set mismatch; missing={missing}, extra={extra}")
    return parsed


def apply_unified_patch(original: bytes, hunks: list[tuple[int, int, list[str]]], path: str) -> bytes:
    source = original.decode("utf-8").splitlines(keepends=True)
    output: list[str] = []
    cursor = 0
    for old_start, old_count, body in hunks:
        hunk_start = max(0, old_start if old_count == 0 else old_start - 1)
        if hunk_start < cursor or hunk_start > len(source):
            raise PatchError(f"invalid hunk offset for {path}")
        output.extend(source[cursor:hunk_start])
        cursor = hunk_start
        for line_number, line in enumerate(body, 1):
            marker, content = line[0], line[1:]
            if marker == " ":
                if cursor >= len(source) or source[cursor] != content:
                    raise PatchError(f"context mismatch while patching {path}")
                output.append(content)
                cursor += 1
            elif marker == "-":
                if cursor >= len(source) or source[cursor] != content:
                    raise PatchError(f"removed-line mismatch while patching {path}")
                cursor += 1
            elif marker == "+":
                output.append(content)
            else:
                raise PatchError(f"unsupported patch marker in {path} at hunk line {line_number}")
    output.extend(source[cursor:])
    return "".join(output).encode("utf-8")


def validate_component(component_dir: Path) -> None:
    metadata = component_dir / "idf_component.yml"
    lock = component_dir / ".component_hash"
    if not metadata.is_file() or not lock.is_file():
        raise PatchError("adapter component metadata or lock hash is missing")
    version_text = metadata.read_text(encoding="utf-8")
    version_match = re.search(r"(?m)^version:\s*['\"]?([^'\"\s]+)", version_text)
    if version_match is None or version_match.group(1) != EXPECTED_VERSION:
        actual = version_match.group(1) if version_match else "missing"
        raise PatchError(f"expected esp_lvgl_adapter {EXPECTED_VERSION}, found {actual}")
    actual_hash = lock.read_text(encoding="utf-8").strip()
    if actual_hash != EXPECTED_COMPONENT_HASH:
        raise PatchError(
            f"component lock hash mismatch: expected {EXPECTED_COMPONENT_HASH}, found {actual_hash}"
        )
    if not PLANNER_PATH.is_file() or artifact_sha256(PLANNER_PATH) != PLANNER_SHA256:
        raise PatchError("tracked cache-range planner is missing or has an unexpected hash")
    if not PATCH_PATH.is_file() or artifact_sha256(PATCH_PATH) != PATCH_SHA256:
        raise PatchError("versioned adapter patch is missing or has an unexpected hash")


def write_stage(path: Path, data: bytes) -> Path:
    descriptor, temp_name = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    temp_path = Path(temp_name)
    with os.fdopen(descriptor, "wb") as staged:
        staged.write(data)
        staged.flush()
        os.fsync(staged.fileno())
    return temp_path


def apply_component(component_dir: Path) -> str:
    validate_component(component_dir)
    patch_text = normalized_artifact_bytes(PATCH_PATH.read_bytes()).decode("utf-8")
    parsed_patch = parse_patch(patch_text)

    original: dict[str, bytes] = {}
    desired: dict[str, bytes] = {}
    for relative, hashes in TARGETS.items():
        target_path = component_dir / relative
        if not target_path.is_file():
            raise PatchError(f"required adapter source is missing: {relative}")
        contents = target_path.read_bytes()
        actual_hash = sha256(contents)
        if actual_hash == hashes.final_sha256:
            original[relative] = contents
            desired[relative] = contents
        elif actual_hash == hashes.pristine_sha256:
            updated = apply_unified_patch(contents, parsed_patch[relative], relative)
            final_hash = sha256(updated)
            if final_hash != hashes.final_sha256:
                raise PatchError(
                    f"patch result hash mismatch for {relative}: expected "
                    f"{hashes.final_sha256}, found {final_hash}"
                )
            original[relative] = contents
            desired[relative] = updated
        else:
            raise PatchError(
                f"unknown source hash for {relative}: {actual_hash}; "
                "refusing to overwrite local changes"
            )

    changed = [relative for relative in TARGETS if desired[relative] != original[relative]]
    if not changed:
        return "already applied"

    staged_new: dict[str, Path] = {}
    staged_old: dict[str, Path] = {}
    try:
        for relative in changed:
            target_path = component_dir / relative
            staged_new[relative] = write_stage(target_path, desired[relative])
            staged_old[relative] = write_stage(target_path, original[relative])

        # Recheck all targets after planning/staging, before the first write.
        for relative in TARGETS:
            actual_hash = sha256((component_dir / relative).read_bytes())
            expected_hash = sha256(original[relative])
            if actual_hash != expected_hash:
                raise PatchError(f"source changed during patch planning: {relative}")

        replaced: list[str] = []
        try:
            for relative in changed:
                os.replace(staged_new[relative], component_dir / relative)
                replaced.append(relative)
        except OSError:
            for relative in reversed(replaced):
                os.replace(staged_old[relative], component_dir / relative)
            raise
    finally:
        for path in (*staged_new.values(), *staged_old.values()):
            try:
                path.unlink(missing_ok=True)
            except OSError:
                pass
    return f"patched {len(changed)} files"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component-dir", required=True, type=Path)
    args = parser.parse_args()
    try:
        result = apply_component(args.component_dir.resolve())
    except (OSError, UnicodeError, PatchError) as error:
        print(f"adapter patch failed: {error}", file=sys.stderr)
        return 1
    print(
        f"esp_lvgl_adapter {EXPECTED_VERSION} system-performance patch: {result}; "
        f"component={args.component_dir.resolve()}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
