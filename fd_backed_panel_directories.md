# FD-backed panel directories

SC identifies each panel directory by an open descriptor rather than by pathname text.
This lets an inactive panel retain an unlinked Linux directory and later make that inode
the shell working directory. The pathname remains useful for presentation, selection
restoration, and shell-facing paths, but it is not the authority for directory identity.

## State and ownership

`PanelDirectory` is the move-only owner of two related values:

- an `O_PATH | O_DIRECTORY | O_CLOEXEC` descriptor that pins the directory inode;
- a slash-terminated display cwd that retains Zsh's logical `$PWD` (without
  canonicalizing symlinks) while that pathname resolves to the pinned inode, then falls
  back to the pathname procfs reports for the descriptor.

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

At every preprompt boundary, Zsh sends its logical `$PWD` and `Shell::capture_cwd()`
opens `/proc/<shell-pid>/cwd`. `PanelDirectory` compares that pathname's inode with the
new descriptor and retains it only on a match. Otherwise it derives the display cwd
from `/proc/<sc-pid>/fd/<new-fd>`:

```text
Zsh $PWD ───── stat() ───────────────┐
                                     ├─ same inode ─► logical display cwd
/proc/<shell-pid>/cwd ── open() ─► retained descriptor
                                     └─ mismatch ───► procfs-reported display cwd
```

The descriptor remains authoritative. Validation prevents a shell-directory change or
pathname replacement from pairing one directory's handle with another directory's
name. Linux appends ` (deleted)` to the procfs fallback after the directory is unlinked,
so the panel title reflects the invalidated pathname without sacrificing inode access.

The focused panel adopts this newly captured handle during preprompt. The inactive
panel keeps its existing handle, logical name, and snapshot until it is focused again.
Panel switching uses that logical name while it still resolves to the retained inode;
otherwise it enters the descriptor through procfs with physical `cd` semantics.

## Reloading entries

`Panel::load_entries()` never reopens the display cwd. It opens `.` relative to the
retained descriptor, converts that scanning descriptor to a `DIR*`, and enumerates it
with `readdir()`. Entry metadata is read with descriptor-relative `fstatat()` and
`AT_SYMLINK_NOFOLLOW`, so a renamed or replaced pathname cannot redirect a reload to a
different directory. Symlinks receive one additional descriptor-relative lookup that
follows the link, distinguishing directory targets from other targets and unresolved
links without changing the metadata recorded for the link itself.

Synthetic `..` is inserted before scanning and remains even when the directory cannot
be scanned or has been unlinked and emptied. A failed scan retains its error and marks
the title as `(unreadable)` when opening is denied, `(unavailable)` when opening
otherwise fails, or `(incomplete)` when `readdir()` fails. The scan marker precedes the
title's trailing slash except for root, whose leading slash remains first. An unlinked
directory can independently report both `(deleted)` and its listing state. Metadata
lookup failure retains the name with a file type, zero size, and unknown modification
time, matching the panel's role as a cached presentation; Zsh validates a selected live
entry before navigation.

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
  → _sc_switch_panel requests directory_for_shell
  → SC replies L<logical-cwd> while it names the inode, otherwise P<procfs-path>
  → Zsh runs logical cd for L or physical cd for P
  → preprompt captures the shell cwd and reloads the focused panel
```

The focused panel descriptor is valid by invariant, including after unlink, so panel
switching is not a conditional or transactional focus operation. SC does not silently
select an existing ancestor and does not roll focus back.

`directory_for_shell` exposes the procfs path only as a fallback control-protocol
navigation target. A valid logical cwd keeps the panel and Zsh path spelling aligned.
When that cwd no longer names the inode, `_sc_switch_panel` uses physical `cd` so Zsh
resolves the procfs magic link before establishing its cwd. Consequently:

- logical symlink aliases survive panel switches while they remain valid;
- a fallback `/proc/<sc-pid>/fd/...` does not become the shell's logical directory;
- the shell enters the pinned directory inode even when its pathname is gone;
- relative `cd ..` follows that inode's real parent instead of `/proc/<sc-pid>/fd`.

Only the invalid-logical-path fallback requests physical resolution. Ordinary
`_sc_cd` calls, valid panel switches, and user-entered `cd` retain their normal logical
symlink behavior; SC does not override the global `cd` function.

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
