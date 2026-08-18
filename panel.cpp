// See LICENSE for license details.
//
// Minimal Norton-Commander-like overlay panel for st (C++17).
// See panel.hpp for the public class; see panel.h for the C ABI shim.
//
// This file implements Panel and exports the plain-C entry points that st.c / x.c call.
// The C API forwards to a single hidden Panel instance (g_panel). No C++ types leak
// across the ABI.

#include <algorithm>            // for std::any_of(), std::lower_bound(), ...
#include <cassert>              // for assert()
#include <cerrno>               // for errno, EINTR
#include <chrono>               // for std::chrono::steady_clock
#include <cstdint>              // for uint32_t
#include <cstring>              // for std::strcmp(), std::memcpy(), ...
#include <ctime>                // for localtime_r(), std::strftime()
#include <cstdio>               // for std::snprintf()
#include <string>               // for std::string, std::to_string(), ...
#include <utility>              // for std::move(), std::pair()
#include <vector>               // for std::vector<>

#include <dirent.h>             // for DIR, opendir(), ...
#include <limits.h>             // for PATH_MAX
#include <poll.h>               // for poll(), pollfd, POLLIN, ...
#include <sys/stat.h>           // for struct stat, lstat(), ...
#include <sys/types.h>          // for pid_t, ssize_t
#include <termios.h>            // for tcgetpgrp()
#include <unistd.h>             // for close(), getcwd(), ...
#include <X11/X.h>              // for ControlMask, ShiftMask
#include <X11/keysym.h>         // for XK_*

#include "panel.hpp"            // for Panel
#include "sc_config.hpp"        // for SC configuration constants

extern "C" {
#include "panel.h"              // for the C ABI shim
// ttywrite() is declared in st.h, pulled in transitively by panel.hpp.
}



