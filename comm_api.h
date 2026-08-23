/* See LICENSE for license details. */

#pragma once

#include <sys/types.h>          // for pid_t

#ifdef __cplusplus
extern "C" {
#endif

void shell_preinit(void);
int shell_ipc_fd(void);
void shell_service_ipc(void);
void shell_cleanup_ipc(void);
void shell_init(int pty_fd, pid_t shell_pid);

void panel_poll(int* term_dirty);
void panel_draw(void);
void panel_resize(int cols, int rows);
void panel_adjust_timeout(double* timeout_ms);
void panel_refresh_prompt(void);
int panel_handle_key(unsigned long ksym, unsigned state);

#ifdef __cplusplus
}
#endif
