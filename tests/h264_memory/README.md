# Scoped S31 H264 allocator

The root CMake enables this policy for the pinned S31 assembly decoder while
retaining the global RTC heap for other tasks. Only H264 allocation references
are isolated; system malloc/heap functions are not globally wrapped.

Run `sh tests/h264_memory/run.sh` in the existing WSL environment. The C test
compiles the actual production shim with ASan/UBSan and replaces only the public
heap/front-end calls. It covers LP rejection, preference order, fallback, one
final OOM notification, the pinned two-cap varargs contract, front-end size
reporting, and unchanged unrelated allocations. Python tests exercise the
actual generation script with an explicitly hash-substituted native fixture;
the production CLI has no hash-override option. They cover source immutability,
symbol isolation, deterministic repeated generation, output boundaries and
failure cleanup. They do not establish S31 instruction correctness.

The link-order regression puts the actual shim in an early component archive
group and its callers in a later imported archive. It reproduces the missing
symbols without an anchor, then verifies that the hook's one `--undefined`
anchor pulls the single object containing all four allocation entry points.
It uses ordinary section garbage collection and ASan/UBSan, without pulling an
entire component through `--whole-archive`.

For an actual archive audit, run the production generator with the existing
IDF Python, objcopy and nm, setting `--source` to the managed
`sw/libs/esp32s31/libtinyh264.a` and `--output` to a build subdirectory containing
the same basename. Only build outputs are written. The script refuses any
original SHA256 other than
`a8b3d56882971c78ebee585a22e336ee64e35279f9553b71aa186e5ee1bf3643`.
It verifies that precisely three undefined allocator names changed, all global
definitions are preserved and the managed input is still unchanged.

The pinned dependency is `espressif/esp_h264 ==1.4.1`, upstream commit
`a8e1281e78d429df3429f7349cddef92abaebc8e`, component hash
`3d60078419ceac61cd87579664ae6a5a9b9a42d6f2edf4e23d753af8d2a67da9`.
Its Apache-2.0 and retained upstream notices still apply. This policy
changes symbol references without changing codec instructions or upgrading
the dependency; generated provenance records both archive hashes.

The root integration after `project(aether01)` is:

```cmake
include(components/aether_mp4_player/cmake/s31_h264_memory.cmake)
aether_enable_s31_h264_hp_alloc()
```

The component guard requires this scoped policy when S31 ASM and RTC heap are
both enabled. Confirm a clean build generates the archive before ldgen, the
original library hash remains identical, and upstream IRAM rules still match
the `libtinyh264.a` basename. Native pixel checks, play/seek/exit/reopen,
HP-stack placement and simultaneous Bluetooth/Wi-Fi scans remain device tests.
