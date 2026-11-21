# S31 XIP / external-BSS linker regression

Run `sh tests/s31_extram_link/run.sh` in the existing WSL environment. Tests use
the project patch function and the installed GNU binutils. The SDK and device
are read-only during these tests.

The observed layout is reproduced exactly: region origin `0x50000020`, reserved
rodata end `0x506d9d00`, location after rodata_noload `0x506de3ac`, page64KiB.
GNU ld rejects the original intermediate assignment, although both addresses
round to `0x506e0000`. The patched script links at that intended final address.
Previously legal layouts and different-bus false branches retain their results.
The fixture also reproduces the layout with `_rodata_reserved_end` at
`0x506ebd80` and unused Wi-Fi DEBUG/VERBOSE strings extending the NOLOAD section
to `0x506f032c`. The patched ELF places external BSS at `0x506f0000` and retains
those strings and their relocation targets in non-allocating PROGBITS outside
the load segments. The reserved end and the loaded fixture VMA stay fixed.
Unrecognized mappings and unrelated growth beyond the rounded target fail.

The guard tests cover16/32/64KiB page capture in the math helper and production
rejection of pages other than the qualified64KiB size, unchanged source bytes, preserved
false branch and following alignment, normalized-template hash enforcement,
unknown/duplicate input rejection, build-only output and idempotence. They use
an explicitly substituted fixture hash inside the Python process; the
production CLI has no hash-override option. The production script was also
smoke-tested against the current real SDK template and ldgen output. Native
host ld establishes GNU script semantics, not S31 runtime PSRAM correctness.

The CMake fixture checks the actual hook's unique linker-option replacement,
idempotence, unchanged disabled configuration, missing/duplicate-option guards,
and final-ELF-only dependency with a simulated ldgen edge. It uses the existing
native CMake/Ninja; this portion skips when those programs are absent in WSL.
It was also run with the installed Windows tools using `AETHER_TEST_CMAKE` and
`AETHER_TEST_NINJA` set to their executable paths.

After `project(aether01)`, the root integration is:

```cmake
include(components/aether_board/cmake/s31_extram_link.cmake)
aether_fix_s31_extram_link()
```

The hook activates only for S31 + external BSS + PSRAM XIP. It replaces exactly
one esp_system INTERFACE linker option, creates an independent build script and
provenance, and makes only the final ELF depend on that copy. Depending from
esp_system itself would cycle through ldgen and is intentionally avoided.

The qualified IDF6.1 `components/esp_system/ld/ld.ext_ram.sections` template has
EOL-normalized SHA256
`459c5255f29966e508b24c019ccde6c6b31a72e6df54c3c572a269b7eb731475`.
The project writes an independent build copy. It uses ABSOLUTE before
two-argument ALIGN in the true branch to detect unexpected cross-page growth.
In the qualified `.flash.rodata_noload` shape, it moves the two exact WLOG
input mappings into a separate `(INFO)` output section. The upstream
Apache-2.0 template and generated SDK script remain byte-for-byte unchanged.
