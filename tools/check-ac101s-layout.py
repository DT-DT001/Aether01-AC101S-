#!/usr/bin/env python3
"""Classify a saved AC101S partition table and check migration space without writing flash."""

from argparse import ArgumentParser
from hashlib import sha256
from pathlib import Path


LEGACY_SHA256 = "48c17ee57491a58737e4998b3010918c3b4c3b552241c42f9cbcc5b0397fddc5"
WALLPAPER_SHA256 = "605b061ee00f33ec11f1c0f18b513cc833db8e6091405ce4bce7156a021e57c4"
AI_SHA256 = {
    "7ed72c296ec9a4f98d6e1b016cef081cc3b6dedf3c65f13b5f12b21f5b374197",
    "30fe3465391a8f2489af6acb55e532a65b5ef01686d2144ec98e5363ff967ec5",
    "df732263a7f2cd68d92f35b3a15c40b3d7b1508871fb64920287725f3ae5d79b",
}
TABLE_BYTES = 0xC00
TAIL_BYTES = 0x100000
SECTOR_BYTES = 0x1000


def erased(path, size):
    data = path.read_bytes()
    return len(data) == size and data.count(0xFF) == size


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("table", type=Path, help="saved 0xc00 bytes from offset 0x8000")
    parser.add_argument("tail", nargs="?", type=Path, help="saved 1 MiB from offset 0x3f00000")
    parser.add_argument("--ai-region", type=Path, help="saved bytes from 0x3e00000 (1 MiB with wallpaper, 2 MiB on legacy)")
    parser.add_argument("--storage-head", type=Path, help="saved 4 KiB from the internal storage start at 0x850000")
    args = parser.parse_args()
    try:
        table = args.table.read_bytes()
        if len(table) != TABLE_BYTES:
            raise ValueError("Read exactly 0xc00 bytes at partition-table offset 0x8000.")
        digest = sha256(table).hexdigest()
        if digest in AI_SHA256:
            print("AI_READY")
        elif args.ai_region is not None or args.storage_head is not None:
            if digest not in (WALLPAPER_SHA256, LEGACY_SHA256):
                raise ValueError("Unknown AC101S partition table. Leave the installed table intact.")
            expected = TAIL_BYTES if digest == WALLPAPER_SHA256 else 2 * TAIL_BYTES
            if (args.ai_region is None or args.storage_head is None or
                    not erased(args.ai_region, expected) or not erased(args.storage_head, SECTOR_BYTES)):
                raise ValueError("The complete reserved range and the start of storage must be erased. Back up and migrate formatted storage first.")
            print("AI_MIGRATION_READY")
        elif digest == WALLPAPER_SHA256:
            print("WALLPAPER_READY")
        elif digest != LEGACY_SHA256:
            raise ValueError("Unknown AC101S partition table. Leave the installed table intact.")
        elif args.tail is None:
            print("LEGACY")
        else:
            if not erased(args.tail, TAIL_BYTES):
                raise ValueError("The complete final 1 MiB is required and must be erased. Leave it intact.")
            print("WALLPAPER_MIGRATION_READY")
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
