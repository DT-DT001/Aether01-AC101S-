"""Build the flash-resident Mandarin vocabulary from the pinned Wanxiang dictionary.

Download dicts/jichu.dict.yaml from WANXIANG_URL below, then pass that file
and --output to this tool. Use --check to verify an existing image, including
its optional JSON manifest, against the same source and firmware fonts.
"""

import argparse
from collections import defaultdict
from functools import lru_cache
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import zlib

from generate_pinyin_aosp_words import curated_words
from generate_pinyin_unihan import ASSETS, ROW, font_codepoints, spelling


WANXIANG_REVISION = "581925b26a89cd86387e20ffbd0ba746f89a81c9"
WANXIANG_URL = (
    "https://raw.githubusercontent.com/amzxyz/rime-wanxiang-data/"
    f"{WANXIANG_REVISION}/dicts/jichu.dict.yaml"
)
WANXIANG_SHA256 = "b0fb06be15dd61be72bce9784f676c18104abb503da09f41d4bfc795529facaf"
MINIMUM_WORDS = 100_000
MAXIMUM_BYTES = 3 * 1024 * 1024
MAXIMUM_SPELLING = 31
HEADER = struct.Struct("<4s9I")
OFFSETS = struct.Struct("<II")


def embedded_words():
    words = {word for _, word in curated_words()}
    source = (ASSETS / "pinyin_words_packed.inc").read_text(encoding="ascii")
    _, data = source.split("static const uint8_t common_word_data[] = {", 1)
    payload, _ = data.split("};", 1)
    packed = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-f]{2})\b", payload))
    if not packed:
        raise ValueError("Embedded dictionary word table is empty")
    cursor = 0
    while cursor < len(packed):
        size = 2 * ((packed[cursor] >> 6) + 2)
        end = cursor + 1 + size
        if end > len(packed):
            raise ValueError("Embedded dictionary contains an incomplete word record")
        words.add(packed[cursor + 1:end].decode("utf-16-le"))
        cursor = end
    return words


@lru_cache(maxsize=2048)
def normalized_syllable(raw):
    try:
        return spelling(raw)
    except ValueError:
        return ""


def source_words(path, excluded, renderable, syllables):
    if hashlib.sha256(path.read_bytes()).hexdigest() != WANXIANG_SHA256:
        raise ValueError(f"Expected the pinned Wanxiang dictionary at {WANXIANG_URL}")
    words = defaultdict(dict)
    with path.open(encoding="utf-8-sig") as source:
        for line in source:
            row = line.rstrip().split("\t")
            if len(row) != 3:
                continue
            word, reading, score = row
            if not 2 <= len(word) <= 5 or word in excluded or not score.isascii() or not score.isdecimal():
                continue
            if not all(0x3400 <= ord(char) <= 0x9FFF and ord(char) in renderable for char in word):
                continue
            units = [normalized_syllable(raw) for raw in reading.split()]
            if len(units) != len(word) or any(unit not in syllables for unit in units):
                continue
            key = "".join(units)
            if len(key) > MAXIMUM_SPELLING:
                continue
            rank = int(score)
            readings = words[word]
            readings[key] = max(rank, readings.get(key, 0))
    return words


def pack_words(words):
    best_ranks = {word: max(readings.values()) for word, readings in words.items()}
    ranked_words = sorted(best_ranks, key=lambda word: (-best_ranks[word], word))[:MINIMUM_WORDS]
    if len(ranked_words) < MINIMUM_WORDS:
        raise ValueError(f"Only {len(ranked_words)} distinct renderable words from pinned dictionary")

    strongest = max(best_ranks.values())
    log_range = math.log1p(strongest)
    by_key = defaultdict(list)
    for word in ranked_words:
        for key, score in words[word].items():
            by_key[key].append((word, score))

    keys_blob, words_blob, index = bytearray(), bytearray(), bytearray()
    reading_count = 0
    for key, readings in sorted(by_key.items()):
        index.extend(OFFSETS.pack(len(keys_blob), len(words_blob)))
        keys_blob.extend(key.encode("ascii") + b"\0")
        for word, score in sorted(readings, key=lambda row: (-row[1], row[0])):
            # Rare extended spellings stay eligible without taking over the
            # short list of high-frequency spellings embedded in the app.
            weight = 1 + round(31 * math.log1p(score) / log_range) if log_range else 1
            words_blob.append(((len(word) - 2) << 6) | weight)
            words_blob.extend(word.encode("utf-16-le"))
            reading_count += 1
    index.extend(OFFSETS.pack(len(keys_blob), len(words_blob)))

    keys_offset = HEADER.size + len(index)
    words_offset = keys_offset + len(keys_blob)
    size = words_offset + len(words_blob)
    if size > MAXIMUM_BYTES:
        raise ValueError(f"The extension is {size} bytes, larger than its {MAXIMUM_BYTES}-byte flash region")
    payload = index + keys_blob + words_blob
    crc = zlib.crc32(payload)
    header = HEADER.pack(b"PYN2", 1, size, crc, len(by_key), reading_count,
                         HEADER.size, keys_offset, words_offset, 0)
    metadata = {
        "extension_unique_words": len(ranked_words),
        "extension_word_readings": reading_count,
        "extension_spellings": len(by_key),
        "bytes": size,
        "crc32": f"{crc:08x}",
        "source": WANXIANG_URL,
        "source_sha256": WANXIANG_SHA256,
    }
    return header + payload, metadata


def generate(source):
    renderable = font_codepoints(ASSETS / "font_16.c") & font_codepoints(ASSETS / "font_24.c")
    syllables = {key for key, _ in ROW.findall((ASSETS / "pinyin_unihan.inc").read_text(encoding="utf-8"))}
    hot = embedded_words()
    image, metadata = pack_words(source_words(source, hot, renderable, syllables))
    metadata["sha256"] = hashlib.sha256(image).hexdigest()
    metadata["embedded_unique_words"] = len(hot)
    metadata["total_unique_words"] = len(hot) + metadata["extension_unique_words"]
    return image, metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dictionary", type=Path, help="Wanxiang dicts/jichu.dict.yaml")
    parser.add_argument("--output", required=True, type=Path, help="Output flash-region image")
    parser.add_argument("--manifest", type=Path, help="Optional JSON manifest")
    parser.add_argument("--check", action="store_true", help="Compare with existing image instead of writing")
    args = parser.parse_args()

    image, metadata = generate(args.dictionary)
    manifest = (json.dumps(metadata, indent=2) + "\n").encode("utf-8")
    if args.check:
        if args.output.read_bytes() != image:
            parser.error("the extension image differs from regenerated output")
        if args.manifest and args.manifest.read_bytes() != manifest:
            parser.error("the extension manifest differs from regenerated output")
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(image)
        if args.manifest:
            args.manifest.write_bytes(manifest)
    print(manifest.decode("utf-8"), end="")


if __name__ == "__main__":
    main()
