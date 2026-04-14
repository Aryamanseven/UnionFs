# Mini-UnionFS Design Document

## 1. Overview

Mini-UnionFS exposes a single mounted directory that merges two backing directories:

- `lower_dir`: read-only image layer
- `upper_dir`: read-write container layer

The mounted filesystem behaves like a small container overlay:

- Reads prefer the `upper_dir` copy when it exists.
- Reads fall back to `lower_dir` when no upper copy exists.
- Writes to lower-only files trigger copy-on-write (CoW), creating the writable copy in `upper_dir`.
- Deleting a lower-layer entry does not remove the lower data. Instead, Mini-UnionFS creates a whiteout file in `upper_dir` to hide that lower entry.

The implementation uses the high-level FUSE 3 API in C. The hardest part is path resolution: every operation must decide whether the visible object comes from the upper layer, the lower layer, or is hidden by metadata.

## 2. Main Data Structures

### 2.1 Global state

`struct mini_unionfs_state` stores the canonical absolute paths for:

- `lower_dir`
- `upper_dir`

This state is passed to `fuse_main(..., private_data)` and later accessed from callbacks through `fuse_get_context()->private_data`.

### 2.2 Path lookup object

`struct path_lookup` stores the host-side paths and cached metadata for one union path:

- `upper`: `upper_dir + path`
- `lower`: `lower_dir + path`
- `whiteout`: parent directory in `upper_dir` plus `/.wh.<name>`
- booleans tracking whether upper, lower, or whiteout entries physically exist
- cached `struct stat` values for upper and lower entries

This structure is intentionally simple. Visibility is decided by helper functions, not by the struct alone.

### 2.3 Name lists for merged `readdir`

`struct name_list` is a small dynamic array of strings used during directory merging:

- one list stores visible names already emitted
- one list stores names hidden by whiteout files

This is enough to merge upper and lower directory entries without pulling in a larger container library.

## 3. Path Resolution

### 3.1 Whiteouts

For a union path such as `/etc/config.txt`, the matching whiteout path is:

`upper_dir/etc/.wh.config.txt`

If that file exists, the lower copy must be hidden from the merged view.

Mini-UnionFS checks every component of a path while walking downward. That matters because a whiteout on a parent directory must also hide its descendants. Example:

- lower has `/app/data/file.txt`
- upper has `/app/.wh.data`

Then both `/app/data` and `/app/data/file.txt` are treated as absent.

### 3.2 Opaque directories

The assignment only requires whiteouts, but recreating a previously deleted lower directory is cleaner if the new upper directory can suppress lower children. For that reason the implementation also supports the conventional opaque marker:

`upper_dir/<dir>/.wh..wh..opq`

If an opaque marker exists on the current directory or one of its ancestors, lower entries below that point are ignored. This is mainly used when the user:

1. deletes a lower directory, creating `.wh.<dirname>` in its parent
2. later creates a new directory with the same name

Without opacity, the recreated directory would merge with the old lower contents.

### 3.3 `resolve_path`

`resolve_path(path, ...)` is the central helper used by most callbacks. Its logic is:

1. Build host paths for upper, lower, and whiteout metadata.
2. If any whiteout hides the union path, return `-ENOENT`.
3. If the upper copy exists, return the upper path.
4. Otherwise, if the lower copy exists and no opaque ancestor blocks it, return the lower path.
5. Otherwise return `-ENOENT`.

This directly matches the required “upper wins, lower is fallback, whiteout hides lower” behavior.

## 4. Operation Design

### 4.1 `getattr`

`getattr` resolves the visible host path and then performs `lstat` on that backing entry. This keeps file metadata aligned with the branch that is currently visible through the union mount.

### 4.2 `readdir`

`readdir` is implemented as a two-pass merge:

1. Scan the upper directory first.
2. Skip metadata entries such as `.wh.*` and `.wh..wh..opq`.
3. Record visible upper names immediately.
4. Record lower names hidden by whiteouts.
5. Scan the lower directory next.
6. Emit only names that were not already seen and are not whiteouted.

If the directory is opaque, the lower scan is skipped entirely.

### 4.3 `open`, `write`, and `truncate`

Copy-on-write is triggered before any modifying access:

- If the visible file is already in `upper_dir`, open it directly.
- If the visible file is lower-only and the open mode is writable, copy it to `upper_dir` first.
- `truncate` follows the same rule because truncation is also a modification.

The copy-up routine:

1. resolves the lower file
2. creates missing upper parent directories
3. creates the new upper file with the lower file’s mode
4. copies bytes
5. preserves timestamps with `futimens`

The lower file is never modified.

### 4.4 `create`

For a brand-new file:

1. return `-EEXIST` if the union path already exists visibly
2. ensure parent directories exist in the upper layer
3. create the upper file directly
4. remove any stale target whiteout

The whiteout is removed only after successful file creation so a lower file does not accidentally reappear if creation fails.

### 4.5 `unlink`

Deletion has three cases:

- visible upper-only file: unlink the real upper file
- visible lower-only file: create an upper whiteout
- visible upper file that shadows a lower file: unlink the upper file, then create a whiteout so the lower copy does not reappear

This mirrors standard overlay delete behavior.

### 4.6 `mkdir` and `rmdir`

`mkdir` always creates in `upper_dir`. If the name was previously whiteouted and a lower directory still exists underneath, the implementation also creates an opaque marker inside the new upper directory before removing the whiteout. That preserves the expected “fresh empty directory” behavior.

`rmdir` first checks emptiness in the merged namespace, not just in the physical upper directory. If the visible directory is empty:

- remove upper-only metadata files left in that directory
- remove the upper directory itself if it exists
- create a whiteout if a lower directory must remain hidden

This lets a user remove a lower directory that has become logically empty even if the upper layer still contains metadata such as `.wh.child`.

## 5. Edge Cases and Invariants

### 5.1 Nested parent creation

When Mini-UnionFS needs to create an upper file or whiteout for a path inside lower-only directories, it first materializes the missing parent directories in `upper_dir`. Parent directories inherit permissions from the visible lower directory they mirror.

### 5.2 Type conflicts

If the visible object resolves to a file, `readdir` returns `-ENOTDIR`.
If the visible object resolves to a directory, `open` and `unlink` reject it with directory errors.

### 5.3 Reserved metadata namespace

Names beginning with `.wh.` are reserved for union metadata in the upper layer. That is acceptable for this project and is consistent with real overlay-style filesystems.

### 5.4 Non-atomic metadata transitions

Because this is a user-space educational filesystem, transitions such as “remove upper file, then create whiteout” are not fully atomic. The design prioritizes clarity and correctness of normal behavior over crash consistency.

## 6. Build and Validation

The deliverables are:

- `mini_unionfs.c`
- `Makefile`
- `test_unionfs.sh`
- `DESIGN.md`

The `Makefile` builds against `libfuse3` using `pkg-config`. The test script validates:

- lower-layer visibility
- copy-on-write
- whiteout unlink behavior
- creation in the upper layer
- `mkdir`
- `rmdir` whiteout behavior

WSL execution note:

- If the repository is located under `/mnt/c/...`, FUSE mounts on that path may be rejected by the kernel (`mounting over filesystem type ... is forbidden`).
- To keep tests reliable in WSL, `test_unionfs.sh` defaults to a native Linux test directory (for example `$HOME/unionfs_test_env`) when run from `/mnt/*`.

Together, these cover the core semantics required by the project specification.
