# Panel internals

## Repository branches

`upstream` names the read-only Suckless `st` remote. `upstream/master` is the
local record of its current tip. `main` is this repository's unmodified mirror of
that tip, and `origin/main` is the corresponding mirror published to GitHub. `sc`
contains the project changes and is based on `main`; `origin/sc` publishes that
customized branch. Several of these refs may name the same commit after an update.
They remain distinct because each records a different branch or remote boundary.

When Suckless updates `upstream/master`, update the mirror and then replay the
custom branch on it:

```
git fetch upstream
git switch main
git merge --ff-only upstream/master
git push origin main
git switch sc
git rebase main
git push --force-with-lease origin sc
```

The fast-forward-only merge ensures `main` remains an exact upstream mirror. The
rebase changes `sc` commit IDs, so its GitHub branch is updated with
`--force-with-lease`, which refuses to overwrite an unexpected remote change.

## Frame lifecycle

`Panel` never mutates `term.line`. `st.c` renders terminal content first, then `Comm`
presents each visible panel's cached `Canvas` region over its covered rows. The two
canvases have disjoint horizontal geometry and share one terminal-width backing buffer.
Hiding either region therefore restores the terminal by marking the shared covered
rows dirty and letting the ordinary terminal renderer repaint them.

The frame call chain is:

```
  run() [x.c - the main event loop]
    --> pselect() - blocks until PTY data arrives or X event (keypress, etc.)
    --> ttyread() - reads PTY bytes, parses them, updates term.line
    --> XNextEvent() - dispatches X events (key/mouse/resize/etc.)
    --> [if idle or maxlatency hit] draw() [st.c]
          --> panel_poll(term.dirty)  // before drawregion() clears row-dirty flags
          --> drawregion() and xdrawcursor()
          --> panel_draw()
```

`panel_poll()` asks both panels to update their visibility history, then marks covered
terminal rows dirty for either visibility transition. Only after both transitions are
collected does `Comm` snapshot whether visible content or covered terminal rows require
presentation. This ordering prevents one pane from invalidating a row after the other
has decided it does not need presentation. `panel_draw()` consumes the shared snapshot
and presents every visible region. It must remain after terminal drawing so terminal
output cannot overwrite either overlay during the same frame.

`run()` batches activity before falling through to `draw()`: it waits up to
`minlatency` after initial activity, forces a frame by `maxlatency` under sustained
PTY output, and otherwise blocks at idle. Consequently, polling is frame-driven, not
per-byte. Prompt-refresh deadlines are the exception: `panel_adjust_timeout()` folds a
pending resize deadline into `pselect()` so the loop wakes even while otherwise idle.

## State and control flow

`Shell::preinit()` is the pre-fork boundary for the control socket: it creates the
socket, exports its path as `SC_SOCKET`, and fails startup when socket setup fails. The
shell inherits that environment. `Shell::init()` services both PTY output and IPC until
the first valid `scctl preprompt` request establishes both panel snapshots. Normal panel
polling and input handling begin only after that request completes.

The first preprompt establishes `m_cwd` and `m_entries` together for both panels;
`Shell::m_preprompt_requested` makes `Comm::reload_panels()` initialize both panels
until the first preprompt completes. Beyond that boundary, each `m_cwd` is
slash-terminated and each `m_entries` is nonempty because `load_entries()` always
inserts synthetic `..`.
The focused panel follows the shell cwd while the inactive panel retains its own
directory. Tab changes focus in dual mode and sends a ZLE event that changes the shell
cwd to the newly focused panel before refreshing its snapshot.
`Shell::get_cwd()`
reads the shell directory through `/proc/<shell-pid>/cwd`. If that directory has been
unlinked, Linux returns a path marked `(deleted)` that cannot be reopened by name;
`load_entries()` retains the synthetic `..` recovery entry so the shell can leave it.
Names returned by `readdir()` remain in the snapshot when `lstat()` fails; their cached
type, size, and mtime stay at non-directory/zero defaults. Cached type controls only
presentation; Zsh validates the selected live path before directory navigation.

Reloading preserves selection by absolute entry path after sorting the new snapshot.
On a cwd change, the previous cwd is the candidate path to restore; ascending to its
direct parent therefore selects the directory just left, while descending keeps the
synthetic `..` index-zero default. On a same-cwd reload, the selected entry's absolute
path is restored when it remains. Reloading retains the viewport; `render()` moves it
only when needed to keep the restored selection visible.

`scctl preprompt <applied_padding>` is the synchronous prompt boundary. It reads the
shell cwd, reconciles the focused panel's cwd, reloads its entries even when the cwd is
unchanged, and finally computes prompt padding. `scctl reload` performs the same
focused-panel reconciliation without reading the terminal cursor. ZLE user commands
use it because their post-command cursor is an output boundary, not a prompt boundary.

The terminal owns the cursor. `Comm::adjust_padding()` reads it on demand through
`tgetcursor()` instead of keeping a copied cursor row
synchronized after every PTY write. The panel needs only the row, but the terminal
accessor returns both coordinates for a symmetric C interface.

`Panel` handles only keys that move its selection and draws immediately only after the
selection actually changes. `Comm` handles forced visibility, focus, layout, and
shell-event keys before delegating pane-local movement to the focused panel. Ctrl+O is
handled before effective visibility so it can restore hidden panels. Ctrl+P switches
between single and dual layout without changing focus; Tab changes focus only in dual
layout and dirties both cached selectors. `x.c` removes lock modifiers before this
dispatch, preserving shortcut behavior with Caps Lock and Num Lock. Resizing schedules
one coordinator-owned debounced prompt refresh; the deadline uses a monotonic clock and
refreshes the prompt after geometry settles.

