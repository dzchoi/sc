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
#include "win.h"        // xdrawline()
// ttywrite() is declared in st.h, pulled in transitively by panel.hpp.
}

// =========================================================================
// Rendering configuration and free helpers (implementation-private).
// =========================================================================

namespace {

// ----- geometry defaults ----------------------------------------------------
constexpr int kHeightFrac = 2;    // panel takes 1/kHeightFrac of terminal rows
constexpr int kMinRows    = 12;   // minimum terminal rows to show the panel
constexpr int kWidthFrac  = 2;    // panel takes 1/kWidthFrac of terminal cols
constexpr int kMinCols    = 80;   // minimum terminal cols to show the panel

// ----- NC-style column layout ----------------------------------------------
// Fixed-width columns (in cells):
constexpr int kColSize = 7;   // "1234567" / "1234K" / "SUB-DIR"
constexpr int kColDate = 8;   // "MM/DD/YY"
constexpr int kColTime = 5;   // "HH:MM"
// The row structure inside the frames is:
//   | Name | Size | Date | Time |
// That is 5 vertical bars (2 outer frame + 3 internal) and 4 columns.
// So Name width = panel_width - (5 bars) - kColSize - kColDate - kColTime.
constexpr int kFixedCells = 5 + kColSize + kColDate + kColTime;   // 25

// ----- colors (indices into the 256-color palette) --------------------------
constexpr uint32_t kBg      = 4;   // blue
constexpr uint32_t kFg      = 15;  // bright white
constexpr uint32_t kSelBg   = 6;   // cyan
constexpr uint32_t kSelFg   = 0;   // black
constexpr uint32_t kFrameFg = 14;  // bright cyan

// ----- frame glyphs (Unicode box drawing) -----------------------------------
constexpr Rune kFrameH  = 0x2500;  // ─
constexpr Rune kFrameV  = 0x2502;  // │
constexpr Rune kFrameTL = 0x250c;  // ┌
constexpr Rune kFrameTR = 0x2510;  // ┐
constexpr Rune kFrameLT = 0x251c;  // ├
constexpr Rune kFrameRT = 0x2524;  // ┤

// ----- generic helpers ------------------------------------------------------

inline void set_glyph(
    Glyph& g, Rune u, uint32_t fg, uint32_t bg, ushort mode = ATTR_NULL)
{
    g.u    = u;
    g.fg   = fg;
    g.bg   = bg;
    g.mode = mode;
}

template <typename T>
constexpr T clamp_between(T v, T lo, T hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Minimal UTF-8 decoder. Returns bytes consumed; writes code point to *out.
int utf8_next(const unsigned char* p, Rune* out)
{
    if (*p < 0x80) { *out = *p; return 1; }
    if ((*p & 0xe0) == 0xc0 && p[1]) {
        *out = (Rune(*p & 0x1f) << 6) | (p[1] & 0x3f);
        return 2;
    }
    if ((*p & 0xf0) == 0xe0 && p[1] && p[2]) {
        *out = (Rune(*p & 0x0f) << 12) |
               (Rune(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
        return 3;
    }
    if ((*p & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) {
        *out = (Rune(*p & 0x07) << 18) |
               (Rune(p[1] & 0x3f) << 12) |
               (Rune(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
        return 4;
    }
    *out = '?';
    return 1;
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
void type_to_pty(std::string_view s)
{
    if (!s.empty()) ttywrite(s.data(), s.size(), 1);
}

// Human-readable size, fits in kColSize cells (right-aligned when printed).
std::string format_size(off_t bytes)
{
    long long s = static_cast<long long>(bytes);
    if (s < 0) s = 0;
    char buf[16];
    if      (s < 1024)                 std::snprintf(buf, sizeof(buf), "%lld",  s);
    else if (s < 1024LL * 1024)        std::snprintf(buf, sizeof(buf), "%lldK", s / 1024);
    else if (s < 1024LL * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%lldM", s / (1024 * 1024));
    else                               std::snprintf(buf, sizeof(buf), "%lldG", s / (1024LL * 1024 * 1024));
    return buf;
}

// Format modtime as "MM/DD/YY" (kColDate wide) and "HH:MM" (kColTime wide). Empty on
// failure (mtime==0 or gmtime error).
struct DateTime {
    char date[kColDate + 1];
    char time[kColTime + 1];
};
DateTime format_mtime(time_t t)
{
    DateTime out{};
    if (t <= 0) return out;
    struct tm tm_val{};
    if (!::localtime_r(&t, &tm_val)) return out;
    std::strftime(out.date, sizeof(out.date), "%m/%d/%y", &tm_val);
    std::strftime(out.time, sizeof(out.time), "%H:%M",    &tm_val);
    return out;
}

// ----- NC column geometry --------------------------------------------------
// Column X positions inside the panel (panel-local, 0..width_-1).
// Row layout:  | Name... | Size | Date | Time |
struct Cols {
    int name_x, name_w;
    int sep1_x;   // | between Name and Size
    int size_x;
    int sep2_x;   // | between Size and Date
    int date_x;
    int sep3_x;   // | between Date and Time
    int time_x;
};

// Compute column X positions given the panel width. Assumes width >= kMinCols/kWidthFrac.
Cols compute_cols(int width)
{
    Cols c{};
    c.time_x = width - 1 - kColTime;               // just before right frame
    c.sep3_x = c.time_x - 1;
    c.date_x = c.sep3_x - kColDate;
    c.sep2_x = c.date_x - 1;
    c.size_x = c.sep2_x - kColSize;
    c.sep1_x = c.size_x - 1;
    c.name_x = 1;                                  // just after left frame
    c.name_w = std::max(1, c.sep1_x - c.name_x);   // shrinks/grows with width
    return c;
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
        height_ = width_ = left_ = 0;
        return;
    }
    height_ = term_rows_ / kHeightFrac;  // half the terminal
    width_  = term_cols_ / kWidthFrac;   // half the terminal
    left_   = term_cols_ - width_;       // top-right placement

    // Size the render buffer to the full terminal width so row_ptr() + left_ always
    // points into the row's panel slice. Cells to the left of left_ are never read
    // (xdrawline is called with x1 = left_).
    const size_t need = static_cast<size_t>(height_) * term_cols_;
    if (need > linebuf_.size()) linebuf_.resize(need);
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
    selected_ = clamp_between(selected_, 0, std::max(0, n - 1));
    viewport_ = 0;
    dirty_ = true;
}

// void Panel::set_cwd(const std::string& path)
// {
//     char resolved[PATH_MAX];
//     cwd_ = ::realpath(path.c_str(), resolved) ? resolved : path;
//     selected_ = 0;
//     viewport_ = 0;
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
// Panel: rendering primitives
// =========================================================================

void Panel::clear_row(int y, uint32_t fg, uint32_t bg)
{
    Glyph* r = row_ptr(y);
    for (int x = 0; x < width_; x++)
        set_glyph(r[left_ + x], ' ', fg, bg);
}

void Panel::put_text(int y, int col, std::string_view s, int max_len,
                     uint32_t fg, uint32_t bg, ushort mode)
{
    if (y < 0 || y >= height_) return;
    Glyph* r = row_ptr(y);
    int x = col;
    int end = std::min(col + max_len, width_);

    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const unsigned char* pend = p + s.size();
    while (p < pend && *p && x < end) {
        Rune u;
        int n = utf8_next(p, &u);
        if (p + n > pend) break;
        set_glyph(r[left_ + x], u, fg, bg, mode);
        ++x;
        p += n;
    }
}

// =========================================================================
// Panel: render()
// =========================================================================

void Panel::render()
{
    if (height_ <= 0 || width_ <= 0 || term_cols_ <= 0) return;
    if (!dirty_) return;
    dirty_ = false;

    const uint32_t bg = kBg;
    const uint32_t fg = kFg;
    const Cols C = compute_cols(width_);

    // Fill background across panel columns.
    for (int y = 0; y < height_; y++) clear_row(y, fg, bg);

    // --- Row 0: top frame + title (cwd) ---
    {
        Glyph* r = row_ptr(0);
        for (int x = 0; x < width_; x++)
            set_glyph(r[left_ + x], kFrameH, kFrameFg, bg);
        set_glyph(r[left_],              kFrameTL, kFrameFg, bg);
        set_glyph(r[left_ + width_ - 1], kFrameTR, kFrameFg, bg);

        std::string title = std::string(" ") + cwd_ + " ";
        int tlen = std::min<int>(title.size(), std::max(0, width_ - 3));
        if (tlen > 0) put_text(0, 1, title, tlen, kFg, bg, ATTR_BOLD);
    }

    // --- Row 1: column headers ---
    // Frame verticals + "Name  Size  Date  Time" labels.
    auto draw_row_frame = [&](int y, uint32_t rfg, uint32_t rbg) {
        Glyph* r = row_ptr(y);
        set_glyph(r[left_],              kFrameV, kFrameFg, bg);
        set_glyph(r[left_ + width_ - 1], kFrameV, kFrameFg, bg);
        set_glyph(r[left_ + C.sep1_x],   kFrameV, kFrameFg, bg);
        set_glyph(r[left_ + C.sep2_x],   kFrameV, kFrameFg, bg);
        set_glyph(r[left_ + C.sep3_x],   kFrameV, kFrameFg, bg);
        // Fill interior (skipping the separators) with row background.
        for (int x = 1; x < width_ - 1; x++) {
            if (x == C.sep1_x || x == C.sep2_x || x == C.sep3_x) continue;
            set_glyph(r[left_ + x], ' ', rfg, rbg);
        }
    };

    {
        const int y = 1;
        draw_row_frame(y, kFg, bg);
        // Name column header: left-aligned label.
        put_text(y, C.name_x, "Name", std::min(4, C.name_w), kFg, bg, ATTR_BOLD);
        // Size column: right-align "Size" within kColSize cells.
        {
            const int pad = kColSize - 4;                // "Size" is 4 chars
            put_text(y, C.size_x + std::max(0, pad), "Size", 4, kFg, bg, ATTR_BOLD);
        }
        // Date column: center "Date" in kColDate cells.
        {
            const int pad = (kColDate - 4) / 2;
            put_text(y, C.date_x + std::max(0, pad), "Date", 4, kFg, bg, ATTR_BOLD);
        }
        // Time column: center "Time" in kColTime cells.
        {
            const int pad = (kColTime - 4) / 2;
            put_text(y, C.time_x + std::max(0, pad), "Time", 4, kFg, bg, ATTR_BOLD);
        }
    }

    // --- Middle: file list. Rows 2 .. height_-3 (inclusive). ---
    const int list_top  = 2;
    const int list_bot  = std::max(list_top + 1, height_ - 2);   // exclusive
    const int list_rows = std::max(1, list_bot - list_top);

    // Keep cursor in view.
    if (selected_ < viewport_)              viewport_ = selected_;
    if (selected_ >= viewport_ + list_rows) viewport_ = selected_ - list_rows + 1;
    if (viewport_ < 0)                      viewport_ = 0;

    for (int i = 0; i < list_rows; i++) {
        const int y = list_top + i;
        const int idx = viewport_ + i;

        const bool selected = (idx == selected_);
        const uint32_t rfg = selected ? kSelFg : fg;
        const uint32_t rbg = selected ? kSelBg : bg;

        draw_row_frame(y, rfg, rbg);

        if (idx < 0 || idx >= static_cast<int>(entries_.size())) continue;
        const Entry& e = entries_[idx];

        // --- Name column (left-aligned; truncated to fit) ---
        {
            // Leading space is inside the column so text isn't glued to �.
            std::string label = e.name + (e.is_dir ? "/" : "");
            const ushort mode = e.is_dir ? ATTR_BOLD : ATTR_NULL;
            put_text(y, C.name_x, label, C.name_w, rfg, rbg, mode);
        }

        // --- Size column (right-aligned) ---
        {
            std::string sz;
            if (e.is_dir) sz = (e.name == "..") ? "UP--DIR" : "SUB-DIR";
            else          sz = format_size(e.size);
            int slen = std::min<int>(sz.size(), kColSize);
            int sx   = C.size_x + (kColSize - slen);   // right-align
            put_text(y, sx, sz, slen, rfg, rbg);
        }

        // --- Date + Time columns ---
        if (e.mtime > 0) {
            DateTime dt = format_mtime(e.mtime);
            if (dt.date[0])
                put_text(y, C.date_x, dt.date, kColDate, rfg, rbg);
            if (dt.time[0])
                put_text(y, C.time_x, dt.time, kColTime, rfg, rbg);
        }
    }

    // --- Status row (height - 2): shows selected entry's full name. ---
    if (height_ >= 3) {
        const int y = height_ - 2;
        Glyph*    r = row_ptr(y);
        for (int x = 0; x < width_; x++)
            set_glyph(r[left_ + x], kFrameH, kFrameFg, bg);
        set_glyph(r[left_],              kFrameLT, kFrameFg, bg);
        set_glyph(r[left_ + width_ - 1], kFrameRT, kFrameFg, bg);
        if (selected_ >= 0 && selected_ < static_cast<int>(entries_.size())) {
            const Entry& e = entries_[selected_];
            std::string sl = " " + e.name + (e.is_dir ? "/" : "") + " ";
            const int slen = std::min<int>(sl.size(), std::max(0, width_ - 3));
            if (slen > 0) put_text(y, 1, sl, slen, kFg, bg);
        }
    }

    // --- Hint row (height - 1) ---
    if (height_ >= 2) {
        const int y = height_ - 1;
        Glyph*    r = row_ptr(y);
        for (int x = 0; x < width_; x++)
            set_glyph(r[left_ + x], ' ', kFg, bg);
        const std::string_view hint =
            std::string_view{" Enter open  Arrows move "};
        put_text(y, 0, hint, std::min<int>(hint.size(), width_),
                 kFg, bg, ATTR_REVERSE);
    }
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
                selected_ = 0;
                viewport_ = 0;
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
    const int y0 = 0;              // panel top row
    // Return true if terminal repainted a covered row.
    return std::any_of(term_dirty, term_dirty + y0 + height_, 
        [](int d) { return d != 0; });
}

void Panel::draw()
{
    if (!visible()) return;
    render();          // no-op if dirty
    const int y0 = 0;  // panel top row
    for (int i = 0; i < height_; ++i)
        // Draw the corresponding segment of row_ptr(i) into columns [left_, term_cols_)
        // for each terminal row in the range [y0, y0 + height_).
        xdrawline(row_ptr(i), left_, y0 + i, term_cols_);
}

void Panel::toggle_panel()
{
    (void)0;
}

bool Panel::handle_key(unsigned long ksym, unsigned /*state*/, const char* buf, int len)
{
    if (!visible()) return false;
    const int list_rows = std::max(1, height_ - 3);
    const int n = static_cast<int>(entries_.size());

    // Note: Switch only expects non-printable keys.
    switch (ksym) {
        case XK_Up:
            --selected_;
            goto clamp_selected;
        case XK_Down:
            ++selected_;
            goto clamp_selected;
        case XK_Home:
            selected_ = 0;
            goto mark_dirty;
        case XK_End:
            selected_ = n - 1;
            goto clamp_selected;
        case XK_Page_Up:
            selected_ -= list_rows;
            goto clamp_selected;
        case XK_Page_Down:
            selected_ += list_rows;
            goto clamp_selected;

        clamp_selected:
            selected_ = clamp_between(selected_, 0, std::max(0, n - 1));
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

            const Entry& e = entries_[selected_];
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
