"""Build-only symbol isolation for the official ESP-H264 1.4.1 S31 archive.

The managed component remains untouched. No instruction or allocation order is
patched: objcopy renames three undefined references to project-owned shims.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

EXPECTED_SHA256 = "a8b3d56882971c78ebee585a22e336ee64e35279f9553b71aa186e5ee1bf3643"
COMPONENT_HASH = "3d60078419ceac61cd87579664ae6a5a9b9a42d6f2edf4e23d753af8d2a67da9"
UPSTREAM_COMMIT = "a8e1281e78d429df3429f7349cddef92abaebc8e"
RENAMES = {
    "heap_caps_malloc_prefer": "aether_h264_hp_malloc_prefer",
    "heap_caps_calloc_prefer": "aether_h264_hp_calloc_prefer",
    "malloc": "aether_h264_hp_malloc",
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def symbols(nm: str, archive: Path, undefined: bool) -> set[str]:
    flags = ["-A", "-u"] if undefined else ["-A", "-g", "--defined-only"]
    text = subprocess.run([nm, *flags, str(archive)], check=True,
                          capture_output=True, text=True).stdout
    if undefined:
        return set(re.findall(r"\bU\s+(\S+)\s*$", text, re.MULTILINE))
    return set(re.findall(r"\b[0-9a-fA-F]+\s+[A-Z]\s+(\S+)\s*$", text, re.MULTILINE))


def prepare(source: Path, output: Path, build_dir: Path, objcopy: str, nm: str) -> dict:
    source, output, build_dir = source.resolve(), output.resolve(), build_dir.resolve()
    if not output.is_relative_to(build_dir) or output == source:
        raise ValueError("Output must be a separate generated file inside the build directory")
    if output.name != "libtinyh264.a":
        raise ValueError("Keep libtinyh264.a basename for the upstream IRAM linker mapping")
    if digest(source) != EXPECTED_SHA256:
        raise ValueError("Unsupported archive SHA256; only official ESP-H264 1.4.1 S31 is qualified")
    original_undefined = symbols(nm, source, True)
    original_defined = symbols(nm, source, False)
    if not RENAMES.keys() <= original_undefined or RENAMES.keys() & original_defined:
        raise ValueError("Expected exactly undefined allocator references in the original archive")
    output.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix=".tinyh264-", suffix=".a", dir=output.parent)
    os.close(descriptor)
    temporary = Path(temporary_name)
    try:
        command = [objcopy, "--enable-deterministic-archives"]
        command += ["--redefine-sym=" + old + "=" + new for old, new in RENAMES.items()]
        subprocess.run([*command, str(source), str(temporary)], check=True,
                       capture_output=True, text=True)
        renamed_undefined = symbols(nm, temporary, True)
        if (RENAMES.keys() & renamed_undefined or
                not set(RENAMES.values()) <= renamed_undefined or
                symbols(nm, temporary, False) != original_defined):
            raise ValueError("Generated allocator references or exported symbols did not match")
        expected_undefined = (original_undefined - RENAMES.keys()) | set(RENAMES.values())
        if renamed_undefined != expected_undefined:
            raise ValueError("Unexpected additional undefined-symbol changes")
        if digest(source) != EXPECTED_SHA256:
            raise ValueError("Original managed archive changed during generation")
        result = {
            "component": "espressif/esp_h264", "version": "1.4.1",
            "component_hash": COMPONENT_HASH, "upstream_commit": UPSTREAM_COMMIT,
            "license": "Apache-2.0; original dependency notices remain applicable",
            "source": str(source), "source_sha256": EXPECTED_SHA256,
            "generated_sha256": digest(temporary), "renamed_undefined_symbols": RENAMES,
        }
        if not output.exists() or output.read_bytes() != temporary.read_bytes():
            os.replace(temporary, output)
        provenance = Path(str(output) + ".provenance.json")
        content = json.dumps(result, indent=2, sort_keys=True) + "\n"
        if not provenance.exists() or provenance.read_text(encoding="utf-8") != content:
            provenance.write_text(content, encoding="utf-8")
        return result
    finally:
        temporary.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "output", "build-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--objcopy", required=True)
    parser.add_argument("--nm", required=True)
    args = parser.parse_args()
    result = prepare(args.source, args.output, args.build_dir, args.objcopy, args.nm)
    print("Prepared S31 H264 1.4.1 allocator isolation: " + result["generated_sha256"])


if __name__ == "__main__":
    main()
