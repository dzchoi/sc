## [Suckless Commander]

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

### [Blocked] Determining Enter key target context (Panel vs. Command Line)

Current implementation uses a simple state switch:
  - Panel focus: Enter targets the panel until a non-cursor navigation key is pressed.
  - Command Line focus: Once a non-cursor key is typed, Enter executes the shell command line until next prompt appears.

#### Checking the line input from `zsh`:
  - Modify user's .zshrc to install a hook that reports to the (upstream) terminal whether the current input line is non-empty, via a privatre OSC sequence (6770).
    ```
    zle-line-pre-redraw() {
        if (( ${BUFFER} )); then
          print -n '\e]6770;1\a'
        else
          print -n '\e]6770;0\a'
    }
    zle -N zle-line-pre-redraw
    ```
  - `strhandle()` [st.c] handles the OSC sequence.
    ```
    case 6770:
        if (narg > 1)
            panel_notify_cmdline(atoi(strescseq.args[1]) != 0);
        return;
    ```
  - `panel_notify_cmdline()` needs only to set typed_since_prompt_.
    ```
    void Panel::notify_cmdline(bool nonempty) { typed_since_prompt_ = nonempty; }
    ```
  - However, Bash does not support such a hook.

### To-do

#### Appearance
  - Typing under panel hides, though Cursor appears sometimes.
  - No icon
  - Scroll back with mouse wheel

#### Keys
  - F3: (colored) `less $0`
  - F4: `vim $0` (not gvim)
  - diff directories, ...
  - Show memory usage, ...

### Support VFS using fuse-zip
  - Not only viewing a file in zip, we can execute a file directly from the zip.

