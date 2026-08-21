// See LICENSE for license details.
//
// Minimal Norton-Commander-like overlay panel for st (C++17).
// See panel.hpp for the public class; see panel.h for the C ABI shim.
//
// This file implements Panel and exports the plain-C entry points that st.c / x.c call.
// The C API forwards to a single hidden Panel instance (g_panel). No C++ types leak
// across the ABI.

#include <algorithm>            // for std::any_of(), std::find_if(), std::sort(), ...
#include <cassert>              // for assert()
#include <chrono>               // for std::chrono::steady_clock
#include <cstdint>              // for uint32_t
#include <cstring>              // for std::strcmp(), std::strerror(), std::memset()
#include <ctime>                // for localtime_r(), std::strftime()
#include <string>               // for std::string, std::to_string(), ...
#include <utility>              // for std::move(), std::pair()

#include <dirent.h>             // for DIR, opendir(), ...
#include <sys/stat.h>           // for struct stat, lstat(), ...
#include <sys/types.h>          // for off_t, pid_t, time_t
#include <X11/X.h>              // for ControlMask, ShiftMask
#include <X11/keysym.h>         // for XK_*

#include "panel.hpp"            // for Panel
#include "sc_config.hpp"        // for SC configuration constants
#include "shell.hpp"            // for Shell, ZleEvent

extern "C" {
#include "panel.h"              // for the C ABI shim
}



