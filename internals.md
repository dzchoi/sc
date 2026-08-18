### How `panel_poll()` is called

The call chain is:
```
  run() [x.c - the main event loop]
    --> pselect() - blocks until PTY data arrives or X event (keypress, etc.)
    --> ttyread() - reads PTY bytes, parses them, updates term.line
    --> XNextEvent() - dispatches X events (key/mouse/resize/etc.)
    --> [if idle or maxlatency hit] draw() [st.c] --> panel_poll()
```

How often:  
Not on every byte or every keypress. run() loops but only falls through to draw() after the batching logic decides a frame is due:
  - On first activity, it waits up to minlatency (default 2 ms) for more data.
  - If data keeps flowing (e.g. cat huge.txt), it keeps deferring with shrinking timeouts, but forces a draw after at most maxlatency (default 30 ms ~ 30 fps).
  - If nothing has arrived (idle), it blocks in pselect() indefinitely - draw() is not called at all until the next event.

So panel_poll() is called:
  - Never, when nothing is happening (no CPU use at idle).
  - ~30 times/second at most, during heavy PTY output.
  - Once per keypress (roughly), during interactive typing.
  - Once per blink tick (every blinktimeout ms = 800 ms by default), if any blinking text is visible.

### To-do
* Declare the cursor being covered when located in any column within panel rows.
  - Seems not working.

* /tmp/sc-* should be cleaned up. And $XDG_RUNTIME_DIR/sc-XXXXXX should be preferred.

* The legacy tty* function names in st.c are upstream API names and can remain. But new comments, fields, and docs should say “PTY” for transport/process-group details and “terminal” only for emulator/UI concepts. For example,
  ```
  // Show the overlay only while the shell owns the PTY's foreground process group.
  shell_owns_tty_ = (::tcgetpgrp(pty_fd_) == shell_pid_);
  ```

* Rename data members: dirty_ -> m_dirty, ...

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

#### Appearance
  - Application icon
  - `static unsigned int rows = 54` in config.h seems adjusting according to the terminal screen height. However, snap to the screen boundary without extra pixels.
  - Handle Symlinks and consider permissions
  - `history` selection menu.
  - Hangul typing shows a combination box.
  - `Panel::dirty_` for each panel row instead of the entire panel

#### Mouse
  - Mouse selects text within the panel.
  - Scroll back with mouse wheel.

#### Keys
  - F3: (colored) `less $0`
  - F4: `vim $0` (not gvim)
  - F5: fast copy using rsync?
  - diff directories, ...
  - Show memory usage, ...

#### Brief list panel

#### Two panels

#### Support VFS using fuse-zip
  - Not only viewing a file in zip, we can execute a file directly from the zip.
