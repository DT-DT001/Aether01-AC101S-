# GPT/FAT32 and disk-I/O ABI regression

Run `sh tests/fatfs_layout/run.sh` with the existing WSL/native host compiler.
No SDK or filesystem package is installed. The runner compiles the project-owned
`components/fatfs/src/ff.c`, `ffunicode.c` and `diskio/diskio.c` themselves with
ASan/UBSan and strict warnings. It uses the production owned ffconf.h; the stub
sdkconfig keeps label/fast-seek disabled and LFN heap/CP936/UTF8 enabled.

The first fixtures are original sparse in-memory30436MiB physical card images.
They have a protective MBR plus valid CRC'd GPT primary/backup headers and a
Microsoft Basic Data partition, or a conventional MBR FAT32 partition. FAT32
BPB/FAT/FSInfo, ASCII and Chinese LFN root entries and deterministic file bytes
are constructed directly; no user media or copied copyrighted files are used.
The real FatFs parser must mount, list both file names, read exact bytes and
seek across sector boundaries without any disk-write callback. Bad GPT header
CRC and non-basic-data GUID cases must fail without writes or allocations left.

The actual IDF diskio bridge is exercised against a deliberately32-bit callback
contract shared by installed SDMMC/rawflash/WL/BDL and managed USB MSC drivers.
GET_SECTOR_COUNT must initialize the entire64-bit LBA_t while preserving a
canary; failed ioctl leaves output untouched. CTRL_TRIM must preserve both
endpoints and reject reversed/too-wide ranges. Read/write requests above the
32-bit driver range, wrapping their last sector or with zero count are rejected
before driver invocation. WORD sector size and DWORD erase-block size remain
unchanged. This enables official GPT parsing, not arbitrary>2TiB driver support.

A separate RAM-only exFAT fixture is created by the real formatter, then mounted
again with writes forbidden and read/seek checked. No physical disk is accessed.
Both FAT32 and exFAT additionally execute real core mkdir/Chinese-name create,
40KB write/sync/close,2KB append, unmount/remount, exact42000-byte readback,
cross-cluster seek, delete and remount-confirm-missing. Allocation counters must
return to zero after each unmount. The readback phases must not write sectors.

These six groups establish core filesystem and callback-ABI behavior. They do
not qualify the physical card's wiring/CRC/power, sustained integrity, concurrent
VFS operations, application codec formats, or files exceeding the platform's
32-bit off_t/stdio range. VFS offset/stat bounds are tested separately; enabling
exFAT/LBA64 must never be interpreted as unlimited application file-size support.
