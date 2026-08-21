# Window resizing

SC keeps the X window's pixel geometry and the terminal's character grid as
separate values. The window manager owns the final outer geometry; SC fits a
whole-cell grid inside the client area it receives.

## Negotiating the window size

At startup, `cols` and `rows` describe the requested terminal grid. After the
font is loaded, `xloadfonts()` calculates the integer cell dimensions `win.cw`
and `win.ch`. `xinit()` then requests this client size:

```text
width  = 2 * borderpx + cols * win.cw
height = 2 * borderpx + rows * win.ch
```

`xhints()` advertises that size together with `win.cw` and `win.ch` as the X11
resize increments and `2 * borderpx` as the base size. These are hints to the
window manager, not a requirement that it preserve the requested grid. A
floating window manager may honor the request, while a tiling or maximizing
window manager may assign the monitor's available client area instead.

Before starting normal event processing, `run()` records the latest
`ConfigureNotify` dimensions received while the window is being mapped. Later
`ConfigureNotify` events follow the same path through `resize()`. In both cases,
`cresize()` treats the window manager's pixel dimensions as authoritative and
derives the grid with integer division:

```text
col = (win.w - 2 * borderpx) / win.cw
row = (win.h - 2 * borderpx) / win.ch
```

Changing the configured `cols` or `rows` therefore changes the initial request,
but it does not force that grid when the window manager assigns a different
size.

## Unused pixels

Only complete cells become part of the terminal grid. `xresize()` records their
pixel extent separately:

```text
win.tw = col * win.cw
win.th = row * win.ch
```

The backing pixmap still has the full `win.w` by `win.h` dimensions and is
cleared to the default background color. Consequently, pixels left by the
integer divisions remain background-colored rather than becoming partial
cells. With the unpatched st layout, the fixed border stays at the top and left,
and any additional division remainder accumulates at the right and bottom.
`xdrawglyphfontspecs()` also clears those outer regions when drawing the last
terminal column or row.

This is why a window can align exactly with a screen or tile boundary even when
its pixel dimensions are not an exact multiple of the cell dimensions: the
window manager aligns the X window, and SC fills the unused client pixels.

## `COLUMNS` and `LINES`

After resizing the terminal model and drawing state, `cresize()` calls
`ttyresize()` with the complete-cell pixel extent. `ttyresize()` publishes both
the adjusted grid and its pixel dimensions to the pseudoterminal with the
`TIOCSWINSZ` ioctl:

```text
ws_col    = term.col
ws_row    = term.row
ws_xpixel = win.tw
ws_ypixel = win.th
```

SC does not assign the adjusted values directly to environment variables. In
fact, `execsh()` removes inherited `COLUMNS` and `LINES` values before starting
the shell so stale parent geometry cannot survive. The kernel communicates PTY
window-size changes to the foreground process group, normally with `SIGWINCH`,
and an interactive shell then updates its `COLUMNS` and `LINES` shell
parameters from `ws_col` and `ws_row`. Programs should likewise query the PTY
window size instead of relying on inherited environment values.
