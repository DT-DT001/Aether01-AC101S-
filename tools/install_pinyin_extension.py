#!/usr/bin/env python3
"""Check or provision the Aether01 AC101S internal-flash Pinyin dictionary."""

import argparse
import hashlib
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import zlib


SECTOR = 0x1000
PARTITION_TABLE_AT = 0x8000
PARTITION_TABLE_SIZE = 0xC00
STORAGE_START = 0x850000
FLASH_END = 0x4000000
WALLPAPER_START = 0x3F00000
EXTENSION_START = 0x900000
EXTENSION_SIZE = 0x300000
HEADER = struct.Struct('<4s9I')
OFFSET = struct.Struct('<II')
PARTITION_ROW = struct.Struct('<2sBBII16sI')
PARTITION_MD5 = b'\xEB\xEB' + b'\xFF' * 14


def read_header(data):
    if len(data) < HEADER.size:
        raise ValueError('Pinyin dictionary header is incomplete')
    magic, version, size, checksum, keys, words, table, letters, records, reserved = HEADER.unpack_from(data)
    if (magic != b'PYN2' or version != 1 or reserved or not keys or words < keys or
            table != HEADER.size or letters != table + (keys + 1) * OFFSET.size or
            not letters < records < size <= EXTENSION_SIZE):
        raise ValueError('Invalid PYN2 dictionary header or section limits')
    return size, checksum, keys, words, table, letters, records


def check_dictionary(data):
    size, checksum, keys, words, table, letters, records = read_header(data)
    if size != len(data) or zlib.crc32(memoryview(data)[table:]) != checksum:
        raise ValueError('Pinyin dictionary length or CRC-32 does not match its header')
    left, bottom = OFFSET.unpack_from(data, table)
    if left != 0 or bottom != 0:
        raise ValueError('Pinyin dictionary offsets must start at zero')
    prior_key = b''
    word_count = 0
    for index in range(1, keys + 1):
        right, top = OFFSET.unpack_from(data, table + index * OFFSET.size)
        if not left < right <= records - letters or not bottom < top <= size - records:
            raise ValueError(f'Invalid Pinyin offsets at key {index}')
        key = data[letters + left:letters + right]
        if (not 3 <= len(key) <= 32 or key[-1] != 0 or key <= prior_key or
                not all(ord('a') <= byte <= ord('z') for byte in key[:-1])):
            raise ValueError(f'Invalid or unsorted Pinyin spelling at key {index}')
        prior_key = key
        cursor, end = records + bottom, records + top
        while cursor < end:
            count = (data[cursor] >> 6) + 2
            cursor += 1 + 2 * count
            word_count += 1
        if cursor != end:
            raise ValueError(f'Truncated Pinyin word record at key {index}')
        left, bottom = right, top
    if (left, bottom, word_count) != (records - letters, size - records, words):
        raise ValueError('Pinyin dictionary final offsets or record count do not match')
    return keys, words


def check_partition_table(data):
    if len(data) != PARTITION_TABLE_SIZE:
        raise ValueError('Incomplete partition table read')
    storage = None
    wallpaper = None
    for start in range(0, len(data), PARTITION_ROW.size):
        row = data[start:start + PARTITION_ROW.size]
        if row.startswith(PARTITION_MD5):
            if hashlib.md5(data[:start]).digest() != row[16:]:
                raise ValueError('Invalid live partition table checksum')
            break
        magic, kind, subtype, address, count, raw_name, flags = PARTITION_ROW.unpack(row)
        if magic != b'\xAA\x50' or count == 0 or address + count > FLASH_END:
            raise ValueError('Invalid live AC101S partition table')
        name = raw_name.split(b'\0', 1)[0]
        if name == b'storage':
            if storage is not None:
                raise ValueError('Duplicate live storage partition')
            storage = kind, subtype, address, count, flags
        elif name == b'wallpaper':
            if wallpaper is not None:
                raise ValueError('Duplicate live wallpaper partition')
            wallpaper = kind, subtype, address, count, flags
        elif address < EXTENSION_START + EXTENSION_SIZE and address + count > EXTENSION_START:
            raise ValueError('Another partition overlaps the dictionary region')
    else:
        raise ValueError('Missing live partition table checksum')
    storage_end = FLASH_END if wallpaper is None else WALLPAPER_START
    if wallpaper is not None and wallpaper != (1, 0x40, WALLPAPER_START, FLASH_END - WALLPAPER_START, 0):
        raise ValueError('Unsupported live AC101S wallpaper partition layout')
    if storage != (1, 0x81, STORAGE_START, storage_end - STORAGE_START, 0):
        raise ValueError('Unsupported live AC101S storage partition layout')
    return storage_end


