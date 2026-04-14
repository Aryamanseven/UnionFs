# 4-Person Work Split (Equal, File + Line Ownership)

This split assigns clear ownership with explicit file ranges so each person can explain exactly what they implemented.

## Member 1 (25%): Path Resolution and Metadata

Primary file ownership:
- `mini_unionfs.c` lines 154-488

Owned implementation blocks:
- Path helper construction and parsing
- Whiteout/opaque path helpers and checks
- Central path lookup and resolve order

Owned function region examples:
- `build_host_path` to `resolve_path`

## Member 2 (25%): Read Path and Directory Merge

Primary file ownership:
- `mini_unionfs.c` lines 489-549
- `mini_unionfs.c` lines 823-1085

Owned implementation blocks:
- Name list management for merged views
- Directory merge logic (`upper` precedence + whiteout filtering)
- Read-only FUSE handlers

Owned function region examples:
- `name_list_destroy` / `name_list_add_unique`
- `collect_merged_dir_entries`
- `unionfs_getattr`, `unionfs_statfs`, `unionfs_readdir`, `unionfs_read`

## Member 3 (25%): Copy-on-Write and Write Path

Primary file ownership:
- `mini_unionfs.c` lines 550-625
- `mini_unionfs.c` lines 730-1277

Owned implementation blocks:
- CoW trigger rules and parent materialization
- File copy-up logic (content, mode, timestamps)
- Write-path FUSE handlers

Owned function region examples:
- `open_requires_copy_up`, `ensure_upper_parents`, `copy_up_file`
- `unionfs_open`, `unionfs_create`, `unionfs_write`, `unionfs_truncate`

## Member 4 (25%): Delete Semantics, Build/Test, Docs, Demo

Primary file ownership:
- `mini_unionfs.c` lines 626-729
- `mini_unionfs.c` lines 1292-1561
- `Makefile` lines 1-46
- `test_unionfs.sh` lines 1-122
- `DESIGN.md` lines 1-205
- `DEMO_GUIDE.md` lines 1-383
- `demo/reset_demo.sh` lines 1-30

Owned implementation blocks:
- Whiteout create/remove helpers used by delete flows
- Deletion and directory removal semantics
- Build/test automation and WSL-safe test/mount workflow
- Project documentation and demo instructions

Owned function region examples:
- `remove_whiteout`, `create_whiteout`
- `unionfs_unlink`, `unionfs_mkdir`, `unionfs_rmdir`, `main`

## Viva Speaking Order

- Member 1: Path resolution order and metadata hiding strategy.
- Member 2: Merged read view and readdir behavior.
- Member 3: CoW write path and why lower stays untouched.
- Member 4: Delete/whiteout behavior, build/test setup, and demo workflow.
