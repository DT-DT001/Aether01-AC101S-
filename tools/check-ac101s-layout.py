#!/usr/bin/env python3
"""Classify a saved AC101S partition table and check migration space without writing flash."""

from argparse import ArgumentParser
from hashlib import sha256
from pathlib import Path


LEGACY_SHA256 = "48c17ee57491a58737e4998b3010918c3b4c3b552241c42f9cbcc5b0397fddc5"
WALLPAPER_SHA256 = "605b061ee00f33ec11f1c0f18b513cc833db8e6091405ce4bce7156a021e57c4"
TABLE_BYTES = 0xC00
TAIL_BYTES = 0x100000


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("table", type=Path, help="saved 0xc00 bytes from offset 0x8000")
    parser.add_argument("tail", nargs="?", type=Path, help="saved 1 MiB from offset 0x3f00000")
    args = parser.parse_args()
    try:
        table = args.table.read_bytes()
        if len(table) != TABLE_BYTES:
            raise ValueError("Read exactly 0xc00 bytes at partition-table offset 0x8000.")
        digest = sha256(table).hexdigest()
        if digest == WALLPAPER_SHA256:
            print("WALLPAPER_READY")
        elif digest != LEGACY_SHA256:
            raise ValueError("Unknown AC101S partition table. Leave the installed table intact.")
        elif args.tail is None:
            print("LEGACY")
        else:
            tail = args.tail.read_bytes()
            if len(tail) != TAIL_BYTES or tail.count(0xFF) != TAIL_BYTES:
                raise ValueError("The complete final 1 MiB is required and must be erased. Leave it intact.")
            print("WALLPAPER_MIGRATION_READY")
    except (OSError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
