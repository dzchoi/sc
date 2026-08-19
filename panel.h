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

/* Create SC's private control socket and export its path as SC_SOCKET before forking
 * the shell. Setup failures terminate SC. */
void shell_preinit(void);

/* Control socket used by SC's required zsh adapter. */
int shell_ipc_fd(void);
void shell_service_ipc(void);

/* Release the parent-owned control socket. Safe to call from a signal handler and
 * harmless in the forked shell process. */
void shell_cleanup_ipc(void);

/* Called once from ttynew() after the shell is forked. Waits up to
 * kZshReadyTimeoutMs for the required adapter before returning. Must be called before
 * panel_poll() and panel_draw(). pty_fd is the master PTY fd; shell_pid is the forked
 * shell's PID. */
void shell_init(int pty_fd, pid_t shell_pid);

/* Synchronize panel state and mark its covered terminal rows dirty when visibility
 * changes. term_dirty is term.dirty and must be passed before drawregion() clears it. */
void panel_poll(int* term_dirty);

/* Paint the panel overlay via xdrawline() if the current panel_poll() selected it. */
void panel_draw(void);

/* Keep the panel informed of terminal geometry without changing its cursor. */
void panel_resize(int cols, int rows);

/* Reduce timeout_ms when needed to refresh the prompt after a resize. A negative
 * timeout represents an unbounded wait. */
void panel_adjust_timeout(double* timeout_ms);

void shell_notify_zsh_ready(void);
void shell_notify_cwd_changed(void);
void panel_refresh_prompt(void);

/* Ctrl+O handler. Toggles the panel and dirties the terminal rows it covers. */
void panel_toggle_panel(const Arg*);

/* Called from kpress() BEFORE the character is forwarded to the PTY. ksym is an X11
 * KeySym (unsigned long). state is the X event state. Draws when it changes panel
 * selection. Returns 1 if the panel consumed the key, 0 otherwise. */
int panel_handle_key(unsigned long ksym, unsigned state, const char* buf, int len);

#ifdef __cplusplus
}  /* extern "C" */
#endif
