/* See LICENSE for license details.
 *
 * Overlay file-manager panel for st (NC-like, minimal).
 *
 * The panel is drawn on top of the terminal grid after drawregion() but before
 * xfinishdraw(). It never mutates term.line, so hiding the panel (by marking the rows
 * it covered dirty) restores the terminal content untouched.
 *
 * Visibility is auto-derived from the PTY's foreground process group:
 *   tcgetpgrp(cmdfd) == shell_pid  ==> panel visible
 *   otherwise (a child owns tty)   ==> panel hidden
 */

#pragma once

#include "st.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called from tresize() when the terminal geometry changes. */
void panel_resize(int cols, int rows);

/* Called once from ttynew() after the shell is forked. Must be called before
 * panel_poll() and panel_draw(). pty_fd is the master PTY fd; shell_pid is the forked
 * shell's PID. */
void panel_init(int pty_fd, pid_t shell_pid);

/* ----- Draw lifecycle: called from draw() each frome in this order ----- */

/* Refresh auto-visibility from the PTY's foreground process group.
 * Returns 1 if visibility just changed and the caller should force all rows dirty (so
 * the terminal content underneath is repainted on hide, or the overlay lands on fresh
 * terminal content on show). */
int panel_poll(void);

/* Returns 1 if the panel overlay must be redrawn this frame:
 * either its own content changed, or the terminal has dirtied rows the panel covers.
 * term_dirty is term.dirty; must be read BEFORE drawregion() clears the flags. */
int panel_needs_draw(const int* term_dirty);

/* Paint the panel overlay via xdrawline(). No-op if not visible. */
void panel_draw(void);

/* ----- User input (called from x.c) ----- */

/* Ctrl+O handler. Toggles panel on/off. */
void panel_toggle_panel(void);

/* Called from kpress() BEFORE the character is forwarded to the PTY. ksym is an X11
 * KeySym (unsigned long). state is the X event state. Returns 1 if the panel consumed
 * the key, 0 otherwise (the key should go to the shell). */
int panel_handle_key(unsigned long ksym, unsigned state, const char* buf, int len);

#ifdef __cplusplus
}  /* extern "C" */
#endif
