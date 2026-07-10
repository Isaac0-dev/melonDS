#!/usr/bin/env python3
import argparse
import struct
import sys


HEADER_SIZE = 0x10
SECTION_HEADER_SIZE = 0x10


def read_sections(path):
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < HEADER_SIZE or data[:4] != b"MELN":
        raise ValueError(f"{path}: not a melonDS savestate")

    declared_len = struct.unpack_from("<I", data, 0x08)[0]
    if declared_len and declared_len <= len(data):
        data = data[:declared_len]

    sections = []
    offset = HEADER_SIZE
    while offset + SECTION_HEADER_SIZE <= len(data):
        magic = data[offset:offset + 4].decode("ascii", errors="replace")
        section_len = struct.unpack_from("<I", data, offset + 4)[0]
        if section_len < SECTION_HEADER_SIZE or offset + section_len > len(data):
            break
        body_start = offset + SECTION_HEADER_SIZE
        body_end = offset + section_len
        sections.append((magic, offset, data[body_start:body_end]))
        offset += section_len

    return data, sections


def first_diff(a, b):
    limit = min(len(a), len(b))
    for i in range(limit):
        if a[i] != b[i]:
            return i
    if len(a) != len(b):
        return limit
    return None


def hex_window(data, offset, width):
    start = max(0, offset - width)
    end = min(len(data), offset + width)
    return data[start:end].hex(" ")


def main():
    parser = argparse.ArgumentParser(description="Diff two melonDS savestates by section.")
    parser.add_argument("left")
    parser.add_argument("right")
    parser.add_argument("--context", type=int, default=16, help="bytes to show around the first differing byte")
    args = parser.parse_args()

    left_data, left_sections = read_sections(args.left)
    right_data, right_sections = read_sections(args.right)

    whole_diff = first_diff(left_data, right_data)
    if whole_diff is None:
        print("Savestates are byte-identical.")
        return 0

    print(f"First file-level difference at 0x{whole_diff:08X}")

    max_sections = max(len(left_sections), len(right_sections))
    found_section_diff = False
    for i in range(max_sections):
        if i >= len(left_sections):
            magic, offset, body = right_sections[i]
            print(f"+ section[{i}] {magic} only in right at 0x{offset:08X}, body {len(body)} bytes")
            found_section_diff = True
            continue
        if i >= len(right_sections):
            magic, offset, body = left_sections[i]
            print(f"- section[{i}] {magic} only in left at 0x{offset:08X}, body {len(body)} bytes")
            found_section_diff = True
            continue

        left_magic, left_offset, left_body = left_sections[i]
        right_magic, right_offset, right_body = right_sections[i]
        if left_magic != right_magic:
            print(f"! section[{i}] magic differs: left {left_magic} at 0x{left_offset:08X}, right {right_magic} at 0x{right_offset:08X}")
            found_section_diff = True
            continue

        diff = first_diff(left_body, right_body)
        if diff is None:
            continue

        print(f"! section[{i}] {left_magic} differs at body offset 0x{diff:08X} "
              f"(file left 0x{left_offset + SECTION_HEADER_SIZE + diff:08X}, "
              f"right 0x{right_offset + SECTION_HEADER_SIZE + diff:08X})")
        print(f"  left : {hex_window(left_body, diff, args.context)}")
        print(f"  right: {hex_window(right_body, diff, args.context)}")
        found_section_diff = True

    if not found_section_diff:
        print("Section bodies match; difference is in savestate header/section metadata.")

    return 1


if __name__ == "__main__":
    sys.exit(main())