namespace {

Shell g_shell;
Panel g_panel;

template <typename T>
constexpr T clamp_between(T v, T lo, T hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Human-readable size, fits in Panel::kColsSize cells (right-aligned when printed).
// Byte counts up to 1M are shown verbatim (exact); larger sizes are abbreviated as
// "DDDD.DM" (one truncated decimal digit, unit suffix M/G/T), where the integer part is
// always < 1024.
std::string format_size(off_t bytes)
{
    assert( bytes >= 0 );
    constexpr off_t ExactMax = 1024LL * 1024;
    if ( bytes <= ExactMax )
        return std::to_string(bytes);

    struct Unit { off_t div; char suffix; };
    static constexpr Unit kUnits[] = {
        { 1024LL * 1024, 'M' },
        { 1024LL * 1024 * 1024, 'G' },
        { 1024LL * 1024 * 1024 * 1024, 'T' },
    };

    const Unit* unit = &kUnits[sizeof(kUnits) / sizeof(kUnits[0]) - 1];  // fall-back
    for ( const Unit& u : kUnits ) {
        if ( bytes < u.div * 1024 ) {
            unit = &u;
            break;
        }
    }
    const off_t whole = bytes / unit->div;
    const off_t frac = (bytes % unit->div) * 10 / unit->div;

    std::string s = std::to_string(whole);
    s.push_back('.');
    s.push_back(static_cast<char>('0' + frac));
    s.push_back(unit->suffix);
    return s;
}

// Format mtime as {date="M/DD/YY", time="HH:MM" + "a"/"p"}. Both empty on failure
// (mtime <= 0 or localtime_r() error).
std::pair<std::string, std::string> format_mtime(time_t mtime)
{
    if ( mtime <= 0 ) return {};
    struct tm tm_val{};
    if ( !::localtime_r(&mtime, &tm_val) ) return {};

    std::string date, time;  // s.capacity() == 15 for Small String Optimization (SSO)
    date.resize(15);  // e.g. "12/31/99"
    time.resize(15);  // e.g. "12:59p"
    date.resize(std::strftime(date.data(), date.size(), "%-m/%d/%y", &tm_val));
    time.resize(std::strftime(time.data(), time.size(), "%-I:%M", &tm_val));
    if ( time.size() > 0 )
        time.push_back(tm_val.tm_hour < 12 ? 'a' : 'p');
    return {date, time};
}

} // namespace



bool Panel::visible() const
{
    return g_shell.owns_pty() && !m_hidden
        && m_canvas.width() > 0 && m_canvas.height() > 0;
}

void Panel::recompute_geometry()
{
    // Panel shows only when the terminal has room for both the panel and the shell.
    // kMinRows and kMinCols are the minimum terminal dimensions (each half gets at
    // least kMinRows/kFracHeight rows and kMinCols/kFracWidth cols).
    if ( m_term_rows < kMinRows || m_term_cols < kMinCols ) {
        m_canvas.reset(0, 0, 0, 0, m_term_cols);
        return;
    }

    const int top = 0;
    const int width = m_term_cols - m_term_cols / kFracWidth;  // half the terminal
    const int height = m_term_rows - m_term_rows / kFracHeight;
    const int left = m_term_cols - width;          // top-right placement
    assert( height >= kMinRowsPanel );
    compute_cols(width);

    m_canvas.reset(top, left, width, height, m_term_cols);
    m_dirty = true;
}

void Panel::load_entries(std::string_view prev_path)
{
    m_entries.clear();

    // Manually add ".." as the first entry in case the filesystem's readdir() does not
    // enumerate it.
    m_entries.emplace_back("..", true, 0, 0);

    if ( DIR* dir = ::opendir(m_cwd.c_str()) ) {
        while ( auto* dirent = ::readdir(dir) ) {
            if ( std::strcmp(dirent->d_name, ".")  == 0 ) continue;
            if ( std::strcmp(dirent->d_name, "..") == 0 ) continue;
            const std::string entry_path = m_cwd + dirent->d_name;
            struct stat st;
            if ( ::lstat(entry_path.c_str(), &st) != 0 )
                std::memset(&st, 0, sizeof(st));
            m_entries.emplace_back(dirent->d_name, S_ISDIR(st.st_mode), st.st_size,
                st.st_mtime);
        }
        ::closedir(dir);
    }

    // ".." is already first; order the remaining snapshot as directories, then files.
    std::sort(m_entries.begin() + 1, m_entries.end(),
        [](const Entry& a, const Entry& b) {
            if ( a.is_dir != b.is_dir ) return a.is_dir;
            return a.name < b.name;
        });

    // A missing slash yields name_pos == 0, which cannot match nonempty m_cwd.
    const size_t name_pos = prev_path.rfind('/') + 1;
    if ( m_cwd.size() == name_pos && prev_path.substr(0, name_pos) == m_cwd ) {
        const std::string_view prev_name = prev_path.substr(name_pos);
        const auto it = std::find_if(m_entries.begin() + 1, m_entries.end(),
            [&](const Entry& entry) { return entry.name == prev_name; });
        if ( it != m_entries.end() )
            m_selected_idx = static_cast<int>(it - m_entries.begin());
    }

    m_selected_idx = clamp_between(
        m_selected_idx, 0, static_cast<int>(m_entries.size()) - 1);
    m_dirty = true;
}

void Panel::render()
{
    const int width  = m_canvas.width();
    const int height = m_canvas.height();
    if ( width <= 0 || height <= 0 ) return;
    if ( !m_dirty ) return;
    m_dirty = false;

    const int list_rows = height - kRowsPanelFrame;  // excludes header and footer.
    const uint32_t fg = kFgDefault;
    const uint32_t bg = kBgDefault;
    auto draw = std::move(m_canvas.draw());

    // --- Row 0: top frame + title ---
    draw.move(0, 0).color(kFgFrame, bg).fill(kFrameH)
        .move(0).put(kFrameTL)
        .move(column.size_x - 1).put(kFrameTT)
        .move(column.date_x - 1).put(kFrameTT)
        .move(column.time_x - 1).put(kFrameTT)
        .move(width - 1).put(kFrameTR)
        .move(1).mid(width - 2).ellipsize(Draw::Keep::Right).put(m_cwd, ATTR_REVERSE);

    // --- Row 1: column headers ---
    draw.move(0, 1).color(kFgFrame, bg).fill(' ')
        .move(0).put(kFrameV)
        .left(column.name_w)
        .with_fg(kFgSelected, [](Draw& d){ d.put("Name", ATTR_BOLD); })
        .put(kFrameV)
        .right(column.size_w)
        .with_fg(kFgSelected, [](Draw& d){ d.put("Size", ATTR_BOLD); })
        .put(kFrameV)
        .mid(column.date_w)
        .with_fg(kFgSelected, [](Draw& d){ d.put("Date", ATTR_BOLD); })
        .put(kFrameV)
        .mid(column.time_w)
        .with_fg(kFgSelected, [](Draw& d){ d.put("Time", ATTR_BOLD); })
        .put(kFrameV);

    // Keep cursor in view.
    if ( m_selected_idx < m_first_visible_idx )
        m_first_visible_idx = m_selected_idx;
    if ( m_selected_idx >= m_first_visible_idx + list_rows )
        m_first_visible_idx = m_selected_idx - list_rows + 1;
    if ( m_first_visible_idx < 0 )
        m_first_visible_idx = 0;

    // --- Rows 2 .. height-4: entries ---
    for ( int i = 0 ; i < list_rows ; ++i ) {
        const int y = 2 + i;  // skip over the header.
        const int idx = m_first_visible_idx + i;

        if ( idx < 0 || idx >= static_cast<int>(m_entries.size()) ) {
            draw.move(0, y).color(fg, bg).fill(' ')  // Clear the line first.
                .move(0).color(kFgFrame).put(kFrameV)
                .move(column.size_x - 1).put(kFrameV)
                .move(column.date_x - 1).put(kFrameV)
                .move(column.time_x - 1).put(kFrameV)
                .move(width - 1).put(kFrameV);
            continue;
        }

        const bool selected = (idx == m_selected_idx);
        const ushort mode = selected
            ? ATTR_REVERSE | ATTR_CLEAR_FIELD : ATTR_CLEAR_FIELD;
        // The selected row's frame follows its text colour; other frames stay dim.
        const uint32_t m_fgtext  = fg;
        const uint32_t m_fgframe = selected ? fg : kFgFrame;

        const Entry& e = m_entries[idx];
        auto [date, time] = format_mtime(e.mtime);

        // Draw row frame.
        draw.move(0, y)
            .color(kFgFrame, bg).put(kFrameV).color(m_fgtext)

            // Name column (abbreviated to fit or left-aligned)
            .left(column.name_w).ellipsize(Draw::Keep::Both).put(
                e.name + (e.is_dir ? "/" : "")
                , mode)
            .with_fg(m_fgframe, [&](Draw& d){ d.put(kFrameV, mode); })

            // Size column (right-aligned)
            .right(column.size_w).put(
                e.is_dir
                ? (e.name == "..") ? "UP--DIR" : "SUB-DIR"
                : format_size(e.size)
                , mode)
            .with_fg(m_fgframe, [&](Draw& d){ d.put(kFrameV, mode); })

            // Date column
            .right(column.date_w).put(date, mode)
            .with_fg(m_fgframe, [&](Draw& d){ d.put(kFrameV, mode); })

            // Time column
            .right(column.time_w).put(time, mode)
            .color(kFgFrame).put(kFrameV);
    }

    // Row -3: separator frame
    draw.move(0, height - 3).color(kFgFrame, bg).fill(kFrameH)
        .move(0).put(kFrameLT)
        .move(column.size_x - 1).put(kFrameBT)
        .move(column.date_x - 1).put(kFrameBT)
        .move(column.time_x - 1).put(kFrameBT)
        .move(width - 1).put(kFrameRT);

    // Row -2: selected entry
    const Entry& e = m_entries[m_selected_idx];
    auto [date, time] = format_mtime(e.mtime);
    draw.move(0, height - 2).color(fg, bg).fill(' ')
        .move(0).with_fg(kFgFrame, [](Draw& d){ d.put(kFrameV); })

        // Name column (abbreviated to fit or left-aligned)
        .left(column.name_w).ellipsize(Draw::Keep::Both)
            .put(e.name + (e.is_dir ? "/" : ""))

        // Size column (right-aligned)
        .move(column.size_x)
        .right(column.size_w).put(
            e.is_dir
            ? (e.name == "..") ? "UP--DIR" : "SUB-DIR"
            : format_size(e.size))

        // Date column
        .move(column.date_x)
        .right(column.date_w).put(date)

        // Time column
        .move(column.time_x)
        .right(column.time_w).put(time)
        .color(kFgFrame).put(kFrameV);

    // Row -1: bottom frame
    draw.move(0, height - 1).color(kFgFrame, bg).fill(kFrameH)
        .move(0).put(kFrameBL)
        .move(width - 1).put(kFrameBR);
}



void Panel::reload_panel(std::string cwd)
{
    std::string prev_path;
    if ( cwd != m_cwd ) {
        // Preserve the directory being left when its absolute path names an ordinary
        // entry in the new cwd. Descending retains the index-zero ".." default.
        // Root cannot be an ordinary entry; other cwd paths omit their trailing slash.
        if ( m_cwd.size() > 1 ) {
            prev_path = m_cwd;
            prev_path.pop_back();
        }
        m_cwd = std::move(cwd);
        m_selected_idx = 0;
    }
    else if ( !m_entries.empty() ) {
        prev_path = m_cwd + m_entries[m_selected_idx].name;
    }
    load_entries(prev_path);
}

int Panel::adjust_padding(int applied_padding) const
{
    // Recover the prompt's row before its existing SC-owned newline prefix, then add
    // only enough newlines to place that row below the visible panel.
    int cursor_y;
    tgetcursor(nullptr, &cursor_y);
    const int prompt_y = std::max(0, cursor_y - applied_padding);
    const bool cursor_obscured = visible()
        && prompt_y >= m_canvas.top() && prompt_y < m_canvas.top() + m_canvas.height();
    return cursor_obscured ? m_canvas.top() + m_canvas.height() - prompt_y : 0;
}

std::optional<std::string_view> Panel::selected_entry() const
{
    if ( !visible() ) return std::nullopt;
    assert( !m_entries.empty() );
    return m_entries[m_selected_idx].name;
}

void Panel::poll(int* term_dirty)
{
    const bool was_visible = m_was_visible;
    const bool now_visible = visible();

    if ( was_visible != now_visible ) {
        const int top = clamp_between(m_canvas.top(), 0, m_term_rows - 1);
        const int bottom = clamp_between(
            m_canvas.top() + m_canvas.height() - 1, 0, m_term_rows - 1);
        for ( int i = top ; i <= bottom ; ++i )
            term_dirty[i] = 1;
    }

    m_needs_draw = now_visible && (was_visible != now_visible || m_dirty
      || std::any_of(
        term_dirty + m_canvas.top(),
        term_dirty + m_canvas.top() + m_canvas.height(),
        [](int dirty) { return dirty != 0; }));

    m_was_visible = now_visible;
}

void Panel::draw()
{
    if ( !m_needs_draw ) return;
    m_needs_draw = false;
    render();           // no-op unless m_dirty
    m_canvas.present();  // re-blits over rows the terminal just redrew underneath us
}

void Panel::resize(int cols, int rows)
{
    m_term_cols = cols;
    m_term_rows = rows;
    recompute_geometry();
    m_prompt_refresh_deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kResizeSettleDelayMs);
}

