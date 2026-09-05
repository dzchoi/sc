// See LICENSE for license details.

#pragma once

#include <cassert>              // for assert()
#include <string>               // for std::string
#include <string_view>          // for std::string_view
#include <utility>              // for std::move()
#include <vector>               // for std::vector<>
#include <sys/types.h>          // for off_t, time_t

#include "canvas.hpp"           // for Canvas, Draw
#include "sc_config.hpp"        // for SC configuration constants



// Move-only handle and display path for a panel directory. m_fd pins the directory
// inode; m_cwd preserves a validated logical path or stores its procfs-derived fallback.
class PanelDirectory {
public:
    PanelDirectory() =default;
    PanelDirectory(std::string cwd, int fd);
    PanelDirectory(const PanelDirectory&) =delete;
    PanelDirectory& operator=(const PanelDirectory&) =delete;
    PanelDirectory(PanelDirectory&& other) noexcept;
    PanelDirectory& operator=(PanelDirectory&& other) noexcept;
    ~PanelDirectory();

    bool valid() const { return m_fd >= 0; }
    int fd() const { return m_fd; }
    std::string_view cwd() const { return m_cwd; }

    // Returns another descriptor for the same inode, or terminates SC on failure.
    PanelDirectory duplicate() const;
    // Returns whether both initialized handles identify the same directory inode.
    bool same_inode(const PanelDirectory& other) const;
    // Returns whether the display cwd still resolves to the retained inode.
    bool cwd_matches_inode() const;
    // Returns "/proc/<sc-pid>/fd/<fd>", which resolves to this retained directory.
    std::string proc_path() const;

private:
    std::string m_cwd;
    int m_fd = -1;
};



class Panel {
public:
    // A single directory entry as displayed in the panel.
    struct Entry {
        enum class Type { File, Directory, SymlinkFile, SymlinkDir, BrokenSymlink };

        std::string name;
        Type    type = Type::File;
        off_t   size = 0;
        time_t  mtime = 0;

        Entry(std::string name, Type type, off_t size, time_t mtime)
        : name(std::move(name)), type(type), size(size), mtime(mtime)
        {}

        bool is_directory() const {
            return type == Type::Directory || type == Type::SymlinkDir;
        }

        bool is_symlink() const {
            return type == Type::SymlinkFile || type == Type::SymlinkDir
                || type == Type::BrokenSymlink;
        }
    };

    Panel() =default;
    Panel(const Panel&) =delete;
    Panel& operator=(const Panel&) =delete;
    Panel(Panel&&) =delete;
    Panel& operator=(Panel&&) =delete;

    // Canvas geometry and backing storage used to present this panel.
    const Canvas& canvas() const { return m_canvas; }

    std::string_view cwd() const { return m_directory.cwd(); }

    // Prefixes the shell path with L for a valid logical cwd or P for the procfs
    // fallback, allowing Zsh to choose logical or physical cd semantics.
    std::string directory_for_shell() const;

    // Comm supplies disjoint horizontal ranges and guarantees that both panels receive
    // the same top and height. Shared vertical geometry lets Comm invalidate and test
    // their covered terminal rows once per frame.
    // Note that set_geometry() can be called before init(); tnew() calls panel_resize()
    // before ttynew() starts the shell and receives the first preprompt.
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

    // Establishes the panel directory and first nonempty snapshot at the first preprompt.
    void init(PanelDirectory directory);

    // Replaces the initialized directory and rebuilds its snapshot without consulting
    // terminal prompt state.
    void reload(PanelDirectory directory);

    // Handles input after Comm has established that the focused panel is visible.
    bool handle_key(unsigned long ksym);

private:
    // Empty until resize_panels() receives terminal dimensions before the first frame.
    Canvas m_canvas;

    bool m_visible = false;  // effective visibility captured by set_visible()
    bool m_dirty = false;   // true: render() rebuilds m_canvas's buffer before next draw.

    // Invalid before init(); afterward owns a descriptor that pins the directory inode
    // and a slash-terminated display path.
    PanelDirectory m_directory;
    // Empty before initialization; afterward always contains at least synthetic "..".
    std::vector<Entry> m_entries;
    // 0: complete; +errno: unavailable; -errno: incomplete.
    int m_listing_error = 0;
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

    // Rebuilds m_entries[] from cwd() and re-seats m_selected_idx when the absolute,
    // non-slash-terminated prev_path names an ordinary entry in the new snapshot.
    void load_entries(std::string_view prev_path);
};
