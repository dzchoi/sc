// See LICENSE for license details.

#include <algorithm>            // for std::find_if(), std::sort()
#include <cassert>              // for assert()
#include <cerrno>               // for errno
#include <cstdint>              // for uint32_t
#include <cstdlib>              // for std::getenv()
#include <cstring>              // for std::strcmp(), std::strlen()
#include <ctime>                // for localtime_r(), std::strftime()
#include <string>               // for std::string, std::to_string(), ...
#include <utility>              // for std::move(), std::pair()

#include <dirent.h>             // for DIR, fdopendir(), readdir(), closedir()
#include <fcntl.h>              // for openat(), O_*, AT_SYMLINK_NOFOLLOW
#include <limits.h>             // for PATH_MAX
#include <sys/stat.h>           // for struct stat, fstat(), fstatat()
#include <sys/types.h>          // for off_t, time_t
#include <unistd.h>             // for close(), getpid(), readlink()
#include <X11/keysym.h>         // for XK_*

#include "comm.hpp"             // for Comm
#include "panel.hpp"            // for Panel
#include "sc_config.hpp"        // for SC configuration constants



PanelDirectory::PanelDirectory(std::string cwd, int fd)
: m_cwd(std::move(cwd)), m_fd(fd)
{
    assert( m_fd >= 0 );

    if ( !cwd_matches_inode() ) {
        char path[PATH_MAX];
        const ssize_t n = ::readlink(proc_path().c_str(), path, sizeof(path) - 1);
        if ( n <= 0 || static_cast<size_t>(n) == sizeof(path) - 1 ) {
            const int error = errno;
            ::close(m_fd);
            m_fd = -1;
            die("read panel directory path failed: %s\n",
                n < 0 ? std::strerror(error) : n == 0 ? "empty path" : "path too long");
        }
        m_cwd.assign(path, n);
    }
    if ( m_cwd.back() != '/' ) m_cwd.push_back('/');
}

PanelDirectory::PanelDirectory(PanelDirectory&& other) noexcept
: m_cwd(std::move(other.m_cwd)), m_fd(other.m_fd)
{
    other.m_fd = -1;
}

PanelDirectory& PanelDirectory::operator=(PanelDirectory&& other) noexcept
{
    if ( this == &other ) return *this;
    if ( m_fd >= 0 ) ::close(m_fd);
    m_cwd = std::move(other.m_cwd);
    m_fd = other.m_fd;
    other.m_fd = -1;
    return *this;
}

PanelDirectory::~PanelDirectory()
{
    if ( m_fd >= 0 ) ::close(m_fd);
}

PanelDirectory PanelDirectory::duplicate() const
{
    const int fd = ::fcntl(m_fd, F_DUPFD_CLOEXEC, 0);
    if ( fd < 0 )
        die("duplicate initial panel directory failed: %s\n", std::strerror(errno));
    return PanelDirectory{m_cwd, fd};
}

bool PanelDirectory::same_inode(const PanelDirectory& other) const
{
    struct stat mine{}, theirs{};
    const bool success = ::fstat(m_fd, &mine) == 0 && ::fstat(other.m_fd, &theirs) == 0;
    assert( success );
    return mine.st_dev == theirs.st_dev && mine.st_ino == theirs.st_ino;
}

bool PanelDirectory::cwd_matches_inode() const
{
    assert( !m_cwd.empty() && m_cwd.front() == '/' );
    struct stat path{}, retained{};
    return ::stat(m_cwd.c_str(), &path) == 0 && ::fstat(m_fd, &retained) == 0
        && path.st_dev == retained.st_dev && path.st_ino == retained.st_ino;
}

std::string PanelDirectory::proc_path() const
{
    return "/proc/" + std::to_string(::getpid()) + "/fd/" + std::to_string(m_fd);
}



