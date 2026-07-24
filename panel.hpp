// See LICENSE for license details.
//
// Public C++ interface for the file-manager panel.
//
// [panel.h] is the C ABI shim used by st.c/x.c. If you're calling from C, use that.
// This header is the C++ interface for callers that want to hold or subclass a Panel
// object directly (e.g. a future dual-pane setup that instantiates two panels).
//
// A single Panel owns:
//   - its geometry within the terminal grid (top-right by default),
//   - a snapshot of the current working directory's entries,
//   - a scratch line buffer used for rendering.
//
// It never touches term.line; it only produces synthesized Line rows via row_glyphs()
// for the caller to hand to xdrawline().

#pragma once

#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include "canvas.hpp"

struct stat;



class Panel {
public:
    // A single directory entry as displayed in the panel.
    struct Entry {
        std::string name;
        bool    is_dir = false;
        off_t   size = 0;
        time_t  mtime = 0;
    };

    Panel();
    Panel(const Panel&) =delete;
    Panel& operator=(const Panel&) =delete;
    Panel(Panel&&) =default;
    Panel& operator=(Panel&&) =default;

    // Geometry change from the terminal (called on tresize()).
    void resize(int cols, int rows);

    // Called once from ttynew() to capture the master PTY fd and shell PID after the
    // shell is forked.
    static void init(int pty_fd, pid_t shell_pid);

    // ----- Draw lifecycle (call in this order once per frame) -----

    // Refresh auto-visibility from the PTY's foreground process group.
    // Returns true if visibility just changed and the caller must force all rows dirty
    // for the terminal underneath to be repainted.
    bool poll();

    // True if the overlay must be repainted this frame: either our content changed, or
    // the terminal has dirtied rows we cover. term_dirty is term.dirty; must be read
    // BEFORE drawregion() clears the flags.
    bool needs_draw(const int* term_dirty) const;

    // Paint the panel overlay via xdrawline(). No-op if not visible.
    void draw();

    // ----- User input -----

    // User hit Ctrl+O.
    void toggle_panel();

    // Route a key press. Returns true if the panel consumed it. Returns false for keys
    // that should flow to the shell.
    bool handle_key(unsigned long ksym, unsigned state, const char* buf, int len);

private:
    // ----- geometry defaults -----
    static constexpr int kHeightFrac = 2;   // panel takes 1/kHeightFrac of terminal rows
    static constexpr int kMinRows    = 12;  // minimum terminal rows to show the panel
    static constexpr int kWidthFrac  = 2;   // panel takes 1/kWidthFrac of terminal cols
    static constexpr int kMinCols    = 80;  // minimum terminal cols to show the panel
    static constexpr int kFrameRows  = 5;   // header (2) + footer (3)

    // ----- NC-style column layout -----
    // The row structure inside the frames is:  | Name | Size | Date | Time |
    static constexpr int kColSize = 7;  // "1048576" / "1023.9M" / "SUB-DIR"
    static constexpr int kColDate = 8;  // "MM/DD/YY"
    static constexpr int kColTime = 6;  // "HH:MMp"

    // Required by format_size() (panel.cpp).
    // 7 = max(digits(1024*1024) == 7, digits(1023) + strlen(".9M") == 7)
    static_assert(kColSize >= 7);

    // ----- terminal geometry -----
    // Terminal dimensions. Placeholder values used during static construction;
    // `st` calls resize() from tresize() before the first frame is rendered.
    inline static int term_cols_ = 80;
    inline static int term_rows_ = 24;

    // ----- shell state -----
    // Set once from init() after the shell is forked; never change thereafter.
    inline static int pty_fd_ = -1;
    inline static pid_t shell_pid_ = 0;
    inline static bool typed_since_prompt_ = false;  // true while the shell command line is non-empty.

    // ----- panel geometry -----
    Canvas canvas_;  // recompute_geometry() only resets its size.

    // ----- panel state -----
    bool visible_ = false;
    bool hidden_ = false;  // true: force-hidden regardless of visible_
    bool dirty_ = false;   // true: render() rebuilds canvas_'s buffer before next draw.

    // ----- data -----
    std::string cwd_;             // the current working directory
    std::vector<Entry> entries_;  // cache of the entries in cwd_
    int cursor_idx_ = 0;          // index into entries_ of the highlighted row
    int scroll_idx_ = 0;          // index into entries_ of the first visible row

    // ----- queries (internal) -----
    bool visible() const {
        return visible_ && !hidden_ && canvas_.width() > 0 && canvas_.height() > 0;
    }

    // ----- geometry helpers -----
    // Column X positions inside the panel (panel-local, 0 .. width-1).
    // Row layout:  | Name... | Size | Date | Time |
    struct Cols {
        static constexpr int name_x = 1;  // just after left frame
        int name_w;
        int size_x;
        static constexpr int size_w = kColSize;
        int date_x;
        static constexpr int date_w = kColDate;
        int time_x;
        static constexpr int time_w = kColTime;
    } column;

    // Compute column X positions given the panel width. Assumes
    // width >= kMinCols/kWidthFrac.
    void compute_cols(int width)
    {
        column.time_x = width - 1 - kColTime;  // just before right frame
        column.date_x = column.time_x - 1 - kColDate;
        column.size_x = column.date_x - 1 - kColSize;
        column.name_w = column.size_x - 1 - column.name_x;  // shrinks/grows with width
    }

    void recompute_geometry();

    // ----- data helpers -----
    // Rebuilds entries_[] from cwd_. pst is the previous directory's stat, used to
    // re-locate that directory among the new entries (e.g. ".." after descending, or
    // the subdir just left after ascending) and re-seat cursor_idx_ on it.
    void load_entries(const struct stat& pst);

    // Returns the shell's cwd via /proc/<shell_pid>/cwd, or an empty string on failure.
    static std::string shell_cwd();

    void render();
};
