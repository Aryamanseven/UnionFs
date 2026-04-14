#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOWER_DIR="$SCRIPT_DIR/lower"
UPPER_DIR="$SCRIPT_DIR/upper"
MOUNT_DIR="$SCRIPT_DIR/mnt"

# Best effort unmount if an old demo mount exists.
if command -v mountpoint >/dev/null 2>&1 && mountpoint -q "$MOUNT_DIR" 2>/dev/null; then
    fusermount3 -u "$MOUNT_DIR" 2>/dev/null || \
    fusermount -u "$MOUNT_DIR" 2>/dev/null || \
    umount "$MOUNT_DIR" 2>/dev/null || true
fi

rm -rf "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR"
mkdir -p "$LOWER_DIR/lower_empty_dir" "$UPPER_DIR" "$MOUNT_DIR"

echo "base_only_content" > "$LOWER_DIR/base.txt"
echo "to_be_deleted" > "$LOWER_DIR/delete_me.txt"

# Keep empty folders visible in Explorer/Git.
touch "$UPPER_DIR/.gitkeep"
touch "$MOUNT_DIR/.gitkeep"

echo "Demo folders are ready:"
echo "  lower: $LOWER_DIR"
echo "  upper: $UPPER_DIR"
echo "  mnt:   $MOUNT_DIR"
