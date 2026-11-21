"""Compile the current production VFS functions, not copied test implementations."""
import pathlib
import re
import sys

source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
names = [
    "get_next_fd", "fat_mode_conv", "fresult_to_errno", "file_cleanup",
    "prepend_drive_to_path", "vfs_fat_max_offset", "vfs_fat_file_in_bounds",
    "vfs_fat_transfer_in_bounds", "vfs_fat_open", "vfs_fat_write",
    "vfs_fat_read", "vfs_fat_pread", "vfs_fat_pwrite", "vfs_fat_lseek",
    "vfs_fat_fstat", "get_stat_mode", "update_stat_struct",
    "vfs_fat_clear_dir_stat_cache", "vfs_fat_invalidate_stat_cache",
    "vfs_fat_consume_stat_cache", "vfs_fat_stat",
    "vfs_fat_link", "f_write_zero_mem", "vfs_fat_truncate", "vfs_fat_ftruncate",
]
out = []
for name in names:
    match = re.search(r"^static[^\n]*\b" + name + r"\s*\([^;]*?\)\s*\{", source, re.M)
    if match is None:
        raise RuntimeError(f"Cannot locate production definition {name}")
    # Mask comments and literals so braces there cannot terminate a definition.
    start = source.index("{", match.start())
    tail = source[start:]
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: " " * len(m.group()), tail, flags=re.S)
    depth = 0
    for index, char in enumerate(masked):
        depth += (char == "{") - (char == "}")
        if depth == 0:
            out.append(source[match.start():start + index + 1])
            break
    else:
        raise RuntimeError(f"Unbalanced definition {name}")
pathlib.Path(sys.argv[2]).write_text("\n\n".join(out) + "\n", encoding="utf-8")
