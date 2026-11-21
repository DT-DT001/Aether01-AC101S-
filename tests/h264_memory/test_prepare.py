"""Build-script contract tests using a native fixture, without modifying S31 inputs."""
from pathlib import Path
import hashlib
import importlib.util
import shutil
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("s31_h264_prepare",
    ROOT / "components/aether_mp4_player/tools/prepare_s31_h264.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class PrepareTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=ROOT / "build/host_h264_memory")
        self.base = Path(self.directory.name)
        self.build = self.base / "build"
        self.build.mkdir()
        self.source = self.base / "official.a"
        self.output = self.build / "generated/libtinyh264.a"
        self.objcopy = shutil.which("objcopy")
        self.nm = shutil.which("nm")
        if not self.objcopy or not self.nm:
            self.skipTest("Existing native objcopy/nm are needed for fixture generation")
        code = self.base / "fixture.c"
        code.write_text("""#include <stddef.h>
extern void *heap_caps_malloc_prefer(size_t,size_t,...);
extern void *heap_caps_calloc_prefer(size_t,size_t,size_t,...);
extern void *malloc(size_t);
void *fixture_alloc(int kind) {
 if(kind==0) return heap_caps_malloc_prefer(5120,2,2052U,1028U);
 if(kind==1) return heap_caps_calloc_prefer(1,1280,2,2052U,1028U);
 return malloc(1280);
}
""", encoding="utf-8")
        subprocess.run(["cc", "-fno-builtin", "-c", str(code), "-o", str(self.base/"fixture.o")], check=True)
        subprocess.run(["ar", "rcs", str(self.source), str(self.base/"fixture.o")], check=True)
        self.before = self.source.read_bytes()
        self.hash = hashlib.sha256(self.before).hexdigest()

    def tearDown(self):
        self.directory.cleanup()

    def invoke(self):
        return module.prepare(self.source,self.output,self.build,self.objcopy,self.nm)

    def test_original_hash_is_pinned_and_failure_publishes_nothing(self):
        with self.assertRaisesRegex(ValueError,"SHA256"):
            self.invoke()
        self.assertFalse(self.output.exists())
        self.assertEqual(self.source.read_bytes(),self.before)

    def test_only_expected_undefined_references_change_and_run_is_idempotent(self):
        # This in-process fixture substitution is not exposed by the production CLI.
        with patch.object(module,"EXPECTED_SHA256",self.hash):
            result=self.invoke()
            stamp=self.output.stat().st_mtime_ns
            self.assertEqual(self.source.read_bytes(),self.before)
            self.assertEqual(module.symbols(self.nm,self.output,True),set(module.RENAMES.values()))
            self.assertEqual(module.symbols(self.nm,self.source,False),module.symbols(self.nm,self.output,False))
            self.assertEqual(result["source_sha256"],self.hash)
            self.assertEqual(self.invoke(),result)
            self.assertEqual(self.output.stat().st_mtime_ns,stamp)
            self.assertTrue(Path(str(self.output)+".provenance.json").exists())

    def test_output_cannot_overwrite_source_or_escape_build(self):
        for output in (self.source,self.base/"outside/libtinyh264.a",self.build/"wrong.a"):
            with self.assertRaises(ValueError):
                module.prepare(self.source,output,self.build,self.objcopy,self.nm)
        self.assertEqual(self.source.read_bytes(),self.before)

    def test_tool_failure_preserves_existing_output_and_cleans_temporary(self):
        with patch.object(module,"EXPECTED_SHA256",self.hash):
            self.invoke()
            before=self.output.read_bytes()
            with self.assertRaises(OSError):
                module.prepare(self.source,self.output,self.build,str(self.base/"missing-objcopy"),self.nm)
            self.assertEqual(self.output.read_bytes(),before)
            self.assertFalse(list(self.output.parent.glob(".tinyh264-*")))


if __name__=="__main__":
    unittest.main(verbosity=2)
