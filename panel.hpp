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

extern "C" {
#include "st.h"         // Line, Glyph, Rune, ushort, ATTR_*
}

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

    // User hit Ctrl+O (Todo: toggle panel on/off).
    void toggle_panel();

    // Route a key press. Returns true if the panel consumed it. Returns false for keys
    // that should flow to the shell.
    bool handle_key(unsigned long ksym, unsigned state, const char* buf, int len);

private:
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
    // Panel dimensions/position, derived from term_cols_/term_rows_ in
    // recompute_geometry().
    int height_ = 0, width_ = 0, left_ = 0;

    // ----- panel state -----
    bool visible_ = false;
    bool dirty_   = false;  // true ==> render() must rebuild linebuf_ before next draw.

    // ----- data -----
    std::string cwd_;             // the current working directory
    std::vector<Entry> entries_;  // cache of the entries in cwd_
    std::vector<Glyph> linebuf_;  // height_ * term_cols_ glyphs
    int selected_ = 0;            // highlighted entry index in entries_
    int viewport_ = 0;            // index of the first visible entry in entries_

    // ----- queries (internal) -----
    bool visible() const { return visible_ && height_ > 0 && width_ > 0; }

    // ----- geometry helpers -----
    void recompute_geometry();

    // ----- data helpers -----
    void load_entries();
    // Returns the shell's cwd via /proc/<shell_pid>/cwd, or an empty string on failure.
    static std::string shell_cwd();

    // ----- rendering primitives (panel-local coords 0..width_-1) -----
    Glyph* row_ptr(int y) { return linebuf_.data() + y * term_cols_; }
    void clear_row(int y, uint32_t fg, uint32_t bg);
    void put_text(int y, int col, std::string_view s, int max_len,
                  uint32_t fg, uint32_t bg, ushort mode = ATTR_NULL);
    void render();
};
