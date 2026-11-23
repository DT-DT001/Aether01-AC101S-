"""Build the firmware's offline Mandarin word table from pinned open dictionaries.

Pass the base64-decoded Android PinyinIME dictionary, phrase-pinyin-data pinyin.txt
and jieba dict.txt in that order. The constants below record the download URLs.
Add --check to compare with the checked-in output without writing it.
"""

import argparse
from collections import defaultdict
from hashlib import sha256
import math
from pathlib import Path
from statistics import median

from generate_pinyin_aosp_words import curated_words, read_word_frequencies
from generate_pinyin_unihan import ASSETS, font_codepoints, spelling


PHRASE_URL = "https://raw.githubusercontent.com/mozillazg/phrase-pinyin-data/v0.20.0/pinyin.txt"
PHRASE_SHA256 = "5d4d86b1337bb2fca95bc5135564a821bfdb40597481ee3e3871bf52d4668622"
JIEBA_URL = "https://raw.githubusercontent.com/fxsjy/jieba/v0.42.1/jieba/dict.txt"
JIEBA_SHA256 = "7197c3211ddd98962b036cdf40324d1ea2bfaa12bd028e68faa70111a88e12a8"
ORIGINAL_WORD_LIMIT = 8000
WORD_LIMIT = 17000
NEW_SOURCE_MINIMUM = 2000
BLOCK_SIZE = 256


def checked_text(path, url, expected_sha256):
    raw = path.read_bytes()
    if sha256(raw).hexdigest() != expected_sha256:
        raise ValueError(f"Expected the unmodified source at {url}")
    return raw.decode("utf-8").splitlines()


def renderable_word(word, renderable):
    return 2 <= len(word) <= 5 and all(
        0x3400 <= ord(char) <= 0x9FFF and ord(char) in renderable for char in word
    )


def jieba_counts(path, renderable):
    counts = {}
    for line in checked_text(path, JIEBA_URL, JIEBA_SHA256):
        word, frequency, _part_of_speech = line.split()
        if renderable_word(word, renderable):
            counts[word] = max(int(frequency), counts.get(word, 0))
    return counts


def supplemental_readings(path, counts):
    readings = set()
    for line in checked_text(path, PHRASE_URL, PHRASE_SHA256):
        content = line.split("#", 1)[0].strip()
        if not content:
            continue
        word, phonetics = content.split(": ", 1)
        if word not in counts:
            continue
        syllables = phonetics.split()
        if len(syllables) != len(word):
            continue
        # Some rows contain a Latin abbreviation; only keep Mandarin readings.
        try:
            key = "".join(spelling(syllable) for syllable in syllables)
        except ValueError:
            continue
        if len(key) <= 31:
            readings.add((key, word))
    return readings


def select_words(aosp_path, phrases_path, jieba_path):
    renderable = font_codepoints(ASSETS / "font_16.c") & font_codepoints(ASSETS / "font_24.c")
    aosp = read_word_frequencies(aosp_path, renderable)
    jieba = jieba_counts(jieba_path, renderable)
    extras = supplemental_readings(phrases_path, jieba) - curated_words()
    aosp_words = defaultdict(float)
    for (_, word), frequency in aosp.items():
        aosp_words[word] = max(frequency, aosp_words[word])

    # Calibrate the independent corpora using shared words, without treating
    # section-level frequencies and raw counts as comparable quantities.
    scale = median(
        frequency / jieba[word] for word, frequency in aosp_words.items()
        if jieba.get(word, 0) >= 100
    )
    scores = {
        pair: max(aosp.get(pair, 0.0), jieba.get(pair[1], 0) * scale)
        for pair in aosp.keys() | extras
    }

    original = sorted(aosp, key=lambda pair: (-aosp[pair], pair))[:ORIGINAL_WORD_LIMIT]
    ranked = sorted(scores, key=lambda pair: (-scores[pair], pair))
    distinct_extra = [pair for pair in ranked if pair in extras and pair[1] not in aosp_words]
    selected = set(original) | set(distinct_extra[:NEW_SOURCE_MINIMUM])
    for pair in ranked:
        if len(selected) == WORD_LIMIT:
            break
        selected.add(pair)
    if len(selected) != WORD_LIMIT:
        raise ValueError("The pinned dictionaries did not provide enough renderable entries")

    by_key = defaultdict(list)
    maximum, minimum = max(scores[pair] for pair in selected), min(scores[pair] for pair in selected)
    for pair in sorted(selected, key=lambda pair: (-scores[pair], pair)):
        score = math.log1p(scores[pair] / minimum) / math.log1p(maximum / minimum)
        weight = max(1, min(63, round(63 * score)))
        by_key[pair[0]].append((pair[1], weight))
    return by_key, selected, aosp_words


