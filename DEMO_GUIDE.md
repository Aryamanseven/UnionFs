# Mini-UnionFS Demo Guide (Step-by-Step)

This guide is designed for live presentation.
Run one command at a time and check the expected result before moving on.

## 1. What You Need Before Starting

- OS/runtime: WSL (Ubuntu or similar)
- You should run commands from project root:
  - `/mnt/c/Users/Aryaman/Desktop/work/CC/unionfs`
- You will use 2 terminals:
  - Terminal 1: runs the filesystem process
  - Terminal 2: runs demo actions

## 2. Folders You Should Keep Open in VS Code Explorer

- `demo/lower`
- `demo/upper`
- `demo/mnt`

What these mean:
- `demo/lower`: read-only base image layer
- `demo/upper`: writable container layer (where CoW and whiteouts appear)
- `demo/mnt`: local project folder (not the active mountpoint in this WSL setup)

Important mountpoint note:
- In this project, `make demo-run` mounts to `$HOME/mini_unionfs_mnt`.
- This avoids WSL errors when mounting directly on `/mnt/c/...`.

## 3. Hard Cleanup (Deletes Demo Files/Folders Content)

Use this when you want a truly empty demo state before starting.

Step 0
Command:

```bash
make demo-hard-clean
```

What it does:
- Unmounts active demo mount if present.
- Deletes compiled `mini_unionfs` binary.
- Deletes `unionfs_test_env` leftovers.
- Deletes `demo/lower`, `demo/upper`, `demo/mnt` and recreates them as empty placeholders.
- Deletes the WSL mountpoint directory (`$HOME/mini_unionfs_mnt`) if it exists.

Where to look:
- `demo/lower`
- `demo/upper`
- `demo/mnt`

Expected:
- These folders exist but contain only `.gitkeep`.
- No `base.txt`, `delete_me.txt`, `created.txt`, `.wh.*`, etc.

Then continue with normal demo setup below.

## 4. Clean Start (Terminal 1)

Step 1
Command:

```bash
cd /mnt/c/Users/Aryaman/Desktop/work/CC/unionfs
```

What it does:
- Moves into the project root.

Expected:
- No error output.

Step 2
Command:

```bash
make demo-unmount
```

What it does:
- Unmounts old demo mount if one is still active.

Expected:
- Usually silent.

Step 3
Command:

```bash
make clean
```

What it does:
- Removes old compiled binary for a fresh build.

Expected:
- Output like `rm -f mini_unionfs`.

Step 4
Command:

```bash
make demo-setup
```

What it does:
- Resets demo data and recreates clean `lower`, `upper`, and `mnt` folders.

Where to look:
- `demo/lower`
- `demo/upper`
- `demo/mnt`

Expected:
- Message `Demo folders are ready:` with paths.

Step 5
Command:

```bash
make demo-run
```

What it does:
- Builds `mini_unionfs` and starts the FUSE filesystem in foreground.
- Mountpoint used: `$HOME/mini_unionfs_mnt`

Expected:
- Terminal stays busy (no prompt return).
- You see `Mountpoint: /home/<user>/mini_unionfs_mnt`.

## 5. Verify Mount and Show Features (Terminal 2)

Step 6
Command:

```bash
cd /mnt/c/Users/Aryaman/Desktop/work/CC/unionfs
```

What it does:
- Sets terminal to project root.

Expected:
- No error output.

Step 7
Command:

```bash
mountpoint -q ~/mini_unionfs_mnt && echo MOUNTED || echo NOT_MOUNTED
```

What it does:
- Confirms whether FUSE mount is active.

Expected:
- `MOUNTED`

Step 8
Command:

```bash
cat ~/mini_unionfs_mnt/base.txt
```

What it does:
- Reads merged-view file from mountpoint.

What it demonstrates:
- Lower-layer file visibility through union mount.

Expected:
- `base_only_content`

Step 9
Command:

```bash
echo "modified_content" >> ~/mini_unionfs_mnt/base.txt
```

What it does:
- Appends to lower-origin file through mount.

What it demonstrates:
- Copy-on-Write (CoW): file is copied to upper, then modified there.

Expected:
- No error output.

Step 10
Command:

```bash
cat demo/lower/base.txt
```

What it demonstrates:
- Lower layer remains unchanged after CoW.

Expected:
- `base_only_content`

Step 11
Command:

```bash
cat demo/upper/base.txt
```

What it demonstrates:
- Upper layer has copied + modified file.

Expected:
- Two lines:
  - `base_only_content`
  - `modified_content`

Step 12
Command:

```bash
rm ~/mini_unionfs_mnt/delete_me.txt
```

What it does:
- Deletes lower-origin file from merged view.

What it demonstrates:
- Whiteout behavior.

Expected:
- No error output.

Step 13
Command:

```bash
ls -la demo/upper | grep .wh.delete_me.txt
```

What it demonstrates:
- Whiteout marker created in upper.

Expected:
- One line containing `.wh.delete_me.txt`.

Step 14
Command:

```bash
echo "fresh_file" > ~/mini_unionfs_mnt/created.txt
```

What it does:
- Creates a new file through the mount.

What it demonstrates:
- New files go directly to upper layer.

Expected:
- No error output.

Step 15
Command:

```bash
cat demo/upper/created.txt
```

Expected:
- `fresh_file`

Step 16
Command:

```bash
mkdir ~/mini_unionfs_mnt/new_dir
```

What it does:
- Creates directory through union mount.

What it demonstrates:
- New directories are created in upper.

Expected:
- No error output.

Step 17
Command:

```bash
ls -ld demo/upper/new_dir
```

Expected:
- One line showing `demo/upper/new_dir` exists.

Step 18
Command:

```bash
rmdir ~/mini_unionfs_mnt/lower_empty_dir
```

What it does:
- Removes lower-only empty dir from merged view.

What it demonstrates:
- Directory whiteout behavior.

Expected:
- No error output.

Step 19
Command:

```bash
ls -la demo/upper | grep .wh.lower_empty_dir
```

Expected:
- One line containing `.wh.lower_empty_dir`.

## 6. Stop Demo (Terminal 2)

Step 20
Command:

```bash
make demo-unmount
```

What it does:
- Unmounts the union filesystem.

Expected:
- Usually silent.

Step 21
Command:

```bash
mountpoint -q ~/mini_unionfs_mnt && echo MOUNTED || echo NOT_MOUNTED
```

Expected:
- `NOT_MOUNTED`

## 7. Quick Troubleshooting

Problem:
`fusermount3: mounting over filesystem type ... is forbidden`

Reason:
- Mounting onto Windows-backed `/mnt/c/...` path.

Fix:
- Use `make demo-run` (already configured to mount on `$HOME/mini_unionfs_mnt`).

Problem:
`NOT_MOUNTED` after running `make demo-run`

Fix:
- Check Terminal 1 for crash/error lines.
- Ensure Terminal 1 is still running in foreground.

Problem:
`rmdir ... lower_empty_dir` fails with not empty

Fix:
- Run `make demo-setup` again before demo to reset clean state.

## 8. 30-Second Viva Summary

- "Lower is base image, upper is writable container layer."
- "Read path shows merged view with upper precedence."
- "Write to lower-origin file triggers CoW into upper."
- "Delete lower-origin file creates whiteout in upper."
- "New files/dirs are created in upper."
