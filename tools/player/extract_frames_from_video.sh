#!/bin/bash

INPUT_VIDEO="bad_apple.mp4"
OUTPUT_DIR="final_frames"
TARGET_WIDTH=128
TARGET_HEIGHT=64
FPS=24

rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

ffmpeg -i "$INPUT_VIDEO" \
  -vf "fps=${FPS},scale=${TARGET_WIDTH}:${TARGET_HEIGHT}:flags=neighbor,format=monob" \
  -y "$OUTPUT_DIR/frame_%04d.png"
