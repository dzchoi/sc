# FD-backed panel directories

SC identifies each panel directory by an open descriptor rather than by pathname text.
This lets an inactive panel retain an unlinked Linux directory and later make that inode
the shell working directory. The pathname remains useful for presentation, selection
restoration, and shell-facing paths, but it is not the authority for directory identity.

## State and ownership

`PanelDirectory` is the move-only owner of two related values:

- an `O_PATH | O_DIRECTORY | O_CLOEXEC` descriptor that pins the directory inode;
- a slash-terminated display cwd derived from that descriptor through procfs.

The descriptor is closed when the handle is replaced or destroyed. Moving transfers
ownership and invalidates the source; copying is prohibited. `Panel` is non-movable
because `Comm` assigns each instance a stable left/right identity and retains focus as
its address.

The first successful `preprompt` request establishes both panels. `Comm` duplicates the
initial descriptor with `F_DUPFD_CLOEXEC`, giving the panels independent ownership of
the same inode; failure to establish the second owner terminates SC. `Panel::init()`
then establishes each directory and its first snapshot. After this initialization
boundary, each panel always owns a valid descriptor and a nonempty entry snapshot
containing at least synthetic `..`; later preprompt requests use `Panel::reload()`.

The descriptor is the authoritative identity. The display cwd can change while the
identity remains stable, such as after a rename or unlink, and the same pathname can
later refer to a different inode.

## Capturing the shell directory

At every preprompt boundary, `Shell::capture_cwd()` opens
`/proc/<shell-pid>/cwd` before it reads a pathname. It then reads
`/proc/self/fd/<new-fd>` to derive the display cwd from the exact inode represented by
the new handle:

```text
/proc/<shell-pid>/cwd
        │ open(O_PATH | O_DIRECTORY | O_CLOEXEC)
        ▼
  retained descriptor ── readlink(/proc/self/fd/<fd>) ──► display cwd
```

Opening first prevents a shell-directory change or pathname replacement from pairing a
descriptor for one directory with the name of another. Linux appends ` (deleted)` to
the procfs link target after the directory is unlinked, so the panel title reflects the
invalidated pathname without sacrificing access to the inode.

The focused panel adopts this newly captured handle during preprompt. The inactive
panel keeps its existing handle and snapshot until it is focused again.

## Reloading entries

`Panel::load_entries()` never reopens the display cwd. It opens `.` relative to the
retained descriptor, converts that scanning descriptor to a `DIR*`, and enumerates it
with `readdir()`. Entry metadata is read with descriptor-relative `fstatat()` and
`AT_SYMLINK_NOFOLLOW`, so a renamed or replaced pathname cannot redirect a reload to a
different directory.

Synthetic `..` is inserted before scanning and remains even when the directory cannot
be scanned or has been unlinked and emptied. Metadata lookup failure retains the name
with non-directory/zero defaults, matching the panel's role as a cached presentation;
Zsh validates a selected live entry before navigation.

Before replacing a panel handle, `Panel::reload()` compares `st_dev` and `st_ino` for
the old and new descriptors:

- Different inode: treat the reload as directory navigation, reset selection to `..`,
  and use the previous display path as the candidate to select when ascending.
- Same inode: retain the selected entry name under the current display cwd, including
  when a rename or the procfs ` (deleted)` suffix changed the pathname text.

This comparison also detects deletion followed by recreation at the same pathname.
Path-string equality alone cannot distinguish those directories.

Reloading retains the viewport. After selection restoration, `render()` moves it only
as far as needed to keep the selected entry visible.

## Switching panels

Tab retains the existing SC-first control flow:

```text
Tab
  → Comm changes focus and sends the SwitchPanel ZLE event
  → _sc_switch_panel requests focused_directory
  → SC replies /proc/<sc-pid>/fd/<focused-panel-fd>
  → Zsh runs builtin cd -P -- <reply>
  → preprompt captures the shell cwd and reloads the focused panel
```

The focused panel descriptor is valid by invariant, including after unlink, so panel
switching is not a conditional or transactional focus operation. SC does not silently
select an existing ancestor and does not roll focus back.

`focused_directory` exposes the procfs path only as a control-protocol navigation
target. `_sc_switch_panel` uses physical `cd` so Zsh resolves the procfs magic link
before establishing its cwd. Consequently:

- `/proc/<sc-pid>/fd/...` does not become the shell's logical directory;
- the shell enters the pinned directory inode even when its pathname is gone;
- relative `cd ..` follows that inode's real parent instead of `/proc/<sc-pid>/fd`.

Only panel switching requests physical resolution. Ordinary `_sc_cd` calls and
user-entered `cd` retain the user's normal logical/physical symlink behavior; SC does
not override the global `cd` function.

## Unlinked-directory lifecycle

An inactive panel survives external recursive removal as follows:

1. The external command empties and unlinks the directory pathname.
2. The inactive panel descriptor keeps the now-unlinked inode alive.
3. Tab focuses that panel and Zsh enters the inode through its procfs descriptor path.
4. Preprompt captures the shell cwd; procfs supplies the old name with ` (deleted)`.
5. Reload finds no ordinary entries, but synthetic `..` remains.
6. `cd ..`, `cd ../..`, or `Ctrl+Page Up` walks the retained inode hierarchy until an
   existing ancestor is reached.

The same mechanism continues through multiple unlinked ancestors because relative
parent traversal is inode-based. SC deliberately does not choose an ancestor on the
user's behalf.

## Linux and prompt behavior

The design is Linux-specific. It depends on `O_PATH`, procfs cwd/fd links, and procfs
magic-link traversal across the SC and shell processes. Retained descriptors also keep
their filesystem mount referenced for the lifetime of the corresponding panel.

Physical entry prevents the procfs control path from appearing in `$PWD`. For an
unlinked cwd, Zsh can expose the old procfs-derived name with ` (deleted)` through
`$PWD`; prompt escapes that ask Zsh to reconstruct a physical cwd, such as `%~`, may
instead render `.` because the `::getcwd()` system call cannot name the unlinked
inode. SC does not rewrite the user's prompt to compensate for that Zsh behavior.
