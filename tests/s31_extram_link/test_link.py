"""Real GNU ld regressions for the isolated S31 XIP/BSS assignment fix."""
from pathlib import Path
import importlib.util
import re
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("prepare_s31_extram",
    ROOT / "components/aether_board/tools/prepare_s31_extram_link.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
OUT = ROOT / "build/host_s31_extram_link"
OUT.mkdir(parents=True, exist_ok=True)


def script(current=0x506DE3AC, target=0x506D9D00, page=0x10000, same_bus=True, wlog=False):
    flash_origin = 0x50000020 if same_bus else 0x40000020
    flash_target = flash_origin + (target - 0x50000020)
    flash_region = "ext_ram_seg" if same_bus else "flash_only"
    mappings = ("   *(.rodata_wlog_debug .rodata_wlog_debug.*)\n"
                "   *(.rodata_wlog_verbose .rodata_wlog_verbose.*)" if wlog else "")
    unknown_growth = ("" if wlog else
                      f" .unknown_noload (NOLOAD) : {{ . = ABSOLUTE({current:#x}); }} > ext_ram_seg")
    return f"""MEMORY {{
 ext_ram_seg (rw) : ORIGIN = 0x50000020, LENGTH = 0x1000000
 flash_only (rw) : ORIGIN = 0x40000020, LENGTH = 0x1000000
}}
REGION_ALIAS("flash_rodata_seg", {flash_region});
SECTIONS {{
 .live_probe : {{
   _flash_rodata_dummy_start = .;
   KEEP(*(.fixture_live_ref))
 }} > flash_rodata_seg
 .rodata (NOLOAD) : {{
   . = ABSOLUTE({flash_target:#x});
 }} > flash_rodata_seg
 .flash.rodata_noload (NOLOAD) : {{
   /* SDK runtime boundary and excluded metadata. */
   . = ALIGN(128);
   _rodata_reserved_end = .;
{mappings}
 }} > flash_rodata_seg
{unknown_growth}
 .ext_ram.dummy (NOLOAD) :
 {{
   HIDDEN(_ext_ram_on_same_bus = ORIGIN(ext_ram_seg) == ORIGIN(flash_rodata_seg));
   . = _ext_ram_on_same_bus ? ORIGIN(ext_ram_seg) + (_rodata_reserved_end - _flash_rodata_dummy_start) : 0;
   . = ALIGN(_ext_ram_on_same_bus ? {page:#x} : 0);
 }} > ext_ram_seg
 .ext_ram.bss (NOLOAD) : {{ final_dummy = ABSOLUTE(.); . += 4; }} > ext_ram_seg
 /DISCARD/ : {{ *(.text*) *(.data*) *(.bss*) *(.note*) *(.comment*) }}
}}
"""


class LinkTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=OUT)
        self.base = Path(self.temp.name)
        self.source = self.base / "original.ld"
        self.template = self.base / "sdk-template"
        self.template.write_bytes(b"qualified fixture\n")
        self.fixture_hash = module.digest(module.normalized_template(self.template.read_bytes()))
        self.build = self.base / "build"
        self.build.mkdir()
        self.output = self.build / "patched/sections.ld"
        self.object = self.base / "empty.o"
        assembly = self.base / "empty.s"
        assembly.write_text('.section .note.GNU-stack,"",@progbits\n', encoding="utf-8")
        subprocess.run(["as", str(assembly), "-o", str(self.object)], check=True)

    def tearDown(self):
        self.temp.cleanup()

    def link(self, content, name, extra_objects=()):
        path = self.base / (name + ".ld")
        path.write_text(content, encoding="utf-8")
        elf = self.base / (name + ".elf")
        result = subprocess.run(["ld", "-T", str(path), str(self.object), *map(str, extra_objects), "-o", str(elf)],
                                text=True, capture_output=True)
        address = None
        if not result.returncode:
            symbols = subprocess.run(["nm", "-n", str(elf)], text=True, capture_output=True, check=True).stdout
            address = int(re.search(r"^([0-9a-f]+)\s+\w\s+final_dummy$", symbols, re.M)[1], 16)
        return result, address

    def test_exact_observed_same_page_backward_assignment_is_repaired(self):
        original = script()
        fixed, page = module.patch_source(original)
        before, _ = self.link(original, "observed-original")
        after, address = self.link(fixed, "observed-patched")
        self.assertNotEqual(before.returncode, 0)
        self.assertIn("backwards", before.stderr)
        self.assertEqual(after.returncode, 0, after.stderr)
        self.assertEqual(page, 65536)
        self.assertEqual(address, 0x506E0000)

    def test_previously_legal_layout_has_identical_final_address(self):
        original = script(current=0x506D9D00)
        fixed, _ = module.patch_source(original)
        before, old_address = self.link(original, "legal-original")
        after, new_address = self.link(fixed, "legal-patched")
        self.assertEqual(before.returncode, 0, before.stderr)
        self.assertEqual(after.returncode, 0, after.stderr)
        self.assertEqual(old_address, new_address)

    def test_cross_page_backtracking_still_fails_instead_of_using_max(self):
        original = script(current=0x506E0004)
        fixed, _ = module.patch_source(original)
        for name, content in (("cross-original", original), ("cross-patched", fixed)):
            result, _ = self.link(content, name)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("backwards", result.stderr)

    def test_cross_page_wlog_only_preserves_anchor_and_resolves_live_relocation(self):
        target = 0x506EBD80
        current = 0x506F032C
        original = script(target=target, wlog=True)
        source = self.base / "wlog.s"
        obj = self.base / "wlog.o"
        source.write_text(f'''\
.section .rodata_wlog_debug.fixture,"a",@progbits
.byte 0x12, 0x34, 0x56, 0x78
.globl wlog_marker
wlog_marker:
.space 0x100
.section .rodata_wlog_verbose,"a",@progbits
.space {current - target - 0x104:#x}
.section .fixture_live_ref,"a",@progbits
.quad wlog_marker
.section .note.GNU-stack,"",@progbits
''', encoding="utf-8")
        subprocess.run(["as", str(source), "-o", str(obj)], check=True)
        fixed, _ = module.patch_source(original)
        before, _ = self.link(original, "cross-wlog-original", (obj,))
        after, address = self.link(fixed, "cross-wlog-patched", (obj,))
        self.assertNotEqual(before.returncode, 0)
        self.assertIn("backwards", before.stderr)
        self.assertEqual(after.returncode, 0, after.stderr)
        self.assertEqual(address, 0x506F0000)
        elf = self.base / "cross-wlog-patched.elf"
        symbols = subprocess.check_output(["nm", "-n", str(elf)], text=True)
        for symbol, expected in (("_rodata_reserved_end", target), ("wlog_marker", 4)):
            found = re.search(rf"^([0-9a-f]+)\s+\S\s+{symbol}$", symbols, re.M)
            self.assertIsNotNone(found, symbols)
            self.assertEqual(int(found[1], 16), expected)
        sections = subprocess.check_output(["readelf", "-SW", str(elf)], text=True)
        metadata = re.search(r"\.wlog\.metadata\s+PROGBITS\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+00\s+([A-Z]*)\s+\d+\s+\d+\s+\d+", sections)
        self.assertIsNotNone(metadata, sections)
        self.assertEqual(int(metadata[1], 16), 0)
        self.assertEqual(int(metadata[2], 16), current - target)
        self.assertNotIn("A", metadata[3])
        self.assertRegex(sections, r"\.live_probe\s+PROGBITS\s+0*50000020\b")
        segments = subprocess.check_output(["readelf", "-lW", str(elf)], text=True)
        self.assertNotIn(".wlog.metadata", segments)
        contents = subprocess.check_output(["objdump", "-s", "-j", ".live_probe", str(elf)], text=True)
        self.assertRegex(contents, r"50000020\s+04000000 00000000")

    def test_unexpected_flash_noload_contents_fail_closed(self):
        source = script(wlog=True)
        for changed in (source.replace("_rodata_reserved_end = .;", "_rodata_reserved_end = .; . += 4;"),
                        source.replace(".rodata_wlog_debug.*", ".unknown.*"),
                        source.replace("ALIGN(128);", "ALIGN(64);"),
                        source.replace(".flash.rodata_noload", ".unexpected_noload")):
            with self.assertRaises(ValueError):
                module.patch_source(changed)

    def test_different_buses_leave_false_branch_and_result_unchanged(self):
        original = script(same_bus=False)
        fixed, _ = module.patch_source(original)
        before, old_address = self.link(original, "otherbus-original")
        after, new_address = self.link(fixed, "otherbus-patched")
        self.assertEqual(before.returncode, 0, before.stderr)
        self.assertEqual(after.returncode, 0, after.stderr)
        self.assertEqual(old_address, new_address)

    def test_page_capture_and_alignment_math(self):
        for page in (0x4000, 0x8000, 0x10000):
            fixed, captured = module.patch_source(script(page=page))
            self.assertEqual(captured, page)
            self.assertIn(f"), {page:#x}) : 0;", fixed)
            for target in range(0x10000, 0x40000, 0x731):
                aligned = (target + page - 1) & -page
                self.assertEqual((aligned + page - 1) & -page, aligned)
                for current in (target - 1, target, target + 1, aligned, aligned + 1):
                    if current <= target:
                        self.assertGreaterEqual(aligned, current)
                    if current > aligned:
                        self.assertLess(aligned, current)

    def test_unrecognized_shape_page_and_match_counts_fail_closed(self):
        original = script()
        variants = [original.replace(".ext_ram.dummy", ".changed"), original + original,
                    original.replace("0x10000 : 0", "0x3000 : 0"),
                    original.replace("0x10000 : 0", "0 : 0"),
                    original.replace(". = ALIGN", "unused = ALIGN"),
                    original.replace(". = ALIGN", ". += 4;\n . = ALIGN")]
        already_patched, _ = module.patch_source(original)
        variants.append(already_patched)
        for value in variants:
            with self.assertRaises(ValueError):
                module.patch_source(value)

    def test_guarded_generation_preserves_sources_false_branch_and_next_align(self):
        original = script(wlog=True).replace("\n", "\r\n").encode()
        self.source.write_bytes(original)
        self.template.write_bytes(b"\xef\xbb\xbfqualified fixture\r\n")
        with patch.object(module, "TEMPLATE_SHA256", self.fixture_hash):
            first = module.prepare(self.source, self.template, self.output, self.build)
            stamp = self.output.stat().st_mtime_ns
            second = module.prepare(self.source, self.template, self.output, self.build)
        self.assertEqual(first, second)
        self.assertEqual(self.output.stat().st_mtime_ns, stamp)
        self.assertEqual(self.source.read_bytes(), original)
        self.assertEqual(self.template.read_bytes(), b"\xef\xbb\xbfqualified fixture\r\n")
        self.assertIn(b". = ALIGN(_ext_ram_on_same_bus ? 0x10000 : 0);\r\n", self.output.read_bytes())
        self.assertIn(b"), 0x10000) : 0;", self.output.read_bytes())
        self.assertIn(module.WLOG_INFO_HEADER.encode() + b"\r\n", self.output.read_bytes())
        self.assertTrue(first["wlog_metadata_relocated"])
        self.assertTrue(Path(str(self.output) + ".provenance.json").exists())

    def test_wrong_template_hash_and_output_paths_cannot_publish(self):
        self.source.write_text(script(), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "SHA256"):
            module.prepare(self.source, self.template, self.output, self.build)
        self.assertFalse(self.output.exists())
        for output in (self.source, self.template, self.base / "outside.ld"):
            with self.assertRaises(ValueError):
                module.prepare(self.source, self.template, output, self.build)

    def test_production_generation_rejects_unqualified_page_without_output(self):
        self.source.write_text(script(page=0x8000), encoding="utf-8")
        with patch.object(module, "TEMPLATE_SHA256", self.fixture_hash):
            with self.assertRaisesRegex(ValueError, "64KiB"):
                module.prepare(self.source, self.template, self.output, self.build)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    if not shutil.which("ld") or not shutil.which("as"):
        raise SystemExit("Use the existing native WSL binutils; no installation is performed")
    unittest.main(verbosity=2)
