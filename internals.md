# SC internals

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

## Architecture map

```text
X11 events and PTY output
          │
          ▼
       st.c / x.c ───── terminal grid, cursor, and frame scheduling
          │
          ▼
         Comm ───────── focus, visibility, layout, and subsystem coordination
        /    \
       ▼      ▼
    Panel    Shell ════ control socket and fixed ZLE events ════ sc.zsh
  directory                                                   cwd, buffer,
  and canvas                                               commands, and prompt
```

The detailed technical documents each own one subsystem or lifecycle:

- [Automatic zsh adapter loading](automatic_adapter_loading.md): startup injection,
  readiness, runtime assets, and cleanup.
- [SC zsh shell integration](shell_integration.md): runtime IPC, ZLE widgets, command
  boundaries, and prompt synchronization.
- [FD-backed panel directories](fd_backed_panel_directories.md): directory identity,
  descriptor ownership, reloads, panel switching, and deleted-directory recovery.
- [Window resizing](window_resizing.md): X11 geometry, terminal cells, unused pixels,
  and PTY size propagation.

This document retains the cross-subsystem architecture, rendering invariants, and
decisions that do not belong to one of those topics.

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

## Cross-subsystem control flow

The first successful `preprompt` request is the initialization boundary: the Zsh
adapter is ready, both panels own valid directory descriptors, and both snapshots are
nonempty. Normal panel polling and input handling begin only afterward.

At runtime, `Comm` routes global keys before `Panel` sees pane-local selection keys.
Shell-dependent actions cross the PTY as fixed ZLE events; Zsh then uses the private
control socket for data and acknowledgements. A successful `preprompt` reconciles the
focused panel and prompt padding synchronously, which prevents ordering dependencies
between the PTY and socket channels.

Ctrl+O is handled before effective visibility so it can restore hidden panels. `x.c`
removes lock modifiers before dispatch, preserving shortcuts under Caps Lock and Num
Lock. `Panel` redraws immediately for selection input only when the selection changes;
layout, focus, and visibility changes remain coordinator operations.

`Comm` owns focus, layout, forced visibility, and the resize-refresh deadline. `Panel`
owns its descriptor-backed directory state, entries, selection, geometry, and cached
canvas. `Shell` owns the managed-shell IPC boundary. Zsh remains authoritative for its
cwd, editable buffer, command execution, and prompt.

## Rendering lifecycle traps

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

- Keep overlay coordination in `Comm` so `Panel` remains pane-local and `st.c` can
  expose frame-boundary state without exposing terminal storage.
- Keep C ABI functions thin. They delegate global lifecycle transitions to static
  `Comm`; terminal callbacks needed by the coordinator (`draw`, `tgetcursor`) are
  declared by the terminal interface rather than mirrored in panel state.
- Keep frame polling responsible only for presentation. Shell reconciliation and prompt
  placement remain synchronous command-boundary work.
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
  - Restore previous panel view as well as m_selected_idx.
  - Title adds a space at the front and end of it like NC does.
  - Inactive panel's title should not be highlighted like NC does.
  - Use '~' instead of '/home/stem' in the title.
  - Appended type markers (e.g., /, @) to entries be displayed in dark text.
  - It has an ugly default application icon now.
  - Handle Symlinks and consider permissions.
  - Hangul typing shows the unnecessary combination box.
  - `Panel::dirty_` for each panel row instead of the entire panel

### Mouse
  - Mouse selects text into the panel.
  - Scroll back with mouse wheel.

### Keys
  - Shift+Tab: insert the other panel's selected name (if visible) or cwd (if hidden) into the command line.
  - Ctrl+U: swap panels.
  - F5: fast copy using `rsync`?
  - ??: diff directories using `rsync -avun`, ...
  - `history` selection menu (F1?).
  - Global key binding in sc_config.hpp and user key/action bindings in sc.zsh.
  - Show memory usage, ...

### Brief/detailed panel view

### Group selection
  - Ctrl+Enter inserts all group entries in command line.

### Support VFS using fuse-zip
  - Not only viewing a file in zip, we can execute a file directly from the zip.

### Misc.
  - Clean up socket when killed by Ctrl+C.
