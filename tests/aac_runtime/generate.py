"""Generate original zero-spectrum AAC-LC test AUs; no encoder or copyrighted media.

These raw_data_blocks contain max_sfb=0 and no spectral coefficients. They are
silence generated from syntax, not excerpts of recordings. A second real AAC
decoder can validate them with oracle.c; the ESP32-S31 acceptance uses the SDK.
"""
import argparse
from pathlib import Path


class Bits:
    def __init__(self):
        self.bits = []

    def put(self, value, count):
        assert 0 <= value < 1 << count
        self.bits.extend((value >> bit) & 1 for bit in reversed(range(count)))

    def bytes(self):
        bits = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(sum(bits[index + bit] << (7 - bit) for bit in range(8))
                     for index in range(0, len(bits), 8))


def ics_info(bits):
    bits.put(0, 1)  # reserved
    bits.put(0, 2)  # ONLY_LONG_SEQUENCE
    bits.put(0, 1)  # sine window
    bits.put(0, 6)  # max_sfb=0: no bands, scalefactors or spectral coefficients
    bits.put(0, 1)  # predictor_data_present


def channel(bits, common_window):
    bits.put(0, 8)  # global_gain
    if not common_window:
        ics_info(bits)
    bits.put(0, 1)  # pulse_data_present
    bits.put(0, 1)  # tns_data_present
    bits.put(0, 1)  # gain_control_data_present


def access_unit(channels):
    bits = Bits()
    bits.put(0 if channels == 1 else 1, 3)  # SCE / CPE
    bits.put(0, 4)  # element_instance_tag
    if channels == 2:
        bits.put(1, 1)  # common_window
        ics_info(bits)
        bits.put(0, 2)  # ms_mask_present
    for _ in range(channels):
        channel(bits, channels == 2)
    bits.put(7, 3)  # ID_END; zero byte alignment supplied by bytes()
    return bits.bytes()


def asc(channels):
    bits = Bits()
    bits.put(2, 5)  # AAC-LC AOT2
    bits.put(4, 4)  # 44.1kHz
    bits.put(channels, 4)
    bits.put(0, 3)  # 1024 frame, no core dependency/extension
    return bits.bytes()


def adts(payload, channels):
    length = len(payload) + 7
    return bytes((0xFF, 0xF1, (1 << 6) | (4 << 2) | (channels >> 2),
                  ((channels & 3) << 6) | (length >> 11),
                  (length >> 3) & 255, ((length & 7) << 5) | 31, 0xFC)) + payload


def header():
    lines = ["/* Original generated AAC-LC silence, no recorded media. Run generate.py. */",
             "#pragma once", "#include <stdint.h>", "#define AAC_FIXTURE_AUS 48U"]
    for channels, name in ((1, "MONO"), (2, "STEREO")):
        for suffix, value in (("ASC", asc(channels)), ("AU", access_unit(channels))):
            values = ", ".join(f"0x{byte:02x}" for byte in value)
            lines.append(f"static const uint8_t AAC_FIXTURE_{name}_{suffix}[] = {{{values}}};")
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--out-dir", type=Path)
    args = parser.parse_args()
    target = Path(__file__).with_name("fixture.h")
    if args.check:
        assert target.read_text(encoding="utf-8") == header(), "fixture.h differs; regenerate"
    else:
        target.write_text(header(), encoding="utf-8", newline="\n")
    if args.out_dir:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        for channels, name in ((1, "mono"), (2, "stereo")):
            payload = access_unit(channels)
            (args.out_dir / f"{name}.au").write_bytes(payload)
            (args.out_dir / f"{name}.asc").write_bytes(asc(channels))
            (args.out_dir / f"{name}.aac").write_bytes(adts(payload, channels) * 48)
