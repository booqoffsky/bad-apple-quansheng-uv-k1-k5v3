#!/usr/bin/env python3
"""
Convert 128x64 1-bit PNGs into a single RLE-compressed binary for external flash.

Copyright (c) 2026 booqoffsky

Licensed under the MIT License (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at the root of this repository.

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
"""

import argparse
import os
import re
import struct
import sys

from PIL import Image

LCD_WIDTH = 128
LCD_HEIGHT = 64
PAGE_COUNT = LCD_HEIGHT // 8
FRAME_BYTES = LCD_WIDTH * PAGE_COUNT

BAD_APPLE_MAGIC = 0x0BADCAFE
BAD_APPLE_VERSION = 1


def pack_image(img):
    """Return a 1024-byte ST7565 page-major buffer from a 128x64 data."""
    if img.size != (LCD_WIDTH, LCD_HEIGHT):
        raise ValueError(f"Expected {LCD_WIDTH}x{LCD_HEIGHT}, got {img.size}")

    bw = img.convert("1").point(lambda v: 1 if v == 0 else 0, mode="1")
    px = bw.load()

    buf = bytearray(FRAME_BYTES)
    for page in range(PAGE_COUNT):
        base_row = page * 8
        for x in range(LCD_WIDTH):
            byte_val = 0
            for bit in range(8):
                if px[x, base_row + bit]:
                    byte_val |= 1 << bit
            buf[page * LCD_WIDTH + x] = byte_val
    return bytes(buf)


def rle_encode(buf):
    """Run-length encode `buf` as a stream of [count, value] byte pairs."""
    out = bytearray()
    i = 0
    n = len(buf)
    while i < n:
        value = buf[i]
        run = 1
        while i + run < n and buf[i + run] == value and run < 255:
            run += 1
        out.append(run)
        out.append(value)
        i += run
    return bytes(out)


def build_rle_index(packed_frames):
    """RLE-encode packed frames and build an offset table."""
    encoded = [rle_encode(f) for f in packed_frames]
    offsets = [0]
    for e in encoded:
        offsets.append(offsets[-1] + len(e))
    flat = b"".join(encoded)
    return encoded, offsets, flat


def write_bin(packed_frames, out_path):
    """Write a flat binary data for uploading to PY25Q16 external flash."""
    encoded, offsets, flat = build_rle_index(packed_frames)
    frame_count = len(packed_frames)
    header_size = 12 + (frame_count + 1) * 4

    buf = bytearray()
    buf.extend(struct.pack("<III", BAD_APPLE_MAGIC, BAD_APPLE_VERSION, frame_count))
    for off in offsets:
        buf.extend(struct.pack("<I", off + header_size))
    buf.extend(flat)

    out_dir = os.path.dirname(out_path)
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)

    with open(out_path, "wb") as f:
        f.write(buf)

    print(
        f"Wrote {len(buf)} bytes to {out_path} "
        f"(header: {header_size}, data: {len(flat)}, frames: {frame_count})"
    )


def main():
    parser = argparse.ArgumentParser(
        description="Convert a directory of 128x64 1-bit PNGs to ST7565 binary."
    )
    parser.add_argument("input_dir", help="Directory containing frame_NNNN.png files")
    parser.add_argument("output_file", help="Output binary file path")
    args = parser.parse_args()

    if not os.path.isdir(args.input_dir):
        print(f"Error: '{args.input_dir}' is not a directory", file=sys.stderr)
        sys.exit(1)

    # Find and sort frames by numeric index
    paths = []
    for fname in os.listdir(args.input_dir):
        match = re.match(r"frame_(\d+)\.png$", fname)
        if match:
            paths.append((int(match.group(1)), os.path.join(args.input_dir, fname)))

    if not paths:
        print(
            f"Error: No frame_NNNN.png files found in '{args.input_dir}'",
            file=sys.stderr,
        )
        sys.exit(1)

    paths.sort(key=lambda x: x[0])

    print(f"Found {len(paths)} frames. Packing...")
    packed = []
    for idx, path in paths:
        try:
            img = Image.open(path)
            packed.append(pack_image(img))
        except Exception as e:
            print(f"Error processing {path}: {e}", file=sys.stderr)
            sys.exit(1)

    write_bin(packed, args.output_file)


if __name__ == "__main__":
    main()
