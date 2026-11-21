"""Exercise the real post-project hook with a small native dependency graph."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/host_s31_extram_link"
OUT.mkdir(parents=True, exist_ok=True)
CMAKE = os.environ.get("AETHER_TEST_CMAKE") or shutil.which("cmake")
NINJA = os.environ.get("AETHER_TEST_NINJA") or shutil.which("ninja")


@unittest.skipUnless(CMAKE and NINJA, "Run this part with the existing native CMake/Ninja")
class HookTests(unittest.TestCase):
    def configure(self, options="SHELL:-T sections.ld", enabled=True):
        temporary = tempfile.TemporaryDirectory(dir=OUT)
        self.addCleanup(temporary.cleanup)
        base = Path(temporary.name)
        (base / "main.c").write_text("int main(void) { return 0; }\n")
        (base / "system.c").write_text("int system_stub;\n")
        (base / "seed.ld").write_text("/* dependency fixture; configure only */\n")
        hook = ROOT / "components/aether_board/cmake/s31_extram_link.cmake"
        (base / "CMakeLists.txt").write_text(f'''cmake_minimum_required(VERSION 3.16)
project(s31_hook_fixture NONE)
set(CONFIG_IDF_TARGET_ESP32S31 {"ON" if enabled else "OFF"})
set(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY ON)
set(CONFIG_SPIRAM_XIP_FROM_PSRAM ON)
add_custom_target(__idf_esp_system)
set_property(TARGET __idf_esp_system PROPERTY INTERFACE_LINK_OPTIONS "{options}")
add_custom_target(fixture)
function(idf_component_get_property out component property)
  set(${{out}} __idf_esp_system PARENT_SCOPE)
endfunction()
function(idf_build_get_property out property)
  if(property STREQUAL "EXECUTABLE")
    set(value fixture)
  elseif(property STREQUAL "BUILD_DIR")
    set(value "${{CMAKE_BINARY_DIR}}")
  elseif(property STREQUAL "IDF_PATH")
    set(value "${{CMAKE_SOURCE_DIR}}/sdk")
  elseif(property STREQUAL "PYTHON")
    set(value python3)
  else()
    message(FATAL_ERROR "Unexpected fake IDF property ${{property}}")
  endif()
  set(${{out}} "${{value}}" PARENT_SCOPE)
endfunction()
# Reproduce ldgen depending on the component before producing sections.ld.
add_custom_command(OUTPUT "${{CMAKE_BINARY_DIR}}/esp-idf/esp_system/ld/sections.ld"
  COMMAND ${{CMAKE_COMMAND}} -E make_directory "${{CMAKE_BINARY_DIR}}/esp-idf/esp_system/ld"
  COMMAND ${{CMAKE_COMMAND}} -E copy "${{CMAKE_SOURCE_DIR}}/seed.ld"
    "${{CMAKE_BINARY_DIR}}/esp-idf/esp_system/ld/sections.ld"
  DEPENDS __idf_esp_system "${{CMAKE_SOURCE_DIR}}/seed.ld")
include("{hook.as_posix()}")
aether_fix_s31_extram_link()
aether_fix_s31_extram_link()
get_target_property(options __idf_esp_system INTERFACE_LINK_OPTIONS)
get_target_property(system_deps __idf_esp_system MANUALLY_ADDED_DEPENDENCIES)
get_target_property(elf_deps fixture MANUALLY_ADDED_DEPENDENCIES)
get_target_property(link_deps fixture LINK_DEPENDS)
file(WRITE "${{CMAKE_BINARY_DIR}}/properties.txt"
  "options=${{options}}\\nsystem_deps=${{system_deps}}\\nelf_deps=${{elf_deps}}\\nlink_deps=${{link_deps}}\\n")
''')
        build = base / "out"
        result = subprocess.run([CMAKE, "-G", "Ninja", f"-DCMAKE_MAKE_PROGRAM={NINJA}",
                                 "-S", str(base), "-B", str(build)],
                                text=True, capture_output=True)
        return build, result

    def test_single_replacement_and_elf_dependency_generate_without_cycle(self):
        build, result = self.configure("SHELL:-T sections.ld;LINKER:--gc-sections")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        properties = (build / "properties.txt").read_text()
        self.assertIn(f'options=SHELL:-T "{build.as_posix()}/aether_s31_extram/sections.ld";LINKER:--gc-sections', properties)
        self.assertNotIn("options=SHELL:-T sections.ld", properties)
        self.assertIn("system_deps=system_deps-NOTFOUND", properties)
        self.assertIn("elf_deps=aether_s31_extram_link_script", properties)
        self.assertIn(f"link_deps={build.as_posix()}/aether_s31_extram/sections.ld", properties)
        graph = (build / "build.ninja").read_text()
        self.assertIn("esp-idf/esp_system/ld/sections.ld", graph)
        self.assertIn("aether_s31_extram/sections.ld", graph)

    def test_unknown_and_duplicate_interface_options_fail_closed(self):
        for options in ("LINKER:--gc-sections", "SHELL:-T sections.ld;SHELL:-T sections.ld"):
            _, result = self.configure(options)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Expected exactly one", result.stdout + result.stderr)

    def test_other_targets_leave_options_and_dependencies_unchanged(self):
        build, result = self.configure(enabled=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        properties = (build / "properties.txt").read_text()
        self.assertIn("options=SHELL:-T sections.ld\n", properties)
        self.assertIn("elf_deps=elf_deps-NOTFOUND", properties)


if __name__ == "__main__":
    unittest.main(verbosity=2)
