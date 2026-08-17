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

/* Create SC's private control socket before forking the shell. The returned path is
 * inherited through SC_SOCKET. Returns NULL when setup failed. */
const char* panel_preinit(void);

/* Control socket integration for the zsh adapter. */
int panel_ipc_fd(void);
void panel_service_ipc(void);

/* Called once from ttynew() after the shell is forked. Must be called before
 * panel_poll() and panel_draw(). pty_fd is the master PTY fd; shell_pid is the forked
 * shell's PID. */
void panel_init(int pty_fd, pid_t shell_pid);

/* Refresh auto-visibility from the PTY's foreground process group. Returns 1 if
 * visibility just changed (caller must force all rows dirty so the terminal content
 * underneath, or the panel itself, gets repainted), 0 otherwise. */
int panel_poll(void);

/* Returns 1 if the panel overlay must be redrawn this frame:
 * either its own content changed, or the terminal has dirtied rows the panel covers.
 * term_dirty is term.dirty; must be read BEFORE drawregion() clears the flags. */
int panel_needs_draw(const int* term_dirty);

/* Paint the panel overlay via xdrawline(). No-op if not visible. */
void panel_draw(void);

/* Keep the panel informed of the terminal cursor without ever moving it. */
void panel_resize(int cols, int rows);
void panel_set_cursor(int x, int y);
void panel_notify_zsh_ready(void);
void panel_notify_cwd_changed(void);
void panel_refresh_prompt(void);

/* Ctrl+O handler. Toggles panel on/off. */
void panel_toggle_panel(void);

/* Called from kpress() BEFORE the character is forwarded to the PTY. ksym is an X11
 * KeySym (unsigned long). state is the X event state. Returns 1 if the panel consumed
 * the key, 0 otherwise (the key should go to the shell). */
int panel_handle_key(unsigned long ksym, unsigned state, const char* buf, int len);

#ifdef __cplusplus
}  /* extern "C" */
#endif
