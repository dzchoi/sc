# Panel internals

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

`panel_poll()` synchronizes PTY ownership and pending shell state, marks covered
terminal rows dirty for a visibility transition, and snapshots `m_needs_draw` before
`drawregion()` clears those flags. `panel_draw()` consumes that snapshot and presents
only when required. It must remain after terminal drawing so terminal output cannot
overwrite the overlay during the same frame.

`run()` batches activity before falling through to `draw()`: it waits up to
`minlatency` after initial activity, forces a frame by `maxlatency` under sustained
PTY output, and otherwise blocks at idle. Consequently, polling is frame-driven, not
per-byte. Prompt-refresh deadlines are the exception: `panel_adjust_timeout()` folds a
pending resize deadline into `pselect()` so the loop wakes even while otherwise idle.

## State and control flow

`Panel::preinit()` is the pre-fork boundary for the control socket: it creates the
socket, exports its path as `SC_SOCKET`, and fails startup if either step fails. The
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
  `poll()` therefore cannot infer the pre-toggle visibility from `visible()` alone.
  Carry that transition into `poll()` explicitly when using a partial redraw; otherwise
  hiding can leave stale overlay pixels. A full-terminal redraw avoids this trap but is
  more expensive.
- The panel's covered row range is the smallest correct invalidation for normal
  visibility changes. Full-terminal invalidation is reserved for terminal-wide changes.

## Decisions

- Panel owns overlay eligibility, row invalidation for its visibility changes, and the
  delayed prompt-refresh deadline. `st.c` owns terminal storage and rendering order;
  it supplies `term.dirty` at the frame boundary rather than exposing terminal globals.
- Keep C ABI functions thin. C++ state transitions happen on `g_panel`; terminal
  callbacks needed by the panel (`draw`, `redraw`, `tgetcursor`) are declared by the
  terminal interface rather than mirrored in panel state.
- Treat the current directory as data, not a lifecycle flag. Adapter readiness and a
  pending cwd refresh have separate meanings and remain explicit state.

## To-do
* The legacy tty* function names in st.c are upstream API names and can remain. But new comments, fields, and docs should say “PTY” for transport/process-group details and “terminal” only for emulator/UI concepts. For example,
  ```
  // Show the overlay only while the shell owns the PTY's foreground process group.
  shell_owns_tty_ = (::tcgetpgrp(pty_fd_) == shell_pid_);
  ```

* View and Edit
  ```
  _sc_run_selected() {
      local action=$1
      _sc_get_selected_entry || return

      local -a command
      case $action in
          view)
              [[ $_sc_selected_entry_kind == F ]] || return
              command=("${PAGER:-less}")
              ;;
          edit)
              [[ $_sc_selected_entry_kind == F ]] || return
              command=("${EDITOR:-vi}")
              ;;
          *)
              return 1
              ;;
      esac

      zle -I
      command "${command[@]}" -- "$PWD/$REPLY"
      zle reset-prompt
  }

  _sc_view() {
      _sc_run_selected view
  }

  _sc_edit() {
      _sc_run_selected edit
  }
  ```

### Appearance
  - `less` with a file that is less than the screen height does not show the file content aligned at the top. It look to align at the bottom.
  - Application icon
  - `static unsigned int rows = 54` in config.h seems adjusting according to the terminal screen height. However, snap to the screen boundary without extra pixels.
  - Use '~' instead of '/home/stem' in the title.
  - Handle Symlinks and consider permissions
  - `history` selection menu.
  - Hangul typing shows a combination box.
  - `Panel::dirty_` for each panel row instead of the entire panel

### Mouse
  - Mouse selects text within the panel.
  - Scroll back with mouse wheel.

### Keys
  - F3: (colored) `less $0`
  - F4: `vim $0` (not gvim)
  - F5: fast copy using rsync?
  - diff directories, ...
  - Show memory usage, ...

### Brief list panel

### Double panel

### Support VFS using fuse-zip
  - Not only viewing a file in zip, we can execute a file directly from the zip.