class Device:
    def __init__(self, port, mac, baud):
        self.port, self.mac, self.baud = port, mac, baud

    def run(self, *args, after='no-reset'):
        from serial.tools import list_ports

        ports = [port for port in list_ports.comports() if port.device.casefold() == self.port.casefold()]
        if (len(ports) != 1 or (ports[0].vid, ports[0].pid) != (0x303A, 0x1001) or
                (ports[0].serial_number or '').upper() != self.mac):
            raise RuntimeError('Specified port no longer has the requested ESP32-S31 USB identity')
        command = [sys.executable, '-m', 'esptool', '--chip', 'esp32s31', '--port', self.port,
                   '--baud', str(self.baud), '--connect-attempts', '3', '--before', 'default-reset',
                   '--after', after, *args]
        process = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace',
                                 timeout=240, check=False)
        if process.returncode:
            raise RuntimeError((process.stdout + process.stderr).strip())
        return process.stdout

    def identify(self):
        output = self.run('read-mac')
        if not re.search(r'^MAC:\s*' + re.escape(self.mac) + r'\s*$', output, re.IGNORECASE | re.MULTILINE):
            raise RuntimeError('MAC returned by the bootloader does not match the requested board')

    def read(self, start, length, output):
        self.run('read-flash', '--flash-size', '64MB', hex(start), hex(length), str(output))
        data = output.read_bytes()
        if len(data) != length:
            raise RuntimeError('Incomplete flash read')
        return data


def preflight(device, folder):
    storage_end = check_partition_table(
        device.read(PARTITION_TABLE_AT, PARTITION_TABLE_SIZE, folder / 'table.bin'))
    head = device.read(EXTENSION_START, SECTOR, folder / 'header.bin')
    old = b''
    if head != b'\xFF' * SECTOR:
        size, *_ = read_header(head)
        old = device.read(EXTENSION_START, size, folder / 'old-pinyin.bin')
        check_dictionary(old)

    # A raw FAT image starts with its boot record. The end of a wear-leveled
    # FAT volume contains its metadata. Require the anchors and all bytes in
    # the reserved band to match an erased region or precisely this dictionary.
    blank = folder / 'blank-sector.bin'
    blank.write_bytes(b'\xFF' * SECTOR)
    expected = folder / 'expected-bank.bin'
    expected.write_bytes(old + b'\xFF' * (EXTENSION_SIZE - len(old)))
    try:
        device.run('verify-flash', '--flash-size', '64MB', hex(STORAGE_START), str(blank),
                   hex(EXTENSION_START - SECTOR), str(blank), hex(storage_end - SECTOR), str(blank),
                   hex(EXTENSION_START), str(expected))
    except RuntimeError as error:
        raise RuntimeError(f'Cannot establish an unused or known dictionary bank. No bytes written.\n{error}') from error
    return old


def install(device, folder, previous, dictionary):
    span = (max(len(previous), len(dictionary)) + SECTOR - 1) // SECTOR * SECTOR
    payload = folder / 'pinyin-write.bin'
    payload.write_bytes(dictionary + b'\xFF' * (span - len(dictionary)))
    device.run('write-flash', '--compress', '--flash-size', '64MB', hex(EXTENSION_START), str(payload))
    device.run('verify-flash', '--flash-size', '64MB', hex(EXTENSION_START), str(payload))


def run_installation(device, folder, data, perform_write):
    device.identify()
    try:
        old = preflight(device, folder)
        if perform_write and old != data:
            install(device, folder, old, data)
            result = 'Installed and verified the Pinyin dictionary in flash.'
        elif old == data:
            result = 'The requested Pinyin dictionary is already installed and verified.'
        else:
            result = 'Flash bank validated. No bytes written.'
    except (OSError, subprocess.SubprocessError, ValueError, RuntimeError):
        try:
            device.run('read-mac', after='hard-reset')
        except (OSError, subprocess.SubprocessError, RuntimeError) as error:
            print(f'Could not reset the device after the check: {error}', file=sys.stderr)
        raise
    device.run('read-mac', after='hard-reset')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=('check', 'install'), help='check is read-only')
    parser.add_argument('dictionary', type=Path)
    parser.add_argument('--port', required=True)
    parser.add_argument('--mac', required=True, type=str.upper)
    parser.add_argument('--sha256', required=True, type=str.lower, help='expected hash from the dictionary manifest')
    parser.add_argument('--baud', type=int, default=460800)
    args = parser.parse_args()
    if not re.fullmatch(r'(?:[0-9A-F]{2}:){5}[0-9A-F]{2}', args.mac):
        parser.error('--mac must contain the six colon-separated octets')
    if not re.fullmatch('[0-9a-f]{64}', args.sha256):
        parser.error('--sha256 must be 64 hexadecimal digits')
    try:
        data = args.dictionary.read_bytes()
        if hashlib.sha256(data).hexdigest() != args.sha256:
            raise ValueError('Pinyin dictionary SHA-256 does not match the supplied manifest')
        keys, words = check_dictionary(data)
        device = Device(args.port, args.mac, args.baud)
        with tempfile.TemporaryDirectory(prefix='aether-pinyin-') as scratch:
            print(f'Validated {keys} spellings and {words} readings for {EXTENSION_START:#x}.', flush=True)
            print(run_installation(device, Path(scratch), data, args.mode == 'install'))
    except (OSError, subprocess.SubprocessError, ValueError, RuntimeError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