## Lifecycle traps

- `term.dirty` is valid for deciding whether to re-present the overlays only before
  `drawregion()` clears it. Preserve the shared decision in `Comm` for `panel_draw()`.
- Terminal dirtiness is row-granular. Collect both visibility transitions before
  testing dirty rows, and re-present every visible pane when any covered row is dirty.
- Marking rows dirty for a visibility change is not a replacement for checking dirty
  rows on every visible frame: terminal output may repaint beneath an already-visible
  overlay.
- A user toggle changes `Comm::m_hidden` before the subsequent draw calls `panel_poll()`.
  `Comm::poll()` computes global visibility once and passes each panel its effective
  visibility. Each `Panel::set_visible()` compares that input with `m_visible`, then
  records the new value. This captures the pre-toggle state for a partial redraw;
  otherwise hiding can leave stale overlay pixels. A full-terminal redraw avoids this
  trap but is more expensive.
- The panels' shared covered row range is the smallest correct invalidation for normal
  visibility changes. Full-terminal invalidation is reserved for terminal-wide changes.

## Decisions

- `Comm` owns effective overlay eligibility, focus, single/dual and hidden state, shared
  row invalidation, shell integration, and the delayed prompt-refresh deadline. Each
  `Panel` owns only pane-local geometry, directory data, selection, and cached rendering
  state. `st.c` owns terminal storage and rendering order; it supplies `term.dirty` at
  the frame boundary rather than exposing terminal globals.
- Keep C ABI functions thin. They delegate global lifecycle transitions to static
  `Comm`; terminal callbacks needed by the coordinator (`draw`, `tgetcursor`) are
  declared by the terminal interface rather than mirrored in panel state.
- Expose only the focused panel's selected entry name outside `Comm`. Zsh checks the
  path's current type immediately before acting instead of treating cached panel
  metadata as the execution authority.
- `ALTERNATE` user commands retain the active prompt's padding through successful
  alternate-screen restoration and let ZLE reset that prompt in place. `NORMAL`
  commands discard the old padding before producing output, while failed `ALTERNATE`
  commands discard it afterward. Both modes reload panel data without consulting the
  output cursor and return command failures for ZLE's configured feedback.
- Treat each user command's display mode as a configuration contract for successful
  completion. Assign `ALTERNATE` only when success restores the old screen and `NORMAL`
  when output advances beyond the old prompt; misclassification gives ZLE the wrong
  prompt geometry. A failed `ALTERNATE` command is treated as normal diagnostic output
  because it may fail before entering the alternate screen. Consequently, a command
  that restores the old screen but returns nonzero remains an ambiguous edge case.
- Keep directory reconciliation and entry reload synchronous in both `scctl preprompt`
  and `scctl reload`. Only preprompt requests calculate padding, at a real prompt
  boundary; reload requests must not infer prompt placement from a command-output
  cursor. Frame polling remains responsible only for presentation state.
- Treat the first successful preprompt request as shell readiness. `Shell::init()` services
  startup PTY and IPC activity until that request completes and initializes both panel
  snapshots, so later panel methods can rely on cwd and entry invariants without
  readiness checks.
- Keep focus non-null and represent forced hiding independently. This preserves the
  active pane across Ctrl+O without a second saved focus pointer. Single/dual mode is
  likewise independent, so Ctrl+P changes layout without changing the active directory.
- Keep alternate-screen startup behavior in terminfo rather than coupling it to the
  panel or changing the upstream terminal parser. `smcup` enters mode 1049 and homes
  the alternate-screen cursor; `rmcup` lets the terminal restore the shell cursor.
  `make install` compiles `st.info` with `tic`, while `make` alone leaves the user's
  installed terminfo database unchanged.

## To-do

### Live directory updates
  - Consider an inotify watch for the current directory if live updates or reload cost
    justify it. Add the new watch before scanning after a cwd change, coalesce child
    create/delete/move/attribute/write events into a stale flag, and reload only when
    visible. Handle `IN_MOVE_SELF`, `IN_DELETE_SELF`, `IN_IGNORED`, and queue overflow;
    a prompt-driven reload remains the recovery boundary.

### Appearance
  - It has an ugly default application icon now.
  - Use '~' instead of '/home/stem' in the title.
  - Handle Symlinks and consider permissions.
  - `history` selection menu (F1?).
  - Hangul typing shows the unnecessary combination box.
  - `Panel::dirty_` for each panel row instead of the entire panel

### Mouse
  - Mouse selects text within the panel.
  - Scroll back with mouse wheel.

### Key customization
  - F5: fast copy using rsync?
  - ??: diff directories, ...
  - Show memory usage, ...

### Brief/detailed panel view

### Support VFS using fuse-zip
  - Not only viewing a file in zip, we can execute a file directly from the zip.

### Group selection
  - Ctrl+Enter inserts all entries in command line.

### Misc.
  - Clean up socket when killed by Ctrl+C.
  - Support Ctrl+U to swap the left and right panels.
  - Unfocused panel should also unhighlight the panel title.
  - Shift+Tab inserts the other panel's cwd into command line.

### _scctl() written in zsh
```
zmodload zsh/net/socket
zmodload zsh/system

_scctl() {
    [[ -n ${SC_SOCKET-} ]] || return 1
    local REPLY fd
    zsocket -- $SC_SOCKET || return 1  # $REPLY = client fd
    fd=$REPLY
    print -u $fd -r -- "$*" || { exec {fd}>&-; return 1; }
    local buf
    sysread -s 4096 buf <&fd  # up to 4096 bytes
    exec {fd}>&-
    printf %s $buf
}
```
