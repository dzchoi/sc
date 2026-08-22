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
#include <chrono>               // for std::chrono::steady_clock
#include <optional>             // for std::optional<>
#include <string>               // for std::string
#include <string_view>          // for std::string_view
#include <sys/types.h>          // for off_t, time_t
#include <utility>              // for std::move()
#include <vector>               // for std::vector<>

#include "canvas.hpp"           // for Canvas, Draw
#include "sc_config.hpp"        // for SC configuration constants

class Panel {
public:
    // A single directory entry as displayed in the panel.
    struct Entry {
        std::string name;
        bool    is_dir = false;
        off_t   size = 0;
        time_t  mtime = 0;

        Entry(std::string name, bool is_dir, off_t size, time_t mtime)
        : name(std::move(name)), is_dir(is_dir), size(size), mtime(mtime)
        {}
    };

    Panel() =default;
    Panel(const Panel&) =delete;
    Panel& operator=(const Panel&) =delete;
    Panel(Panel&&) =default;
    Panel& operator=(Panel&&) =default;

    // Reconciles the shell cwd and rebuilds its directory snapshot without consulting
    // terminal prompt state.
    void reload_panel(std::string cwd);

    // Returns the total prompt-owned padding needed to keep the prompt below the panel.
    int adjust_padding(int applied_padding) const;

    // Returns the active entry's name, if any. The view remains valid until the
    // directory snapshot is rebuilt.
    std::optional<std::string_view> selected_entry() const;

    // Snapshots whether this frame needs the overlay. term_dirty is the terminal's
    // mutable row-dirty array, before drawregion() clears it.
    void poll(int* term_dirty);
    void draw();

    void resize(int cols, int rows);
    void adjust_timeout(double& timeout_ms);

    void refresh_prompt();

    void toggle_panel();
    bool handle_key(unsigned long ksym, unsigned state, const char* buf, int len);

private:
    // Empty until resize() receives the terminal dimensions before the first frame.
    Canvas m_canvas;

    bool m_hidden = false;  // true: force-hidden regardless of shell ownership
    bool m_was_visible = false;  // visibility observed during the previous poll()
    bool m_dirty = false;   // true: render() rebuilds m_canvas's buffer before next draw.
    bool m_needs_draw = false;  // set by poll() before terminal rows are redrawn

    // Debounce geometry changes so ZLE redraws its prompt only after its final shape.
    static constexpr int kResizeSettleDelayMs = 150;
    std::optional<std::chrono::steady_clock::time_point> m_prompt_refresh_deadline;

    std::string m_cwd;             // current directory, always ending with '/'
    std::vector<Entry> m_entries;  // cached entries in m_cwd
    int m_selected_idx = 0;        // index into m_entries of the highlighted row
    int m_first_visible_idx = 0;   // index into m_entries of the first visible row

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

    void recompute_geometry(int cols, int rows);

    // Rebuilds m_entries[] from m_cwd and re-seats m_selected_idx when the absolute,
    // non-slash-terminated prev_path names an ordinary entry in the new snapshot.
    void load_entries(std::string_view prev_path);

    void render();
};