template <typename T>
constexpr T clamp_between(T v, T lo, T hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Formats the Size column within kColsSize cells. Directories and symlinks use type
// markers; regular-file byte counts up to 1M are exact, while larger values use
// "DDDD.DM" (one truncated decimal digit, unit suffix M/G/T) with an integer part below
// 1024.
static std::string format_size(const Panel::Entry& entry)
{
    if ( entry.is_symlink() ) return "SYMLINK";
    if ( entry.is_directory() )
        return entry.name == ".." ? "UP--DIR" : "SUB-DIR";

    const off_t bytes = entry.size;
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
// (mtime < 0 or localtime_r() error).
static std::pair<std::string, std::string> format_mtime(time_t mtime)
{
    if ( mtime < 0 ) return {};  // mtime == 0 is Unix Epoch (1/1/1970, 00:00:00 UTC).
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

static char entry_indicator(Panel::Entry::Type type)
{
    using Type = Panel::Entry::Type;
    switch ( type ) {
        case Type::Directory:     return '/';
        case Type::SymlinkFile:   return '@';
        case Type::SymlinkDir:    return '>';
        case Type::BrokenSymlink: return '!';
        default:                  return 0;
    }
}

static Draw& draw_name(Draw& draw, const Panel::Entry& entry, int width,
    ushort mode = ATTR_NULL)
{
    draw.left(width).ellipsize(Draw::Keep::Both);
    if ( const char indicator = entry_indicator(entry.type) )
        return draw.put_with_suffix(entry.name, indicator,
            mode & ATTR_REVERSE ? kFgDefault : kFgIndicator, mode);
    return draw.put(entry.name, mode);
}



std::string Panel::directory_for_shell() const
{
    if ( m_directory.cwd_matches_inode() )
        return "L" + std::string(cwd());
    return "P" + m_directory.proc_path();
}

void Panel::set_geometry(int top, int left, int width, int height, int term_cols)
{
    // Comm gives both canvases the same top and height. Their disjoint left/width
    // ranges share Canvas's terminal-width line buffer.
    if ( height == 0 ) {
        assert( width == 0 );
        m_canvas.reset(0, 0, 0, 0, term_cols);
        return;
    }

    assert( top >= 0 && left >= 0 && width > 0 && height >= kMinRowsPanel );
    assert( left + width <= term_cols );
    compute_cols(width);

    m_canvas.reset(top, left, width, height, term_cols);
    m_dirty = true;
}

std::pair<bool, bool> Panel::set_visible(bool visible)
{
    const bool visibility_changed = (m_visible != visible);
    if ( visibility_changed ) m_dirty = true;
    m_visible = visible;
    return {visibility_changed, visible && m_dirty};
}

void Panel::render()
{
    if ( !m_visible ) return;
    if ( m_dirty ) {
        m_dirty = false;

        const int width  = m_canvas.width();
        const int height = m_canvas.height();
        const int list_rows = height - kRowsPanelFrame;  // excludes header and footer.
        const uint32_t fg = kFgDefault;
        const uint32_t bg = kBgDefault;
        const bool focused = Comm::is_focused(this);
        auto draw = std::move(m_canvas.draw());

        std::string title(cwd());
        if ( const char* home = std::getenv("HOME"); home && *home ) {
            const size_t home_len = std::strlen(home);
            if ( title.compare(0, home_len, home) == 0 && title[home_len] == '/' )
                title.replace(0, home_len, "~");
        }
        if ( m_listing_error ) {
            const char* marker;
            if ( m_listing_error < 0 )
                marker = " (incomplete)";
            else if ( m_listing_error == EACCES || m_listing_error == EPERM )
                marker = " (unreadable)";
            else
                marker = " (unavailable)";

            // PanelDirectory guarantees a slash-terminated display cwd. Keep that slash
            // last when adding status, including after procfs's " (deleted)" suffix.
            if ( title.size() > 1 )
                title.insert(title.size() - 1, marker);
            else
                title += marker;  // e.g. "/ (unavailable)"
        }

        // --- Row 0: top frame + title ---
        draw.move(0, 0).color(kFgFrame, bg).fill(kFrameOuterH)
            .move(0).put(kFrameOuterTL)
            .move(column.size_x - 1).put(kFrameOuterTT)
            .move(column.date_x - 1).put(kFrameOuterTT)
            .move(column.time_x - 1).put(kFrameOuterTT)
            .move(width - 1).put(kFrameOuterTR)
            // Keep one title-colored space adjacent to each end even when the path
            // consumes all remaining cells and must be ellipsized.
            .move(1).mid(width - 2).pad(1, 1).ellipsize(Draw::Keep::Right)
                .color(focused ? kFgFrame : fg)
                .put(title, focused ? ATTR_REVERSE : 0);

        // --- Row 1: column headers ---
        draw.move(0, 1).color(kFgFrame, bg).fill(' ')
            .move(0).put(kFrameOuterV)
            .left(column.name_w)
            .with_fg(kFgSelected, [](Draw& d){ d.put("Name", ATTR_BOLD); })
            .put(kFrameInnerV)
            .right(column.size_w)
            .with_fg(kFgSelected, [](Draw& d){ d.put("Size", ATTR_BOLD); })
            .put(kFrameInnerV)
            .mid(column.date_w)
            .with_fg(kFgSelected, [](Draw& d){ d.put("Date", ATTR_BOLD); })
            .put(kFrameInnerV)
            .mid(column.time_w)
            .with_fg(kFgSelected, [](Draw& d){ d.put("Time", ATTR_BOLD); })
            .put(kFrameOuterV);

        // Keep the selection in view.
        if ( m_selected_idx < m_first_visible_idx )
            m_first_visible_idx = m_selected_idx;
        if ( m_selected_idx >= m_first_visible_idx + list_rows )
            m_first_visible_idx = m_selected_idx - list_rows + 1;

        // Avoid blank rows when enough entries exist to fill the viewport.
        m_first_visible_idx = std::min(
            m_first_visible_idx,
            std::max(0, static_cast<int>(m_entries.size()) - list_rows));

        // --- Rows 2 .. height-4: entries ---
        for ( int i = 0 ; i < list_rows ; ++i ) {
            const int y = 2 + i;  // skip over the header.
            const int idx = m_first_visible_idx + i;

            if ( idx >= static_cast<int>(m_entries.size()) ) {
                draw.move(0, y).color(fg, bg).fill(' ')  // Clear the line first.
                    .move(0).color(kFgFrame).put(kFrameOuterV)
                    .move(column.size_x - 1).put(kFrameInnerV)
                    .move(column.date_x - 1).put(kFrameInnerV)
                    .move(column.time_x - 1).put(kFrameInnerV)
                    .move(width - 1).put(kFrameOuterV);
                continue;
            }

            const bool selected = (idx == m_selected_idx);
            const bool focused_selection = selected && focused;
            const ushort mode = focused_selection
                ? ATTR_REVERSE | ATTR_CLEAR_FIELD : ATTR_CLEAR_FIELD;
            // The selected row's frame follows its text colour; other frames stay dim.
            const uint32_t m_fgtext  = fg;
            const uint32_t m_fgframe = focused_selection ? fg : kFgFrame;

            const Entry& e = m_entries[idx];
            auto [date, time] = format_mtime(e.mtime);

            // Draw row frame.
            draw.move(0, y)
                .color(kFgFrame, bg).put(kFrameOuterV).color(m_fgtext);

            // Name column (abbreviated to fit or left-aligned)
            draw_name(draw, e, column.name_w, mode)
                .with_fg(m_fgframe, [&](Draw& d){ d.put(kFrameInnerV, mode); })

                // Size column (right-aligned)
                .right(column.size_w).put(format_size(e), mode)
                .with_fg(m_fgframe, [&](Draw& d){ d.put(kFrameInnerV, mode); })

                // Date column
                .right(column.date_w).put(date, mode)
                .with_fg(m_fgframe, [&](Draw& d){ d.put(kFrameInnerV, mode); })

                // Time column
                .right(column.time_w).put(time, mode)
                .color(kFgFrame).put(kFrameOuterV);
        }

        // Row -3: separator frame
        draw.move(0, height - 3).color(kFgFrame, bg).fill(kFrameInnerH)
            .move(0).put(kFrameOuterLT)
            .move(column.size_x - 1).put(kFrameInnerBT)
            .move(column.date_x - 1).put(kFrameInnerBT)
            .move(column.time_x - 1).put(kFrameInnerBT)
            .move(width - 1).put(kFrameOuterRT);

        // Row -2: selected entry
        const Entry& e = m_entries[m_selected_idx];
        auto [date, time] = format_mtime(e.mtime);
        draw.move(0, height - 2).color(fg, bg).fill(' ')
            .move(0).with_fg(kFgFrame, [](Draw& d){ d.put(kFrameOuterV); });

        // Name column (abbreviated to fit or left-aligned)
        draw_name(draw, e, column.name_w)

            // Size column (right-aligned)
            .move(column.size_x)
            .right(column.size_w).put(format_size(e))

            // Date column
            .move(column.date_x)
            .right(column.date_w).put(date)

            // Time column
            .move(column.time_x)
            .right(column.time_w).put(time)
            .color(kFgFrame).put(kFrameOuterV);

        // Row -1: bottom frame
        draw.move(0, height - 1).color(kFgFrame, bg).fill(kFrameOuterH)
            .move(0).put(kFrameOuterBL)
            .move(width - 1).put(kFrameOuterBR);
    }

    m_canvas.present();
}

static Panel::Entry load_entry(int fd, const char* name)
{
    using Entry = Panel::Entry;
    using Type = Entry::Type;

    struct stat st{};
    if ( ::fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0 )
        // mtime == -1 shows empty Date and Time.
        return Entry{name, Type::File, 0, -1};

    Type type;
    if ( S_ISDIR(st.st_mode) )
        type = Type::Directory;
    else if ( S_ISLNK(st.st_mode) ) {
        struct stat target;
        if ( ::fstatat(fd, name, &target, 0) != 0 )
            type = Type::BrokenSymlink;
        else
            type = S_ISDIR(target.st_mode) ? Type::SymlinkDir : Type::SymlinkFile;
    }
    else
        type = Type::File;

    return Entry{name, type, st.st_size, st.st_mtime};
}

void Panel::load_entries(std::string_view prev_path)
{
    m_entries.clear();
    m_listing_error = 0;

    // Manually add ".." as the first entry in case the filesystem's readdir() does not
    // enumerate it. (mtime == -1 shows empty Date and Time.)
    m_entries.emplace_back("..", Entry::Type::Directory, 0, -1);

    const int scan_fd = ::openat(m_directory.fd(), ".",
        O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if ( scan_fd >= 0 ) {
        if ( DIR* dir = ::fdopendir(scan_fd) ) {
            // readdir() returns null for both EOF and failure; errno distinguishes them.
            while ( auto* const dirent = (errno = 0, ::readdir(dir)) ) {
                if ( std::strcmp(dirent->d_name, ".")  == 0 ) continue;
                if ( std::strcmp(dirent->d_name, "..") == 0 ) continue;
                m_entries.push_back(load_entry(m_directory.fd(), dirent->d_name));
            }
            m_listing_error = -errno;
            ::closedir(dir);  // closes both dir and scan_fd.
        }
        else {
            m_listing_error = errno;
            ::close(scan_fd);
        }
    }
    else
        m_listing_error = errno;

    // ".." is already first; order the remaining snapshot as directories, then files.
    std::sort(m_entries.begin() + 1, m_entries.end(),
        [](const Entry& a, const Entry& b) {
            if ( a.is_directory() != b.is_directory() ) return a.is_directory();
            return a.name < b.name;
        });

    // A missing slash yields name_pos == 0, which cannot match nonempty cwd().
    const size_t name_pos = prev_path.rfind('/') + 1;
    if ( cwd().size() == name_pos && prev_path.substr(0, name_pos) == cwd() ) {
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

void Panel::init(PanelDirectory directory)
{
    assert( !m_directory.valid() && directory.valid() );
    m_directory = std::move(directory);
    load_entries({});
}

void Panel::reload(PanelDirectory directory)
{
    assert( m_directory.valid() && directory.valid() );
    const bool directory_changed = !m_directory.same_inode(directory);
    std::string prev_path;
    if ( directory_changed ) {
        // Preserve the directory being left when its absolute path names an ordinary
        // entry in the new cwd. Descending retains the index-zero ".." default.
        // Root (/) cannot be an ordinary entry; other cwd paths omit their trailing /.
        if ( cwd().size() > 1 ) {
            prev_path = cwd();
            prev_path.pop_back();
        }
        m_selected_idx = 0;
    }
    else
        prev_path = std::string(directory.cwd()) + m_entries[m_selected_idx].name;

    m_directory = std::move(directory);
    load_entries(prev_path);

    if ( directory_changed ) {
        // Reposition the viewport so that selected entry is roughly centered.
        const int list_rows = m_canvas.height() - kRowsPanelFrame;
        m_first_visible_idx = std::max(0, m_selected_idx - list_rows / 2);
    }
}

bool Panel::handle_key(unsigned long ksym)
{
    assert( m_selected_idx >= 0 );
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
            m_selected_idx -= list_rows;
            goto clamp_cursor;
        case XK_Page_Down:
            m_selected_idx += list_rows;
            goto clamp_cursor;

        clamp_cursor:
            m_selected_idx = clamp_between(m_selected_idx, 0, n - 1);
            if ( old_selected_idx != m_selected_idx ) {
                m_dirty = true;
                ::draw();
            }
            return true;

        default:
            break;
    }

    return false;
}
