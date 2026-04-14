#!/usr/bin/env bash

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FUSE_BINARY="$SCRIPT_DIR/mini_unionfs"

# On WSL, mounting on /mnt/c (drvfs) is usually forbidden for FUSE.
# Default to a native Linux path unless user overrides UNIONFS_TEST_DIR.
if [ -n "${UNIONFS_TEST_DIR:-}" ]; then
    TEST_DIR="$UNIONFS_TEST_DIR"
elif [[ "$SCRIPT_DIR" == /mnt/* ]]; then
    TEST_DIR="$HOME/unionfs_test_env"
else
    TEST_DIR="$SCRIPT_DIR/unionfs_test_env"
fi

LOWER_DIR="$TEST_DIR/lower"
UPPER_DIR="$TEST_DIR/upper"
MOUNT_DIR="$TEST_DIR/mnt"

GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m'

cleanup() {
    if mountpoint -q "$MOUNT_DIR" 2>/dev/null; then
        fusermount3 -u "$MOUNT_DIR" 2>/dev/null || \
        fusermount -u "$MOUNT_DIR" 2>/dev/null || \
        umount "$MOUNT_DIR" 2>/dev/null
    fi
    rm -rf "$TEST_DIR"
}

pass() {
    printf "${GREEN}PASSED${NC}\n"
}

fail() {
    printf "${RED}FAILED${NC}\n"
}

trap cleanup EXIT

echo "Starting Mini-UnionFS test suite..."
echo "Using test directory: $TEST_DIR"

if [ ! -x "$FUSE_BINARY" ]; then
    echo "Missing executable $FUSE_BINARY. Run 'make' first."
    exit 1
fi

cleanup
mkdir -p "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR"

echo "base_only_content" > "$LOWER_DIR/base.txt"
echo "to_be_deleted" > "$LOWER_DIR/delete_me.txt"
mkdir -p "$LOWER_DIR/lower_empty_dir"

"$FUSE_BINARY" "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR"
sleep 1

if ! mountpoint -q "$MOUNT_DIR" 2>/dev/null; then
    echo "Failed to mount Mini-UnionFS at $MOUNT_DIR"
    echo "Tip: avoid /mnt/c mountpoints; use a native Linux path in WSL."
    exit 1
fi

printf "Test 1: Layer visibility... "
if grep -q "base_only_content" "$MOUNT_DIR/base.txt" 2>/dev/null; then
    pass
else
    fail
fi

printf "Test 2: Copy-on-Write... "
echo "modified_content" >> "$MOUNT_DIR/base.txt" 2>/dev/null
if [ "$(grep -c "modified_content" "$MOUNT_DIR/base.txt" 2>/dev/null)" -eq 1 ] &&
   [ "$(grep -c "modified_content" "$UPPER_DIR/base.txt" 2>/dev/null)" -eq 1 ] &&
   [ "$(grep -c "modified_content" "$LOWER_DIR/base.txt" 2>/dev/null)" -eq 0 ]; then
    pass
else
    fail
fi

printf "Test 3: Whiteout on unlink... "
rm "$MOUNT_DIR/delete_me.txt" 2>/dev/null
if [ ! -f "$MOUNT_DIR/delete_me.txt" ] &&
   [ -f "$LOWER_DIR/delete_me.txt" ] &&
   [ -f "$UPPER_DIR/.wh.delete_me.txt" ]; then
    pass
else
    fail
fi

printf "Test 4: Create new upper file... "
echo "fresh_file" > "$MOUNT_DIR/created.txt" 2>/dev/null
if [ -f "$UPPER_DIR/created.txt" ] && grep -q "fresh_file" "$UPPER_DIR/created.txt" 2>/dev/null; then
    pass
else
    fail
fi

printf "Test 5: mkdir in upper layer... "
mkdir "$MOUNT_DIR/new_dir" 2>/dev/null
if [ -d "$UPPER_DIR/new_dir" ] && [ -d "$MOUNT_DIR/new_dir" ]; then
    pass
else
    fail
fi

printf "Test 6: rmdir lower-only directory creates whiteout... "
rmdir "$MOUNT_DIR/lower_empty_dir" 2>/dev/null
if [ ! -d "$MOUNT_DIR/lower_empty_dir" ] &&
   [ -d "$LOWER_DIR/lower_empty_dir" ] &&
   [ -f "$UPPER_DIR/.wh.lower_empty_dir" ]; then
    pass
else
    fail
fi

echo "Mini-UnionFS test suite completed."