void Panel::adjust_timeout(double& timeout_ms)
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

void Panel::refresh_prompt()
{
    if ( visible() )
        g_shell.send_event(ZleEvent::RefreshPrompt);
}

void Panel::toggle_panel()
{
    m_hidden = !m_hidden;
    m_dirty = true;
    if ( visible() )
        refresh_prompt();
    // Repaint the overlay, or restore the terminal rows it covered when hidden.
    ::redraw();
}

bool Panel::handle_key(unsigned long ksym, unsigned state, const char*, int)
{
    assert( m_selected_idx >= 0 );  // m_entries[] is never empty (when visible()).
    if ( !visible() ) return false;
    const int list_rows = m_canvas.height() - kRowsPanelFrame;
    const int n = static_cast<int>(m_entries.size());
    const int old_selected_idx = m_selected_idx;

    // Note: Switch only expects non-printable keys.
    switch ( ksym ) {
        case XK_Up:
            --m_selected_idx;
            goto clamp_cursor;
        case XK_Down:
            ++m_selected_idx;
            goto clamp_cursor;
        case XK_Home:
            m_selected_idx = 0;
            goto clamp_cursor;
        case XK_End:
            m_selected_idx = n - 1;
            goto clamp_cursor;

        case XK_Page_Up:
            if ( (state & ControlMask) == 0 ) {
                m_selected_idx -= list_rows;
                goto clamp_cursor;
            }
            g_shell.send_event(ZleEvent::CdParent);
            return true;
        case XK_Page_Down:
            if ( (state & ControlMask) == 0 ) {
                m_selected_idx += list_rows;
                goto clamp_cursor;
            }
            g_shell.send_event(ZleEvent::CdChild);
            return true;

        clamp_cursor:
            m_selected_idx = clamp_between(m_selected_idx, 0, std::max(0, n - 1));
            if ( old_selected_idx != m_selected_idx ) {
                m_dirty = true;
                ::draw();
            }
            return true;

        case XK_Return:
        case XK_KP_Enter:
            if ( state & ControlMask ) {
                g_shell.send_event((state & ShiftMask)
                    ? ZleEvent::InsertPath : ZleEvent::InsertName);
                return true;
            }
            // Plain Enter belongs to ZLE, which has the authoritative BUFFER.
            return false;

        default:
            break;
    }

    return false;
}



// The single instance and the C ABI shim.

extern "C" {

void shell_preinit(void) { g_shell.preinit(); }
void shell_init(int pty_fd, pid_t shell_pid) { g_shell.init(pty_fd, shell_pid, g_panel); }
void shell_cleanup_ipc(void) { g_shell.cleanup(); }
int  shell_ipc_fd(void) { return g_shell.ipc_fd(); }
void shell_service_ipc(void) { (void)g_shell.service_ipc(g_panel); }

void panel_poll(int* term_dirty) { g_panel.poll(term_dirty); }
void panel_draw(void) { g_panel.draw(); }

void panel_resize(int cols, int rows) { g_panel.resize(cols, rows); }
void panel_adjust_timeout(double* timeout_ms) { g_panel.adjust_timeout(*timeout_ms); }
void panel_refresh_prompt(void) { g_panel.refresh_prompt(); }

void panel_toggle_panel(const Arg*) { g_panel.toggle_panel(); }
int  panel_handle_key(unsigned long ksym, unsigned state, const char* buf, int len) {
    return g_panel.handle_key(ksym, state, buf, len);
}

}  // extern "C"
