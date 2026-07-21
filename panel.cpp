// See LICENSE for license details.
//
// Minimal Norton-Commander-like overlay panel for st (C++17).
// See panel.hpp for the public class; see panel.h for the C ABI shim.
//
// This file implements Panel and exports the plain-C entry points that st.c / x.c call.
// The C API forwards to a single hidden Panel instance (g_panel). No C++ types leak
// across the ABI.

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <X11/keysym.h>

#include "panel.hpp"

extern "C" {
#include "panel.h"      // C ABI shim
// ttywrite() is declared in st.h, pulled in transitively by panel.hpp.
}

// =========================================================================
// Rendering configuration and free helpers (implementation-private).
// =========================================================================

namespace {

// ----- colors (indices into the 256-color palette) --------------------------
// 0:  black
// 1:  red
// 2:  green
// 3:  yellow
// 4:  blue
// 5:  magenta
// 6:  cyan
// 7:  white (light gray)
// 8:  bright black (dark gray)
// 9:  bright red
// 10: bright green
// 11: bright yellow
// 12: bright blue
// 13: bright magenta
// 14: bright cyan
// 15: bright white
// constexpr uint32_t kBg      = 4;   // blue
// constexpr uint32_t kFg      = 15;  // bright white
// constexpr uint32_t kSelBg   = 6;   // cyan
// constexpr uint32_t kSelFg   = 0;   // black
// constexpr uint32_t kFrameFg = 14;  // bright cyan
constexpr uint32_t kFg      = 7;
constexpr uint32_t kBg      = 4;
constexpr uint32_t kSelFg   = 3;
constexpr uint32_t kFrameFg = 6;

// ----- frame glyphs (Unicode box drawing) -----------------------------------
constexpr Rune kFrameH  = 0x2500;  // ─
constexpr Rune kFrameV  = 0x2502;  // │
constexpr Rune kFrameTL = 0x250c;  // ┌
constexpr Rune kFrameTR = 0x2510;  // ┐
constexpr Rune kFrameBL = 0x2514;  // └
constexpr Rune kFrameBR = 0x2518;  // ┘
constexpr Rune kFrameLT = 0x251c;  // ├
constexpr Rune kFrameRT = 0x2524;  // ┤
constexpr Rune kFrameTT = 0x252c;  // ┬
constexpr Rune kFrameBT = 0x2534;  // ┴

// ----- generic helpers ------------------------------------------------------

template <typename T>
constexpr T clamp_between(T v, T lo, T hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Shell-single-quote a path so it's safe to inject on a command line.
std::string shell_quote(std::string_view in)
{
    std::string out;
    out.reserve(in.size() + 8);
    out.push_back('\'');
    for (char c : in) {
        if (c == '\'') out += "'\\''";
        else           out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

// Send text to the PTY as if typed by the user.
inline void type_to_pty(std::string_view s)
{
    ttywrite(s.data(), s.size(), 1);
}

// Human-readable size, fits in C_.size_w cells (right-aligned when printed).
std::string format_size(off_t bytes)
{
    std::string s;
    s.reserve(16);
    if (bytes < 0) bytes = 0;
    if (bytes < 1024)
        s = std::to_string(bytes);
    else if (bytes < 1024L * 1024)
        s = std::to_string(bytes / 1024) + 'K';
    else if (bytes < 1024L * 1024 * 1024)
        s = std::to_string(bytes / (1024 * 1024)) + 'M';
    else
        s = std::to_string(bytes / (1024L * 1024 * 1024)) + 'G';
    return s;
}

// Format modtime as "MM/DD/YY" (kColDate wide) and "HH:MM" (kColTime wide). Empty on
// failure (mtime==0 or gmtime error).
struct DateTime {
    char date[Panel::kColDate + 1];
    char time[Panel::kColTime + 1];
};
DateTime format_mtime(time_t t)
{
    DateTime out{};  // zeros .date[] and .time[].
    if (t <= 0) return out;
    struct tm tm_val{};
    if (!::localtime_r(&t, &tm_val)) return out;
    std::strftime(out.date, sizeof(out.date), "%-m/%d/%y", &tm_val);
    std::strftime(out.time, sizeof(out.time), "%-I:%M", &tm_val);
    std::strcat(out.time, tm_val.tm_hour < 12 ? "a" : "p");
    return out;
}

} // namespace



// =========================================================================
// Panel: geometry
// =========================================================================

void Panel::recompute_geometry()
{
    // Panel shows only when the terminal has room for both the panel and the shell.
    // kMinRows and kMinCols are the minimum terminal dimensions (each half gets at least
    // kMinRows/kHeightFrac rows and kMinCols/kWidthFrac cols).
    if (term_rows_ < kMinRows || term_cols_ < kMinCols) {
        canvas_.reset(0, 0, 0, 0, term_cols_);
        return;
    }

    const int top = 0;
    const int width = term_cols_ / kWidthFrac;    // half the terminal
    const int height = term_rows_ / kHeightFrac;  // half the terminal
    const int left = term_cols_ - width;          // top-right placement
    assert(height >= 6);  // header (2) + one entry (1) + footer (3)
    compute_cols(width);

    canvas_.reset(top, left, width, height, term_cols_);
    dirty_ = true;
}

// =========================================================================
// Panel: data
// =========================================================================

void Panel::load_entries()
{
    entries_.clear();

    // Manually add ".." in case the filesystem's readdir() does not enumerate it.
    Entry dotdot{"..", true, 0};
    struct stat st{};
    if (::lstat((cwd_ + "/..").c_str(), &st) == 0) {
        dotdot.size  = st.st_size;
        dotdot.mtime = st.st_mtime;
    }
    entries_.push_back(std::move(dotdot));

    if (DIR* d = ::opendir(cwd_.c_str())) {
        while (auto* de = ::readdir(d)) {
            if (std::strcmp(de->d_name, ".")  == 0) continue;
            if (std::strcmp(de->d_name, "..") == 0) continue;
            Entry e;
            e.name = de->d_name;
            const std::string full = cwd_ + "/" + e.name;
            if (::lstat(full.c_str(), &st) == 0) {
                e.is_dir = S_ISDIR(st.st_mode);
                e.size   = st.st_size;
                e.mtime  = st.st_mtime;
            }
            entries_.push_back(std::move(e));
        }

        ::closedir(d);
    }
    // The case `d == nullptr` can happen when the current directory is deleted or
    // permission-changed while we are still in it.

    // Sort: ".." first, then dirs, then files, each alphabetical.
    std::sort(entries_.begin(), entries_.end(),
        [](const Entry& a, const Entry& b) {
            if (a.name == "..") return true;
            if (b.name == "..") return false;
            if (a.is_dir != b.is_dir) return a.is_dir;
            return a.name < b.name;
        });

    const int n = static_cast<int>(entries_.size());
    cursor_idx_ = clamp_between(cursor_idx_, 0, std::max(0, n - 1));
    scroll_idx_ = 0;
    dirty_ = true;
}

// void Panel::set_cwd(const std::string& path)
// {
//     char resolved[PATH_MAX];
//     cwd_ = ::realpath(path.c_str(), resolved) ? resolved : path;
//     selected_ = 0;
//     scroll_idx_ = 0;
//     load_entries();
// }

std::string Panel::shell_cwd()
{
    char buf[PATH_MAX];
    char proc[32];
    std::snprintf(proc, sizeof(proc), "/proc/%d/cwd", static_cast<int>(shell_pid_));
    ssize_t n = ::readlink(proc, buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    return std::string(buf, n);
}

// =========================================================================
// Panel: render()
// =========================================================================

void Panel::render()
{
    const int width  = canvas_.width();
    const int height = canvas_.height();
    if (width <= 0 || height <= 0) return;
    if (!dirty_) return;
    dirty_ = false;

    const int list_rows = height - 5;  // excludes header and footer.
    const uint32_t fg = kFg;
    const uint32_t bg = kBg;

    // Fill background across panel columns.
    auto draw = canvas_.draw().color(fg, bg);

    // --- Row 0: top frame + title ---
    draw.move(0, 0).color(kFrameFg).fill(kFrameH)
        .move(0).put(kFrameTL)
        .move(C_.sep1_x).put(kFrameTT)
        .move(C_.sep2_x).put(kFrameTT)
        .move(C_.sep3_x).put(kFrameTT)
        .move(width - 1).put(kFrameTR)
        .move(1).left(width - 2).put(cwd_, ATTR_REVERSE);

    // --- Row 1: column headers ---
    draw.move(0, 1).color(fg, bg).fill(' ')
        .move(0).color(kFrameFg).put(kFrameV)
        .color(3).left(C_.name_w).put("Name", ATTR_BOLD)  // in yellow
        .color(kFrameFg).put(kFrameV)
        .color(3).right(C_.size_w).put("Size", ATTR_BOLD)
        .color(kFrameFg).put(kFrameV)
        .color(3).left(C_.date_w).put("Date", ATTR_BOLD)
        .color(kFrameFg).put(kFrameV)
        .color(3).left(C_.time_w).put("Time", ATTR_BOLD)
        .color(kFrameFg).put(kFrameV);

    // Keep cursor in view.
    if (cursor_idx_ < scroll_idx_)
        scroll_idx_ = cursor_idx_;
    if (cursor_idx_ >= scroll_idx_ + list_rows)
        scroll_idx_ = cursor_idx_ - list_rows + 1;
    if (scroll_idx_ < 0)
        scroll_idx_ = 0;

    // --- Rows 2 .. height-4: entries ---
    for (int i = 0; i < list_rows; ++i) {
        const int y = 2 + i;  // skip over the header.
        const int idx = scroll_idx_ + i;

        const bool selected = (idx == cursor_idx_);
        const ushort mode = selected ? ATTR_REVERSE | ATTR_FILL_UNCOVERED : ATTR_NULL;
        const uint32_t ffg = selected ? fg : kFrameFg;

        draw.move(0, y).color(fg, bg).fill(' ');

        if (idx < 0 || idx >= static_cast<int>(entries_.size())) {
            draw.move(0).color(kFrameFg).put(kFrameV)
                .move(C_.sep1_x).put(kFrameV)
                .move(C_.sep2_x).put(kFrameV)
                .move(C_.sep3_x).put(kFrameV)
                .move(width - 1).put(kFrameV);
            continue;
        }

        const Entry& e = entries_[idx];
        DateTime dt = format_mtime(e.mtime);

        // Draw row frame.
        draw.move(0).color(kFrameFg).put(kFrameV)

            // Name column (left-aligned; truncated to fit)
            .color(fg).left(C_.name_w).put(
                e.name + (e.is_dir ? "/" : "")
                , mode)
            .color(ffg).put(kFrameV, mode)

            // Size column (right-aligned)
            .color(fg).right(C_.size_w).put(
                e.is_dir
                ? (e.name == "..") ? "UP--DIR" : "SUB-DIR"
                : format_size(e.size)
                , mode)
            .color(ffg).put(kFrameV, mode)

            // Date column
            .color(fg).right(C_.date_w).put(dt.date, mode)
            .color(ffg).put(kFrameV, mode)

            // Time column
            .color(fg).right(C_.time_w).put(dt.time, mode)
            .color(kFrameFg).put(kFrameV);
    }

    // Row -3: separator frame
    draw.move(0, height - 3).color(kFrameFg, bg).fill(kFrameH)
        .move(0).put(kFrameLT)
        .move(C_.sep1_x).put(kFrameBT)
        .move(C_.sep2_x).put(kFrameBT)
        .move(C_.sep3_x).put(kFrameBT)
        .move(width - 1).put(kFrameRT);

    // Row -2: selected entry
    const Entry& e = entries_[cursor_idx_];
    DateTime dt = format_mtime(e.mtime);
    draw.move(0, height - 2).color(fg, bg).fill(' ')
        .move(0).color(kFrameFg).put(kFrameV).color(fg)

        // Name column (left-aligned; truncated to fit)
        .left(C_.name_w).put(e.name + (e.is_dir ? "/" : ""))

        // Size column (right-aligned)
        .move(C_.size_x)
        .right(C_.size_w).put(
            e.is_dir
            ? (e.name == "..") ? "UP--DIR" : "SUB-DIR"
            : format_size(e.size))

        // Date column
        .move(C_.date_x)
        .right(C_.date_w).put(dt.date)

        // Time column
        .move(C_.time_x)
        .right(C_.time_w).put(dt.time)
        .color(kFrameFg).put(kFrameV);

    // Row -1: bottom frame
    draw.move(0, height - 1).color(kFrameFg, bg).fill(kFrameH)
        .move(0).put(kFrameBL)
        .move(width - 1).put(kFrameBR);
}

// =========================================================================
// Panel: public API
// =========================================================================

Panel::Panel()
{
    // Constructed during static initialization, before the shell is forked from `st`.
    // The child shell inherits our current working directory (cwd) during the fork.
    char buf[PATH_MAX];
    // Cannot use shell_now() instead of getcwd() now until init() is called.
    cwd_ = ::getcwd(buf, sizeof(buf)) ? buf : "/";
    recompute_geometry();
    load_entries();
}

void Panel::resize(int cols, int rows)
{
    term_cols_ = cols;
    term_rows_ = rows;
    recompute_geometry();
}

void Panel::init(int pty_fd, pid_t shell_pid)
{
    pty_fd_ = pty_fd;
    shell_pid_ = shell_pid;
}

bool Panel::poll()
{
    assert(pty_fd_ >= 0 && shell_pid_ > 0);  // also asserts that .init() was called.
    const bool was = visible();
    visible_ = (::tcgetpgrp(pty_fd_) == shell_pid_);
    if (visible()) {
        // A foreground command just finished -> prompt is fresh again and files may
        // have been modified; force a reload.
        static std::string last_cwd;
        bool needs_reload = !was;
        if (needs_reload) typed_since_prompt_ = false;

        // Detect a shell-initiated cwd change (e.g. user typed `cd` at the prompt)
        // by watching for a change in the shell's /proc cwd. We compare against
        // the *previous* shell cwd, not against cwd_, so panel-initiated `cd`s
        // we've already applied to cwd_ don't get clobbered while the shell is still
        // catching up. Two cases:
        //   - shell cwd changed AND differs from cwd_: user cd'd via the shell;
        //     adopt it.
        //   - shell cwd changed to match cwd_: shell just caught up to a panel-
        //     initiated cd; leave cwd_ alone.
        if (std::string cwd = shell_cwd(); !cwd.empty() && cwd != last_cwd) {
            if (cwd != cwd_) {
                cwd_ = cwd;
                cursor_idx_ = 0;
                scroll_idx_ = 0;
                needs_reload = true;
            }
            last_cwd = std::move(cwd);
        }

        if (needs_reload) load_entries();
    }

    return was != visible();
}

bool Panel::needs_draw(const int* term_dirty) const
{
    if (!visible()) return false;
    if (dirty_) return true;       // our own content changed
    // Return true if terminal repainted a covered row.
    return std::any_of(
        term_dirty + canvas_.top(),
        term_dirty + canvas_.top() + canvas_.height(), 
        [](int d) { return d != 0; });
}

void Panel::draw()
{
    if (!visible()) return;
    render();          // no-op if dirty
    canvas_.present();
}

void Panel::toggle_panel()
{
    (void)0;
}

bool Panel::handle_key(unsigned long ksym, unsigned /*state*/, const char* buf, int len)
{
    if (!visible()) return false;
    const int list_rows = std::max(1, canvas_.height() - 5);  // header (2) + footer (3)
    const int n = static_cast<int>(entries_.size());

    // Note: Switch only expects non-printable keys.
    switch (ksym) {
        case XK_Up:
            --cursor_idx_;
            goto clamp_selected;
        case XK_Down:
            ++cursor_idx_;
            goto clamp_selected;
        case XK_Home:
            cursor_idx_ = 0;
            goto mark_dirty;
        case XK_End:
            cursor_idx_ = n - 1;
            goto clamp_selected;
        case XK_Page_Up:
            cursor_idx_ -= list_rows;
            goto clamp_selected;
        case XK_Page_Down:
            cursor_idx_ += list_rows;
            goto clamp_selected;

        clamp_selected:
            cursor_idx_ = clamp_between(cursor_idx_, 0, std::max(0, n - 1));
        mark_dirty:
            dirty_ = true;
            return true;

        // case XK_BackSpace:
        //     // Consistent with Enter-on-dir: keep the shell in sync so a later command
        //     // runs in the directory the user is viewing, and so the next `!was` sync
        //     // in poll() doesn't snap the panel back to the shell's untouched cwd.
        //     set_cwd(cwd_ + "/..");
        //     type_to_pty("cd " + shell_quote(cwd_) + "\n");
        //     return true;

        case XK_Return:
        case XK_KP_Enter: {
            if (typed_since_prompt_) {
                // Let the shell handle Enter after the user started typing a command.
                // The input line is about to be submitted, so the prompt is fresh again
                // immediately - poll() no longer has to guess when to reset this flag.
                typed_since_prompt_ = false;
                return false;
            }

            const Entry& e = entries_[cursor_idx_];
            if (e.is_dir) {
                // We could change cwd now to update the panel quickly.
                // set_cwd(cwd_ + "/" + e.name);
                // type_to_pty("cd " + shell_quote(cwd_) + "\n");
                type_to_pty("cd " + shell_quote(cwd_ + "/" + e.name) + "\n");
            } else {
                // Execute the selected file immediately.
                type_to_pty(shell_quote(cwd_ + "/" + e.name) + "\n");
            }
            return true;
        }

        // case XK_Escape:
        // case XK_Tab:
        //     return true;
    }

    if (len > 0 && std::isprint(static_cast<unsigned char>(buf[0])))
        typed_since_prompt_ = true;

    return false;
}



// =========================================================================
// The single instance and the C ABI shim.
// =========================================================================

namespace { Panel g_panel; }

extern "C" {

void panel_resize(int cols, int rows) { g_panel.resize(cols, rows); }
void panel_init(int pty_fd, pid_t shell_pid) { g_panel.init(pty_fd, shell_pid); }

int  panel_poll(void) { return g_panel.poll();}
int  panel_needs_draw(const int* term_dirty) { return g_panel.needs_draw(term_dirty); }
void panel_draw(void) { g_panel.draw(); }

void panel_toggle_panel(void) { g_panel.toggle_panel(); }

int  panel_handle_key(unsigned long ksym, unsigned state, const char* buf, int len) {
    return g_panel.handle_key(ksym, state, buf, len);
}

}  // extern "C"
