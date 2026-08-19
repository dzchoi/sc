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

`Panel` never mutates `term.line`. `st.c` renders terminal content first, then the
panel's cached `Canvas` overlays its covered rows. Hiding the panel therefore restores
the terminal by marking the rows it covered dirty and letting the ordinary terminal
renderer repaint them.

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

`panel_poll()` observes pending shell state, marks covered terminal rows dirty for a
visibility transition, and snapshots `m_needs_draw` before
`drawregion()` clears those flags. `panel_draw()` consumes that snapshot and presents
only when required. It must remain after terminal drawing so terminal output cannot
overwrite the overlay during the same frame.

`run()` batches activity before falling through to `draw()`: it waits up to
`minlatency` after initial activity, forces a frame by `maxlatency` under sustained
PTY output, and otherwise blocks at idle. Consequently, polling is frame-driven, not
per-byte. Prompt-refresh deadlines are the exception: `panel_adjust_timeout()` folds a
pending resize deadline into `pselect()` so the loop wakes even while otherwise idle.

## State and control flow

`Shell::preinit()` is the pre-fork boundary for the control socket: it creates the
socket, exports its path as `SC_SOCKET`, and fails startup when socket setup fails. The
shell inherits that environment. Adapter readiness (`OSC 6770`) is a startup latch;
cwd changes (`OSC 6771`) are later state-change notifications. Do not merge them:
readiness permits initialization to finish, while a cwd notification requests a
directory reconciliation on a later frame.

`m_cwd` is always a usable, slash-terminated directory path. Acquiring it through
`getcwd()` or `/proc/<shell-pid>/cwd` fails SC on error rather than storing an empty
sentinel. This lets entry loading and selection rely on one directory invariant.

The terminal owns the cursor. `Panel::prompt_padding()` reads it on demand through
`tgetcursor()` instead of keeping a copied cursor row synchronized after every PTY
write. The panel needs only the row, but the terminal accessor returns both coordinates
for a symmetric C interface.

Panel key handling draws immediately only after a selection actually changes. Keys
that the panel consumes solely to send a ZLE event leave drawing to the shell's normal
output path. Resizing schedules one debounced prompt refresh; the deadline uses a
monotonic clock and refreshes the prompt after geometry settles.

## Lifecycle traps

- `term.dirty` is valid for deciding whether to re-present the overlay only before
  `drawregion()` clears it. Preserve the decision in panel state for `panel_draw()`.
- Marking rows dirty for a visibility change is not a replacement for checking dirty
  rows on every visible frame: terminal output may repaint beneath an already-visible
  overlay.
- A user toggle changes `m_hidden` before the subsequent draw calls `panel_poll()`.
  `poll()` compares `visible()` with `m_was_visible`, then records the new value. This
  captures the pre-toggle state for a partial redraw; otherwise hiding can
  leave stale overlay pixels. A full-terminal redraw avoids this trap but is more
  expensive.
- The panel's covered row range is the smallest correct invalidation for normal
  visibility changes. Full-terminal invalidation is reserved for terminal-wide changes.

## Decisions

- Panel owns overlay eligibility, row invalidation for its visibility changes, and the
  delayed prompt-refresh deadline. `st.c` owns terminal storage and rendering order;
  it supplies `term.dirty` at the frame boundary rather than exposing terminal globals.
- Keep C ABI functions thin. C++ state transitions happen on `g_shell` or `g_panel`
  according to their boundary; terminal callbacks needed by the panel (`draw`,
  `redraw`, `tgetcursor`) are declared by the terminal interface rather than mirrored
  in panel state.
- Expose only the selected entry name outside `Panel`. Zsh checks the path's current
  type immediately before acting instead of treating cached panel metadata as the
  execution authority.
- Before user commands invalidate ZLE and write to the terminal, they discard the old
  prompt's padding so ZLE's automatic redisplay re-expands the unpadded prompt. Command
  output may have moved the cursor past the old prompt; reusing its padding would push
  the next prompt down, while explicitly resetting it would clear command output before
  the new prompt is drawn.
- Treat the current directory as data, not a lifecycle flag. Adapter readiness and a
  pending cwd refresh have separate meanings and remain explicit state.
- Keep alternate-screen startup behavior in terminfo rather than coupling it to the
  panel or changing the upstream terminal parser. `smcup` enters mode 1049 and homes
  the alternate-screen cursor; `rmcup` lets the terminal restore the shell cursor.
  `make install` compiles `st.info` with `tic`, while `make` alone leaves the user's
  installed terminfo database unchanged.

## To-do

### Appearance
  - It has a ungly default application icon now.
  - `static unsigned int rows = 54` in config.h seems adjusting according to the terminal screen height. However, snap to the screen boundary without extra pixels.
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

### Support Double panel

### Support VFS using fuse-zip
  - Not only viewing a file in zip, we can execute a file directly from the zip.
