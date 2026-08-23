// See LICENSE for license details.

#pragma once

#include <cassert>              // for assert()
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

    // Canvas geometry and backing storage used to present this panel.
    const Canvas& canvas() const { return m_canvas; }

    std::string_view cwd() const { return m_cwd; }

    // Comm supplies disjoint horizontal ranges and guarantees that both panels receive
    // the same top and height. Shared vertical geometry lets Comm invalidate and test
    // their covered terminal rows once per frame.
    void set_geometry(int top, int left, int width, int height, int term_cols);

    // Records effective visibility and returns {visibility changed, needs render}.
    // Comm separately invalidates terminal rows after polling both panels.
    std::pair<bool, bool> set_visible(bool visible);

    // Marks the cached canvas for rebuilding before its next presentation.
    void dirty() { m_dirty = true; }

    // Returns the selected snapshot name. The panel initialization boundary guarantees
    // that m_entries is nonempty and m_selected_idx is valid.
    std::string_view selected_entry() const { return m_entries[m_selected_idx].name; }

    // Rebuilds dirty content, then presents the visible canvas.
    void render();

    // Reconciles the shell cwd and rebuilds its directory snapshot without consulting
    // terminal prompt state.
    void reload(std::string cwd);

    // Handles input after Comm has established that the focused panel is visible.
    bool handle_key(unsigned long ksym);

private:
    // Empty until resize_panels() receives terminal dimensions before the first frame.
    Canvas m_canvas;

    bool m_visible = false;  // effective visibility captured by set_visible()
    bool m_dirty = false;   // true: render() rebuilds m_canvas's buffer before next draw.

    std::string m_cwd;  // empty before initialization; otherwise always ends with '/'
    // Empty before initialization; afterward always contains at least synthetic "..".
    std::vector<Entry> m_entries;
    int m_selected_idx = 0;        // index into m_entries of the highlighted row
    int m_first_visible_idx = 0;   // index into m_entries of the first visible row

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

    // Rebuilds m_entries[] from m_cwd and re-seats m_selected_idx when the absolute,
    // non-slash-terminated prev_path names an ordinary entry in the new snapshot.
    void load_entries(std::string_view prev_path);
};
