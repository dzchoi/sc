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

#include <cassert>              // for assert()
#include <string>               // for std::string
#include <sys/types.h>          // for off_t, pid_t, time_t
#include <vector>               // for std::vector<>

#include "canvas.hpp"           // for Canvas, Draw
#include "ipc.hpp"              // for Ipc
#include "sc_config.hpp"        // for SC configuration constants

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

    // Ipc owns the control socket's creation, lifetime, and request protocol.
    // These methods expose it through Panel for the C ABI.
    static const char* preinit() { return m_ipc.init(); }
    static int ipc_fd() { return m_ipc.fd(); }
    static void cleanup_ipc() noexcept { m_ipc.cleanup(); }
    void service_ipc() { m_ipc.service(*this); }

    // Queries used during service_ipc() execution.
    int prompt_padding(int applied_padding) const;
    const Entry* selected_entry() const;

    void init(int pty_fd, pid_t shell_pid);

    bool poll();
    bool needs_draw(const int* term_dirty) const;
    void draw();

    void resize(int cols, int rows);
    static void set_cursor(int y) { cursor_y_ = y; }

    void notify_zsh_ready() { zsh_ready_ = true; }
    void notify_cwd_changed() { cwd_changed_ = true; }
    void refresh_prompt();

    void toggle_panel();
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
    inline static int cursor_y_ = 0;
    inline static Ipc m_ipc;

    // ----- panel geometry -----
    Canvas canvas_;  // recompute_geometry() only resets its size.

    // ----- panel state -----
    bool shell_owns_tty_ = false;
    bool hidden_ = false;  // true: force-hidden regardless of shell ownership
    bool zsh_ready_ = false;
    bool cwd_changed_ = false;
    bool dirty_ = false;   // true: render() rebuilds canvas_'s buffer before next draw.

    std::string cwd_;             // the current working directory
    std::vector<Entry> entries_;  // cache of the entries in cwd_, always ends with '/'
    int selected_idx_ = 0;        // index into entries_ of the highlighted row
    int first_visible_idx_ = 0;   // index into entries_ of the first visible row

    bool visible() const;

    // Column X positions inside the panel (panel-local, 0 .. width-1).
    // Row layout:  | Name... | Size | Date | Time |
    struct Cols {
        static constexpr int name_x = 1;  // just after left frame
        int name_w;
        int size_x;
        static constexpr int size_w = kColsSize;
        int date_x;
        static constexpr int date_w = kColsDate;
        int time_x;
        static constexpr int time_w = kColsTime;
    } column;

    // Compute column X positions given the panel width. Assumes
    // width >= kMinCols/kFracWidth.
    void compute_cols(int width)
    {
        column.time_x = width - 1 - kColsTime;  // just before right frame
        column.date_x = column.time_x - 1 - kColsDate;
        column.size_x = column.date_x - 1 - kColsSize;
        column.name_w = column.size_x - 1 - column.name_x;  // shrinks/grows with width
        assert( column.name_w > 0 );
    }

    void recompute_geometry();

    // Rebuilds entries_[] from cwd_. prev_dir_stat is the previous directory's stat,
    // used to re-locate that directory among the new entries (e.g. ".." after
    // descending, or the subdir just left after ascending) and re-seat selected_idx_ on
    // it.
    void load_entries(const struct stat& prev_dir_stat);

    // Returns the shell's cwd via /proc/<shell_pid>/cwd, or an empty string on failure.
    static std::string shell_cwd();

    void render();
};
