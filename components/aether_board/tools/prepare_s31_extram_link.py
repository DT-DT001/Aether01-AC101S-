"""Create an independent IDF 6.1 S31 XIP/BSS linker-script workaround.

Align the existing same-bus dummy target before assigning it. Move the SDK's
runtime-disabled Wi-Fi debug/verbose strings to a non-allocating ELF section,
leaving the runtime rodata boundary and external BSS anchor unchanged.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile

# EOL-normalized official IDF 6.1 components/esp_system/ld/ld.ext_ram.sections.
TEMPLATE_SHA256 = "459c5255f29966e508b24c019ccde6c6b31a72e6df54c3c572a269b7eb731475"
DUMMY = re.compile(r"\.ext_ram\.dummy\s*\(NOLOAD\)\s*:\s*\{(?P<body>.*?)\}\s*>\s*ext_ram_seg", re.DOTALL)
ASSIGN = re.compile(
    r"\.\s*=\s*_ext_ram_on_same_bus\s*\?\s*"
    r"(?P<target>ORIGIN\(ext_ram_seg\)\s*\+\s*"
    r"\(_rodata_reserved_end\s*-\s*_flash_rodata_dummy_start\))\s*:\s*0\s*;")
FOLLOWING_ALIGN = re.compile(
    r"\.\s*=\s*ALIGN\(_ext_ram_on_same_bus\s*\?\s*"
    r"(?P<page>0x[0-9a-fA-F]+|[0-9]+)\s*:\s*0\s*\)\s*;")
RODATA_NOLOAD = re.compile(
    r"\.flash\.rodata_noload\s*\(NOLOAD\)\s*:\s*\{(?P<body>.*?)\}\s*>\s*flash_rodata_seg",
    re.DOTALL)
RODATA_END = re.compile(r"_rodata_reserved_end\s*=\s*\.\s*;")
WLOG_MAPPINGS = re.compile(
    r"\s*\*\(\.rodata_wlog_debug\s+\.rodata_wlog_debug\.\*\)"
    r"\s*\*\(\.rodata_wlog_verbose\s+\.rodata_wlog_verbose\.\*\)\s*")
WLOG_INFO_HEADER = ".wlog.metadata 0 (INFO) :"


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def normalized_template(data: bytes) -> bytes:
    return data.decode("utf-8-sig").replace("\r\n", "\n").replace("\r", "\n").encode("utf-8")


def move_wlog_metadata(source: str) -> str:
    sections = list(RODATA_NOLOAD.finditer(source))
    if len(sections) != 1 or WLOG_INFO_HEADER in source:
        raise ValueError("Expected one original flash rodata NOLOAD section")
    section = sections[0]
    body = section.group("body")
    markers = list(RODATA_END.finditer(body))
    if len(markers) != 1:
        raise ValueError("Expected one SDK runtime rodata boundary")
    marker = markers[0]
    prefix = re.sub(r"/\*.*?\*/", "", body[:marker.start()], flags=re.DOTALL)
    if not re.fullmatch(r"\s*\.\s*=\s*ALIGN\(128\)\s*;\s*", prefix):
        raise ValueError("Unexpected layout before the runtime rodata boundary")
    mappings = body[marker.end():]
    if not mappings.strip():
        return source
    if not WLOG_MAPPINGS.fullmatch(mappings):
        raise ValueError("Unexpected mappings after the runtime rodata boundary")

    # These input strings are outside the runtime rodata reservation by
    # design. Keep their ELF symbols/content for blob relocations, with no
    # SHF_ALLOC or region cursor advance. The runtime PSRAM mapper places BSS
    # at ALIGN(_rodata_reserved_end, page) and trims the heap by BSS size only.
    newline = "\r\n" if "\r\n" in source else "\n"
    mapping_start = section.start("body") + marker.end()
    keep_boundary = source[section.end("body"):section.end()]
    metadata = newline + f"  {WLOG_INFO_HEADER}" + newline + "  {" + mappings + "}"
    return source[:mapping_start] + newline + "  " + keep_boundary + metadata + source[section.end():]


def patch_source(source: str) -> tuple[str, int]:
    blocks = list(DUMMY.finditer(source))
    if len(blocks) != 1:
        raise ValueError("Expected exactly one S31 external-RAM dummy section")
    body = blocks[0].group("body")
    assignments, alignments = list(ASSIGN.finditer(body)), list(FOLLOWING_ALIGN.finditer(body))
    if len(assignments) != 1 or len(alignments) != 1:
        raise ValueError("Expected one original dummy assignment and one following page alignment")
    assignment, alignment = assignments[0], alignments[0]
    if assignment.end() > alignment.start() or body[assignment.end():alignment.start()].strip():
        raise ValueError("The SDK dummy alignment must immediately follow its assignment")
    page_text = alignment.group("page")
    page = int(page_text, 0)
    if page <= 0 or page & (page - 1):
        raise ValueError("MMU page alignment must be a positive power of two")
    start = blocks[0].start("body") + assignment.start("target")
    end = blocks[0].start("body") + assignment.end("target")
    target = assignment.group("target")
    # Without ABSOLUTE here, GNU ld can treat this ALIGN's section-relative
    # operand as padding during its allocation pass and silently advance a
    # cross-page layout. Force the intended address before aligning it.
    aligned = source[:start] + f"ALIGN(ABSOLUTE({target}), {page_text})" + source[end:]
    return move_wlog_metadata(aligned), page


def atomic_write_if_changed(path: Path, data: bytes) -> None:
    if path.exists() and path.read_bytes() == data:
        return
    descriptor, name = tempfile.mkstemp(prefix=".s31-extram-", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "wb") as output:
            output.write(data)
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def prepare(source: Path, template: Path, output: Path, build_dir: Path) -> dict:
    source, template, output, build_dir = (p.resolve() for p in (source, template, output, build_dir))
    if not output.is_relative_to(build_dir) or output in (source, template):
        raise ValueError("Output must be an independent file inside the build directory")
    template_data = template.read_bytes()
    if digest(normalized_template(template_data)) != TEMPLATE_SHA256:
        raise ValueError("Unsupported SDK template SHA256; review the workaround before using another template")
    original = source.read_bytes()
    patched, page = patch_source(original.decode("utf-8"))
    if page != 65536:
        raise ValueError("This workaround is qualified only for the S31 64KiB MMU page")
    patched_data = patched.encode("utf-8")
    if source.read_bytes() != original or template.read_bytes() != template_data:
        raise ValueError("Input changed while creating the independent linker script")
    result = {
        "purpose": "IDF 6.1 S31 XIP/BSS anchor with non-allocating Wi-Fi log metadata",
        "sdk_template": str(template), "sdk_template_normalized_sha256": TEMPLATE_SHA256,
        "original": str(source), "original_sha256": digest(original),
        "generated_sha256": digest(patched_data), "page_bytes": page,
        "assignment_matches": 1, "following_alignment_preserved": True,
        "wlog_metadata_relocated": WLOG_INFO_HEADER in patched,
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    atomic_write_if_changed(output, patched_data)
    atomic_write_if_changed(Path(str(output) + ".provenance.json"),
                            (json.dumps(result, indent=2, sort_keys=True) + "\n").encode("utf-8"))
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "template", "output", "build-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    result = prepare(args.source, args.template, args.output, args.build_dir)
    print(f"Prepared S31 external-BSS linker script: page={result['page_bytes']} SHA256={result['generated_sha256']}")


if __name__ == "__main__":
    main()
