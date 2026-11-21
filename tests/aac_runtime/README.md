# AAC-LC adapter regression

Run with the existing host C compiler and Python 3:

```sh
sh tests/aac_runtime/run.sh
```

This compiles production `components/aether_mp4_player/mp4_aac.c` with ASan/UBSan and a deterministic SDK-shaped decoder stub. It tests AAC-LC AudioSpecificConfig validation, all nine standard sample rates from 8–48kHz in mono/stereo, 73 access units per format against an independent absolute-position PCM interpolation oracle, phase continuity across AU boundaries, 48kHz stereo output, callback blocks of at most 1,024 frames, EOF/reset/error latching, cancellation before/after decode and during output, allocation rollback, malformed/zero-progress decoder responses and callback failures. A 20,000-case bounded malformed-ASC sweep runs as well. The stub does **not** decode AAC and does not establish target throughput.

`generate.py` creates original zero-spectrum AAC-LC raw data blocks directly from syntax, plus AudioSpecificConfig and optional ADTS files. No recorded audio, third-party media or encoder is involved. `fixture.h` is checked for reproducibility on every run. Generate separate files for inspection with:

```sh
python3 tests/aac_runtime/generate.py --check --out-dir build/host_aac_runtime/fixtures
```

The mono AU is `00 00 00 07`, stereo `21 00 00 00 00 1c`; both have 1,024 samples per channel, `max_sfb=0`, no spectral coefficients, and 44.1kHz ASC. The 48-AU fixtures were additionally decoded through real MiniAAC 1.0.1 (0BSD), under ASan/UBSan: every decoded sample was zero. This validates the original raw AAC syntax independently; it is not a run of Espressif's RV32 binary.

The optional independent decoder test accepts a separately obtained `maac.h`; it does not download or vendor a second decoder into firmware:

```sh
MAAC_HEADER_DIR=/path/to/miniaac sh tests/aac_runtime/run.sh
```

The audited source was the unmodified MiniAAC header at [vgmstream's source mirror](https://github.com/vgmstream/vgmstream/blob/master/src/coding/libs/maac.h), SHA-256 `a3b9f2bff81a1c4a3393dbf94d842ef73fdb5f336f8abbe8c54d522bd72bc1cf`. Its SPDX 0BSD notice remains in that externally supplied header.

`target/` is an optional Unity test component for an existing ESP-IDF test application on the **actual ESP32-S31**, with PSRAM initialized. Add that directory to the test app's extra test-component paths, and run `[aac][s31]`. It runs production adapter code plus the **real official decoder**, mono/stereo for 48 AUs each, resets, then repeats; it checks silent PCM, exact source/output counts and format. Do not add it to the normal app or flash it without the device-test workflow. This target test has been provided but has **not** yet been executed on the device.

The required production dependency is official `espressif/esp_audio_codec` **2.6.2**, using `CONFIG_ESP_AUDIO_CODEC_S31_USE_ASM=n`. Official 2.6.2 fixes a mono AAC-LC interleaved-output overflow; its CMake selects `lib/esp32s31_esp_c/` when ASM is disabled. The decoder interface uses raw AAC (`no_adts_header=true`, `aac_plus_enable=false`, PCM16), so MP4 ASC is validated before opening it. HE-AAC/SBR/PS, PCE/channelConfig0, 960-sample frames and non-LC profiles are rejected. FFmpeg's optional explicit SBR-absent sync extension is accepted. See [official AAC header](https://github.com/espressif/esp-adf-libs/blob/master/esp_audio_codec/include/decoder/impl/esp_aac_dec.h), [changelog](https://github.com/espressif/esp-adf-libs/blob/master/esp_audio_codec/CHANGELOG.md), and the preserved [Espressif Modified MIT license](../../components/aether_mp4_player/LICENSE.esp_audio_codec).

The adapter's handle and status belong to one AAC-owner task. The worker must copy status into its own synchronized snapshot before other tasks inspect it. MP4 edit-list/priming trimming is owned by demux/worker, not this helper. Real A/V sync, non-silent AAC decoding, memory pressure, stop/unplug/pause while I2S blocks, and long-run S31 CPU/PSRAM performance still require separate worker/device acceptance. ESP32-S3 benchmark numbers are not evidence for ESP32-S31.