namespace {

template <typename T>
constexpr T clamp_between(T v, T lo, T hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

enum class ZleEvent {
    CdParent,
    CdChild,
    InsertName,
    InsertPath,
    RefreshPrompt,
};

// Deliver an SC control event to the shell's ZLE input stream.
void send_zle_event(ZleEvent event)
{
    constexpr const char* sequences[] = {
        "\033[6770~",  // CdParent
        "\033[6771~",  // CdChild
        "\033[6772~",  // InsertName
        "\033[6773~",  // InsertPath
        "\033[6774~",  // RefreshPrompt
    };
    const char* sequence = sequences[static_cast<unsigned>(event)];
    ttywrite(sequence, std::strlen(sequence), 1);
}

// Appends a trailing '/' unless already present (e.g. root "/").
static std::string with_trailing_slash(std::string s)
{
    if ( s.empty() || s.back() != '/' ) s.push_back('/');
    return s;
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
    return shell_owns_tty_ && !hidden_ && canvas_.width() > 0 && canvas_.height() > 0;
}

void Panel::recompute_geometry()
{
    // Panel shows only when the terminal has room for both the panel and the shell.
    // kMinRows and kMinCols are the minimum terminal dimensions (each half gets at
    // least kMinRows/kFracHeight rows and kMinCols/kFracWidth cols).
    if ( term_rows_ < kMinRows || term_cols_ < kMinCols ) {
        canvas_.reset(0, 0, 0, 0, term_cols_);
        return;
    }

    const int top = 0;
    const int width = term_cols_ - term_cols_ / kFracWidth;  // half the terminal
    const int height = term_rows_ - term_rows_ / kFracHeight;
    const int left = term_cols_ - width;          // top-right placement
    assert( height >= kMinRowsPanel );
    compute_cols(width);

    canvas_.reset(top, left, width, height, term_cols_);
    dirty_ = true;
}

void Panel::load_entries(const struct stat& prev_dir_stat)
{
    entries_.clear();

    // Manually add ".." as the first entry in case the filesystem's readdir() does not
    // enumerate it.
    Entry dotdot{"..", true, 0};
    struct stat st{};
    bool matched = false;
    if ( ::lstat((cwd_ + "..").c_str(), &st) == 0 ) {
        dotdot.size  = st.st_size;
        dotdot.mtime = st.st_mtime;
        matched = (st.st_dev == prev_dir_stat.st_dev && st.st_ino == prev_dir_stat.st_ino);
        if ( matched ) selected_idx_ = 0;
    }
    entries_.push_back(std::move(dotdot));

    if ( DIR* d = ::opendir(cwd_.c_str()) ) {
        while ( auto* de = ::readdir(d) ) {
            if ( std::strcmp(de->d_name, ".")  == 0 ) continue;
            if ( std::strcmp(de->d_name, "..") == 0 ) continue;
            Entry e;
            e.name = de->d_name;
            const std::string full = cwd_ + e.name;
            struct stat st{};  // zeroed each iteration in case lstat() below fails.
            if ( ::lstat(full.c_str(), &st) == 0 ) {
                e.is_dir = S_ISDIR(st.st_mode);
                e.size   = st.st_size;
                e.mtime  = st.st_mtime;
            }

            // Insert `e` at its sorted position (".." first, then dirs, then files).
            auto it = std::lower_bound(entries_.begin(), entries_.end(), e,
                [](const Entry& a, const Entry& b) {
                    if ( a.name == ".." ) return true;
                    if ( b.name == ".." ) return false;
                    if ( a.is_dir != b.is_dir ) return a.is_dir;
                    return a.name < b.name;
                });
            const int idx = static_cast<int>(it - entries_.begin());
            entries_.emplace(it, std::move(e));

            if ( matched ) {
                // Any later insertion landing at or before selected_idx_ shifts
                // selected_idx_ one slot to the right.
                if ( idx <= selected_idx_ ) ++selected_idx_;
            }
            else {
                matched = (st.st_dev == prev_dir_stat.st_dev && st.st_ino == prev_dir_stat.st_ino);
                if ( matched ) selected_idx_ = idx;
            }
        }

        ::closedir(d);
    }
    // The case `d == nullptr` can happen when the current directory is deleted or
    // permission-changed while we are still in it.

    // Fall back to the old selected_idx_ if prev_dir_stat was not found (e.g. after a same-dir
    // reload, or if the target no longer exists).
    if ( !matched ) {
        const int n = static_cast<int>(entries_.size());
        selected_idx_ = clamp_between(selected_idx_, 0, std::max(0, n - 1));
    }
    first_visible_idx_ = 0;
    dirty_ = true;
}

std::string Panel::shell_cwd()
{
    char buf[PATH_MAX];
    char proc[32];
    std::snprintf(proc, sizeof(proc), "/proc/%d/cwd", static_cast<int>(shell_pid_));
    ssize_t n = ::readlink(proc, buf, sizeof(buf) - 1);
    if ( n <= 0 ) return {};
    return with_trailing_slash(std::string(buf, n));
}

void Panel::render()
{
    const int width  = canvas_.width();
    const int height = canvas_.height();
    if ( width <= 0 || height <= 0 ) return;
    if ( !dirty_ ) return;
    dirty_ = false;

    const int list_rows = height - kRowsPanelFrame;  // excludes header and footer.
    const uint32_t fg = kFgDefault;
    const uint32_t bg = kBgDefault;
    auto draw = std::move(canvas_.draw());

    // --- Row 0: top frame + title ---
    draw.move(0, 0).color(kFgFrame, bg).fill(kFrameH)
        .move(0).put(kFrameTL)
        .move(column.size_x - 1).put(kFrameTT)
        .move(column.date_x - 1).put(kFrameTT)
        .move(column.time_x - 1).put(kFrameTT)
        .move(width - 1).put(kFrameTR)
        .move(1).mid(width - 2).ellipsize(Draw::Keep::Right).put(cwd_, ATTR_REVERSE);

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
    if ( selected_idx_ < first_visible_idx_ )
        first_visible_idx_ = selected_idx_;
    if ( selected_idx_ >= first_visible_idx_ + list_rows )
        first_visible_idx_ = selected_idx_ - list_rows + 1;
    if ( first_visible_idx_ < 0 )
        first_visible_idx_ = 0;

    // --- Rows 2 .. height-4: entries ---
    for ( int i = 0 ; i < list_rows ; ++i ) {
        const int y = 2 + i;  // skip over the header.
        const int idx = first_visible_idx_ + i;

        if ( idx < 0 || idx >= static_cast<int>(entries_.size()) ) {
            draw.move(0, y).color(fg, bg).fill(' ')  // Clear the line first.
                .move(0).color(kFgFrame).put(kFrameV)
                .move(column.size_x - 1).put(kFrameV)
                .move(column.date_x - 1).put(kFrameV)
                .move(column.time_x - 1).put(kFrameV)
                .move(width - 1).put(kFrameV);
            continue;
        }

        const bool selected = (idx == selected_idx_);
        const ushort mode = selected
            ? ATTR_REVERSE | ATTR_CLEAR_FIELD : ATTR_CLEAR_FIELD;
        // The selected row's frame follows its text colour; other frames stay dim.
        const uint32_t fg_text  = fg;
        const uint32_t fg_frame = selected ? fg : kFgFrame;

        const Entry& e = entries_[idx];
        auto [date, time] = format_mtime(e.mtime);

        // Draw row frame.
        draw.move(0, y)
            .color(kFgFrame, bg).put(kFrameV).color(fg_text)

            // Name column (abbreviated to fit or left-aligned)
            .left(column.name_w).ellipsize(Draw::Keep::Both).put(
                e.name + (e.is_dir ? "/" : "")
                , mode)
            .with_fg(fg_frame, [&](Draw& d){ d.put(kFrameV, mode); })

            // Size column (right-aligned)
            .right(column.size_w).put(
                e.is_dir
                ? (e.name == "..") ? "UP--DIR" : "SUB-DIR"
                : format_size(e.size)
                , mode)
            .with_fg(fg_frame, [&](Draw& d){ d.put(kFrameV, mode); })

            // Date column
            .right(column.date_w).put(date, mode)
            .with_fg(fg_frame, [&](Draw& d){ d.put(kFrameV, mode); })

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
    const Entry& e = entries_[selected_idx_];
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



Panel::Panel()
{
    // Constructed during static initialization, before the shell is forked from `st`.
    // The child shell inherits our current working directory (cwd) during the fork.
    char buf[PATH_MAX];
    // Cannot use shell_now() instead of getcwd() now until init() is called.
    cwd_ = with_trailing_slash(::getcwd(buf, sizeof(buf)) ? buf : "");
    recompute_geometry();
}

// Returns the total zsh-owned padding needed to place the prompt below the panel.
int Panel::prompt_padding(int applied_padding) const
{
    const int prompt_y = std::max(0, cursor_y_ - applied_padding);
    const bool cursor_obscured = visible()
        && prompt_y >= canvas_.top() && prompt_y < canvas_.top() + canvas_.height();
    return cursor_obscured ? canvas_.top() + canvas_.height() - prompt_y : 0;
}

const Panel::Entry* Panel::selected_entry() const
{
    if ( !visible() ) return nullptr;
    assert( !entries_.empty() );
    return &entries_[selected_idx_];
}

void Panel::init(int pty_fd, pid_t shell_pid)
{
    pty_fd_ = pty_fd;
    shell_pid_ = shell_pid;

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kZshReadyTimeoutMs);
    struct pollfd pfd{pty_fd_, POLLIN, 0};
    while ( !zsh_ready_ ) {
        const auto now = std::chrono::steady_clock::now();
        if ( now >= deadline )
            die("SC zsh adapter did not report readiness within %d ms; "
                "source sc.zsh from ~/.zshrc\n", kZshReadyTimeoutMs);

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count();
        const int timeout_ms = static_cast<int>(std::max<int64_t>(1, remaining));
        pfd.revents = 0;
        const int result = ::poll(&pfd, 1, timeout_ms);
        if ( result < 0 ) {
            if ( errno == EINTR ) continue;
            die("waiting for SC zsh adapter failed: %s\n", std::strerror(errno));
        }
        if ( result == 0 ) continue;
        if ( pfd.revents & POLLNVAL )
            die("waiting for SC zsh adapter failed: invalid PTY descriptor\n");

        // Preserve all startup output and let st's existing parser recognize OSC 6770.
        if ( pfd.revents & (POLLIN | POLLERR | POLLHUP) )
            ttyread();
    }

    // Read the authoritative startup state once. Normal polling begins only after the
    // required adapter has installed all private ZLE bindings.
    shell_owns_tty_ = (::tcgetpgrp(pty_fd_) == shell_pid_);
    if ( std::string cwd = shell_cwd(); !cwd.empty() )
        cwd_ = std::move(cwd);
    load_entries({});
}

bool Panel::poll()
{
    assert( pty_fd_ >= 0 && shell_pid_ > 0 );  // also asserts that .init() was called.
    const bool was_visible = visible();
    shell_owns_tty_ = (::tcgetpgrp(pty_fd_) == shell_pid_);
    const bool now_visible = visible();

    if ( now_visible ) {
        // zsh reports chpwd through the private OSC. Reconcile that event against the
        // shell's actual cwd via /proc.
        if ( !was_visible || cwd_changed_ ) {
            struct stat prev_dir_stat{};  // zero-initialized in case lstat() below fails.
            cwd_changed_ = false;
            if ( std::string cwd = shell_cwd(); !cwd.empty() && cwd != cwd_ ) {
                ::lstat(cwd_.c_str(), &prev_dir_stat);
                cwd_ = cwd;
                selected_idx_ = 0;  // Reset selection on a long jump (e.g. "cd /").
            }

            // If the shell's cwd changed, prev_dir_stat holds the stat of the directory
            // being left; load_entries() uses it to re-select that directory if
            // applicable.
            load_entries(prev_dir_stat);
        }
    }

    return was_visible != now_visible;
}

bool Panel::needs_draw(const int* term_dirty) const
{
    if ( !visible() ) return false;
    if ( dirty_ ) return true;       // our own content changed

    // Return true if terminal repainted a covered row.
    return std::any_of(
        term_dirty + canvas_.top(),
        term_dirty + canvas_.top() + canvas_.height(), 
        [](int d) { return d != 0; });
}

void Panel::draw()
{
    if ( !visible() ) return;
    render();           // no-op unless dirty_
    canvas_.present();  // re-blits over rows the terminal just redrew underneath us
}

void Panel::resize(int cols, int rows)
{
    term_cols_ = cols;
    term_rows_ = rows;
    recompute_geometry();
}

void Panel::refresh_prompt()
{
    if ( visible() )
        send_zle_event(ZleEvent::RefreshPrompt);
}

void Panel::toggle_panel()
{
    hidden_ = !hidden_;
    dirty_ = true;
    if ( visible() )
        refresh_prompt();
}

bool Panel::handle_key(unsigned long ksym, unsigned state, const char*, int)
{
    assert( selected_idx_ >= 0 );  // entries_[] is never empty (when visible()).
    if ( !visible() ) return false;
    const int list_rows = canvas_.height() - kRowsPanelFrame;
    const int n = static_cast<int>(entries_.size());
    const int old_selected_idx = selected_idx_;

    // Note: Switch only expects non-printable keys.
    switch ( ksym ) {
        case XK_Up:
            --selected_idx_;
            goto clamp_cursor;
        case XK_Down:
            ++selected_idx_;
            goto clamp_cursor;
        case XK_Home:
            selected_idx_ = 0;
            goto clamp_cursor;
        case XK_End:
            selected_idx_ = n - 1;
            goto clamp_cursor;

        case XK_Page_Up:
            if ( (state & ControlMask) == 0 ) {
                selected_idx_ -= list_rows;
                goto clamp_cursor;
            }
            send_zle_event(ZleEvent::CdParent);
            return true;
        case XK_Page_Down:
            if ( (state & ControlMask) == 0 ) {
                selected_idx_ += list_rows;
                goto clamp_cursor;
            }
            if ( entries_[selected_idx_].is_dir )
                send_zle_event(ZleEvent::CdChild);
            return true;

        clamp_cursor:
            selected_idx_ = clamp_between(selected_idx_, 0, std::max(0, n - 1));
            dirty_ |= (old_selected_idx != selected_idx_);
            return true;

        case XK_Return:
        case XK_KP_Enter:
            if ( state & ControlMask ) {
                send_zle_event((state & ShiftMask)
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



// =========================================================================
// The single instance and the C ABI shim.
// =========================================================================

namespace { Panel g_panel; }

extern "C" {

const char* panel_preinit(void) { return Panel::preinit(); }
int panel_ipc_fd(void) { return Panel::ipc_fd(); }
void panel_service_ipc(void) { g_panel.service_ipc(); }
void panel_cleanup_ipc(void) { Panel::cleanup_ipc(); }

void panel_init(int pty_fd, pid_t shell_pid) { g_panel.init(pty_fd, shell_pid); }
int  panel_poll(void) { return g_panel.poll(); }
int  panel_needs_draw(const int* term_dirty) { return g_panel.needs_draw(term_dirty); }
void panel_draw(void) { g_panel.draw(); }

void panel_resize(int cols, int rows) { g_panel.resize(cols, rows); }
void panel_set_cursor(int, int y) { Panel::set_cursor(y); }
void panel_notify_zsh_ready(void) { g_panel.notify_zsh_ready(); }
void panel_notify_cwd_changed(void) { g_panel.notify_cwd_changed(); }
void panel_refresh_prompt(void) { g_panel.refresh_prompt(); }

void panel_toggle_panel(void) { g_panel.toggle_panel(); }
int  panel_handle_key(unsigned long ksym, unsigned state, const char* buf, int len) {
    return g_panel.handle_key(ksym, state, buf, len);
}

}  // extern "C"