def c_array(declaration, values, columns):
    rows = [", ".join(values[index:index + columns]) for index in range(0, len(values), columns)]
    return declaration + " = {\n" + "".join(f"    {row},\n" for row in rows) + "};\n\n"


def generate(aosp_path, phrases_path, jieba_path):
    by_key, selected, aosp_words = select_words(aosp_path, phrases_path, jieba_path)
    keys = sorted(by_key)
    spelling_lines = ["".join(key + r"\0" for key in keys[i:i + 7]) for i in range(0, len(keys), 7)]
    spelling_blob = "static const char common_spellings[] =\n" + "".join(f'    "{line}"\n' for line in spelling_lines)
    letters, words = 0, bytearray()
    offsets, spelling_bases, word_bases = [], [], []
    for index, key in enumerate(keys + [""]):
        if index % BLOCK_SIZE == 0:
            spelling_bases.append(letters)
            word_bases.append(len(words))
        offsets.append((letters - spelling_bases[-1], len(words) - word_bases[-1]))
        if max(offsets[-1]) > 0xFFFF:
            raise ValueError("Dictionary block exceeds the 16-bit offset format")
        if index == len(keys):
            break
        letters += len(key) + 1
        for word, weight in by_key[key]:
            words.append(((len(word) - 2) << 6) | weight)
            words.extend(word.encode("utf-16-le"))

    numeric = lambda values: [str(value) for value in values]
    generated = (
        "/* Generated by tools/generate_pinyin_words.py.\n"
        " * Derived from Android PinyinIME, phrase-pinyin-data and jieba.\n"
        " * See pinyin_aosp_words.LICENSE.txt and pinyin_words_extra.LICENSE.txt. */\n\n"
        f"enum {{ COMMON_PHRASE_COUNT = {len(keys)}, COMMON_BLOCK_SIZE = {BLOCK_SIZE} }};\n\n"
        f"{spelling_blob};\n\n"
        + c_array("static const uint32_t common_spelling_bases[]", numeric(spelling_bases), 10)
        + c_array("static const uint32_t common_word_bases[]", numeric(word_bases), 10)
        + c_array("static const struct { uint16_t spelling, words; } common_offsets[]",
                  [f"{{{key}, {value}}}" for key, value in offsets], 6)
        + c_array("static const uint8_t common_word_data[]", [f"0x{byte:02x}" for byte in words], 16)
    ).rstrip("\n").encode("ascii") + b"\n"
    size = letters + 1 + len(words) + 4 * len(offsets) + 8 * len(spelling_bases)
    word_set = {word for _, word in selected}
    return generated, len(word_set), len(selected), len(keys), len(word_set - aosp_words.keys()), size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("aosp", type=Path, help="base64-decoded Android PinyinIME dictionary")
    parser.add_argument("phrases", type=Path, help="UTF-8 pinyin.txt from phrase-pinyin-data")
    parser.add_argument("jieba", type=Path, help="UTF-8 jieba/dict.txt")
    parser.add_argument("--check", action="store_true", help="compare with the checked-in output")
    args = parser.parse_args()

    generated, words, readings, keys, extra, size = generate(args.aosp, args.phrases, args.jieba)
    destination = ASSETS / "pinyin_words_packed.inc"
    if args.check:
        if destination.read_bytes().replace(b"\r\n", b"\n") != generated:
            parser.error("pinyin_words_packed.inc differs from regenerated output")
    else:
        destination.write_bytes(generated)
    print(f"{words} unique words, {readings} word readings, {keys} spellings, "
          f"{extra} words absent from Android PinyinIME, {size} packed bytes")


if __name__ == "__main__":
    main()
