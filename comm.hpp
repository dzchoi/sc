// See LICENSE for license details.

// Global coordinator for SC's shell integration and left/right panel lifecycle.
// comm_api.h exposes its terminal-facing C ABI to st.c and x.c. Comm presents each
// visible Canvas after terminal drawing and never mutates term.line.

#pragma once

#include <chrono>               // for std::chrono::steady_clock
#include <optional>             // for std::optional<>
#include <string>               // for std::string
#include <string_view>          // for std::string_view

#include "panel.hpp"            // for Panel
#include "shell.hpp"            // for Shell, ZleEvent



class Comm {
public:
    // Static-only coordinator; no Comm object has an independent lifecycle.
    Comm() =delete;

    // Reports whether panel is the coordinator-owned left panel.
    static bool is_left(const Panel* panel) { return panel == &m_left; }
    // Reports whether panel is the coordinator-owned right panel.
    static bool is_right(const Panel* panel) { return panel == &m_right; }
    // Reports whether panel owns the current selection and shell directory.
    static bool is_focused(const Panel* panel) { return panel == m_focus; }
    // Reports effective visibility from hidden state, shell PTY ownership, and geometry.
    static bool any_panel_visible();
    // Reports whether the managed shell owns the PTY foreground process group.
    static bool shell_owns_pty() { return m_shell.owns_pty(); }
    // Sends one fixed control event to the managed shell's ZLE input stream.
    static void shell_send_event(ZleEvent event) { m_shell.send_event(event); }

    // Captures the shell directory, initializes both panels on `init`, then reloads
    // only the focused panel at later boundaries.
    static void reload_panels(bool init);
    // Returns the focused selection only while a panel is effectively visible.
    static std::optional<std::string_view> selected_entry();
    // Returns a procfs path ("/proc/<sc-pid>/fd/<panel-fd>") that resolves to the
    // focused panel's pinned directory.
    static std::string focused_directory() { return m_focus->directory_proc_path(); }
    // Computes prompt padding from the focused canvas and terminal cursor.
    static int adjust_padding(int applied_padding);

    // Creates and exports the private control socket before forking, or terminates SC.
    static void shell_preinit() { m_shell.preinit(); }
    // Associates the shell with its PTY and establishes snapshots before poll or draw.
    static void shell_init(int pty_fd, pid_t shell_pid) {
        m_shell.init(pty_fd, shell_pid);
    }
    // Signal-safely releases the parent-owned socket; forked shell cleanup is harmless.
    static void shell_cleanup() { m_shell.cleanup(); }
    // Returns the required shell adapter's control socket for the terminal event loop.
    static int shell_ipc_fd() { return m_shell.ipc_fd(); }
    // Services one pending request from the required shell adapter.
    static void shell_service_ipc() { m_shell.service_ipc(); }

    // Collects both panels and shared term.dirty rows before terminal drawing clears them.
    static void poll(int* term_dirty);
    // Presents every visible Canvas through xdrawline() when poll() selected a frame.
    static void draw_panels();
    // Assigns disjoint geometry without changing the terminal cursor.
    static void resize_panels(int cols, int rows);
    // Shortens a negative or finite event timeout for a pending resize prompt refresh.
    static void adjust_timeout(double& timeout_ms);
    // Requests a prompt refresh when the panels are effectively visible.
    static void refresh_prompt();
    // Routes visibility, focus, layout, shell-event, and panel keys before the PTY.
    static bool handle_key(unsigned long ksym, unsigned state);

private:
    // Debounce interval before resizing refreshes the shell prompt.
    static constexpr int kResizeSettleDelayMs = 150;

    // Left panel and its retained directory state.
    inline static Panel m_left;
    // Right panel and its retained directory state.
    inline static Panel m_right;
    // Non-null panel whose selection and directory drive shell actions.
    inline static Panel* m_focus = &m_right;
    // Forced-hidden state shared by both panels (Ctrl+O).
    inline static bool m_hidden = false;
    // Layout state selecting one focused panel or both panels (Ctrl+P).
    inline static bool m_dual_panel = false;
    // Frame decision preserved until terminal drawing has completed.
    inline static bool m_needs_draw = false;
    // Managed shell and private control-channel owner.
    inline static Shell m_shell;
    // Monotonic deadline for the resize-debounced prompt refresh.
    inline static std::optional<std::chrono::steady_clock::time_point>
        m_prompt_refresh_deadline;

    // Switches focus to the other panel and synchronizes the shell cwd.
    static void switch_panel();
    // Switches between single and dual layouts without changing focus.
    static void toggle_dual_panel();
    // Toggles forced hiding and invalidates covered rows without changing focus or layout.
    static void toggle_panels();
};
