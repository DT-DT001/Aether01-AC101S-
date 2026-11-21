"""Generate an original standards-valid silent MPEG-1 Layer III test stream.

Each frame contains empty granules (all part2_3_length values are zero), a zero
bit reservoir, and no copyrighted audio. Stereo/mono frame headers use 128kbps
at 44.1kHz. No encoder installation is required. Regenerate with Python 3.
"""
from pathlib import Path
import math
import struct
import wave

destination = Path(__file__).parent / "fixtures"
destination.mkdir(exist_ok=True)
for name, mode in [("silence-stereo.mp3", 0), ("silence-mono.mp3", 0xC0)]:
    # No padding: frame length floor(144 * 128000 / 44100) = 417 bytes.
    frame = bytes([0xFF, 0xFB, 0x90, mode]) + bytes(413)
    (destination / name).write_bytes(frame * 40)
print("Generated 40-frame mono/stereo silent MPEG-1 Layer III fixtures.")

# Original two-tone source for a non-silent decoder regression. Encode with
# mp3-enc-bsd (instructions and pinned source in fixtures/README.md).
with wave.open(str(destination / "sine-source.wav"), "wb") as audio:
    audio.setnchannels(2)
    audio.setsampwidth(2)
    audio.setframerate(44100)
    audio.writeframes(b"".join(struct.pack("<hh",
        round(12000 * math.sin(2 * math.pi * 440 * i / 44100)),
        round(12000 * math.sin(2 * math.pi * 880 * i / 44100)))
        for i in range(44100)))
