// See LICENSE for license details.

#include <algorithm>            // for std::any_of(), std::max()
#include <cassert>              // for assert()
#include <chrono>               // for std::chrono::steady_clock
#include <utility>              // for std::move()
#include <X11/X.h>              // for ControlMask, ShiftMask, ...
#include <X11/keysym.h>         // for XK_*

#include "comm.hpp"             // for Comm
#include "sc_config.hpp"        // for unlikely(), SC configuration constants

extern "C" {
#include "comm_api.h"           // for SC's terminal-facing C ABI
#include "st.h"                 // for draw(), tgetcursor()
}



bool Comm::any_panel_visible()
{
    return !m_hidden && m_focus->canvas().height() > 0 && shell_owns_pty();
}

void Comm::reload_panels(bool init)
{
    PanelDirectory directory = m_shell.capture_cwd();

    // The first preprompt establishes both panels' directory and snapshot invariants.
    // Later boundaries update only the focused panel, preserving the inactive panel's
    // complete directory state.
    if ( unlikely(init) ) {
        PanelDirectory duplicate = directory.duplicate();
        m_left.init(std::move(duplicate));
        m_right.init(std::move(directory));
    }
    else {
        m_focus->reload(std::move(directory));
    }
}

std::optional<std::string_view> Comm::selected_entry()
{
    if ( !any_panel_visible() ) return std::nullopt;
    return m_focus->selected_entry();
}

int Comm::adjust_padding(int applied_padding)
{
    int cursor_y;
    tgetcursor(nullptr, &cursor_y);
    const int prompt_y = std::max(0, cursor_y - applied_padding);
    const Canvas& canvas = m_focus->canvas();
    const bool cursor_obscured = any_panel_visible()
        && prompt_y >= canvas.top() && prompt_y < canvas.top() + canvas.height();
    return cursor_obscured ? canvas.top() + canvas.height() - prompt_y : 0;
}

void Comm::poll(int* term_dirty)
{
    const bool panels_visible = any_panel_visible();
    const auto [left_visibility_changed, left_needs_refresh] = m_left.set_visible(
        panels_visible && (m_dual_panel || is_focused(&m_left)));
    const auto [right_visibility_changed, right_needs_refresh] = m_right.set_visible(
        panels_visible && (m_dual_panel || is_focused(&m_right)));

    const Canvas& canvas = m_focus->canvas();
    const int top = canvas.top();
    const int bottom = top + canvas.height();
    if ( left_visibility_changed || right_visibility_changed )
        for ( int row = top; row < bottom; ++row )
            term_dirty[row] = 1;

    const bool covered_rows_dirty = std::any_of(
        term_dirty + top, term_dirty + bottom, [](int dirty) { return dirty != 0; });
    m_needs_draw = left_needs_refresh || right_needs_refresh
        || (panels_visible && covered_rows_dirty);
}

void Comm::draw_panels()
{
    if ( !m_needs_draw ) return;
    m_needs_draw = false;
    m_left.render();
    m_right.render();
}

void Comm::resize_panels(int cols, int rows)
{
    if ( rows < kMinRows || cols < kMinCols ) {
        m_left.set_geometry(0, 0, 0, 0, cols);
        m_right.set_geometry(0, 0, 0, 0, cols);
    }
    else {
        const int top = 0;
        const int height = rows - rows / kFracHeight;
        const int left_width = cols / kFracWidth;
        const int right_width = cols - left_width;
        assert( height >= kMinRowsPanel );
        m_left.set_geometry(top, 0, left_width, height, cols);
        m_right.set_geometry(top, left_width, right_width, height, cols);
    }

    m_prompt_refresh_deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kResizeSettleDelayMs);
}

void Comm::adjust_timeout(double& timeout_ms)
{
    if ( !m_prompt_refresh_deadline ) return;

    const auto now = std::chrono::steady_clock::now();
    if ( now >= *m_prompt_refresh_deadline ) {
        m_prompt_refresh_deadline.reset();
        refresh_prompt();
        return;
    }

    const double remaining = std::chrono::duration<double, std::milli>(
        *m_prompt_refresh_deadline - now).count();
    if ( timeout_ms < 0 || remaining < timeout_ms )
        timeout_ms = remaining;
}

void Comm::refresh_prompt()
{
    if ( any_panel_visible() )
        shell_send_event(ZleEvent::RefreshPrompt);
}

void Comm::toggle_panels()
{
    m_hidden = !m_hidden;
    m_left.dirty();
    m_right.dirty();
    refresh_prompt();
    ::draw();
}

void Comm::switch_panel()
{
    assert( m_dual_panel );
    m_focus = is_left(m_focus) ? &m_right : &m_left;
    m_left.dirty();
    m_right.dirty();
    ::draw();
    shell_send_event(ZleEvent::SwitchPanel);
}

void Comm::toggle_dual_panel()
{
    m_dual_panel = !m_dual_panel;
    m_left.dirty();
    m_right.dirty();
    ::draw();
}

bool Comm::handle_key(unsigned long ksym, unsigned state)
{
    const unsigned modifiers = state &
        (ShiftMask | ControlMask | Mod1Mask | Mod2Mask | Mod3Mask | Mod4Mask | Mod5Mask);

    // Forced hiding is handled before effective visibility so Ctrl+O can restore the
    // panels and retains its shortcut behavior while a child process owns the PTY.
    if ( ksym == XK_o && modifiers == ControlMask ) {
        toggle_panels();
        return true;
    }

    // Panel keys apply only while panels are visible; otherwise shell-defined keys such
    // as Up retain their normal behavior.
    if ( !any_panel_visible() ) return false;

    if ( m_dual_panel && ksym == XK_Tab && modifiers == 0 ) {
        switch_panel();
        return true;
    }
    if ( ksym == XK_p && modifiers == ControlMask ) {
        toggle_dual_panel();
        return true;
    }
    if ( ksym == XK_Page_Up && (state & ControlMask) ) {
        shell_send_event(ZleEvent::CdParent);
        return true;
    }
    if ( ksym == XK_Page_Down && (state & ControlMask) ) {
        shell_send_event(ZleEvent::CdChild);
        return true;
    }
    if ( ksym == XK_Return || ksym == XK_KP_Enter ) {
        if ( state & ControlMask ) {
            shell_send_event((state & ShiftMask)
                ? ZleEvent::InsertPath : ZleEvent::InsertName);
            return true;
        }
        // Plain Enter belongs to ZLE, which has the authoritative BUFFER.
        return false;
    }

    return m_focus->handle_key(ksym);
}



extern "C" {

void shell_preinit(void) { Comm::shell_preinit(); }
void shell_init(int pty_fd, pid_t shell_pid) { Comm::shell_init(pty_fd, shell_pid); }
void shell_cleanup_ipc(void) { Comm::shell_cleanup(); }
int  shell_ipc_fd(void) { return Comm::shell_ipc_fd(); }
void shell_service_ipc(void) { Comm::shell_service_ipc(); }

void panel_poll(int* term_dirty) { Comm::poll(term_dirty); }
void panel_draw(void) { Comm::draw_panels(); }

void panel_resize(int cols, int rows) { Comm::resize_panels(cols, rows); }
void panel_adjust_timeout(double* timeout_ms) { Comm::adjust_timeout(*timeout_ms); }
void panel_refresh_prompt(void) { Comm::refresh_prompt(); }

int panel_handle_key(unsigned long ksym, unsigned state) {
    return Comm::handle_key(ksym, state);
}

}  // extern "C"
