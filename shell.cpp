// See LICENSE for license details.

#include <algorithm>            // for std::max()
#include <cassert>              // for assert()
#include <cerrno>               // for errno, EINTR
#include <charconv>             // for std::from_chars()
#include <chrono>               // for std::chrono::steady_clock
#include <cstdint>              // for int64_t
#include <cstdlib>              // for std::getenv(), mkdtemp(), setenv()
#include <cstring>              // for std::memcpy(), std::strcmp(), ...
#include <cstdio>               // for std::snprintf()
#include <limits.h>             // for PATH_MAX
#include <poll.h>               // for poll(), pollfd, POLLIN, ...
#include <string>               // for std::string, std::to_string()
#include <sys/socket.h>         // for accept4(), bind(), listen(), socket()
#include <sys/stat.h>           // for chmod()
#include <unistd.h>             // for close(), getpid(), readlink(), ...

#include "comm.hpp"             // for Comm
#include "shell.hpp"            // for Shell

extern "C" {
#include "st.h"                 // for die(), ttyread(), ttywrite()
}



void Shell::preinit()
{
    if ( m_ipc_fd >= 0 ) return;

    m_owner = ::getpid();

    const auto create_directory = [this](const char* base) {
        size_t base_length = std::strlen(base);
        const size_t separator_length = base[base_length - 1] != '/';
        if ( sizeof(m_directory) < base_length + separator_length
          + sizeof(kDirectoryName) + sizeof(kSocketName) - 1 )
            return false;

        std::memcpy(m_directory, base, base_length);
        if ( separator_length ) m_directory[base_length++] = '/';
        std::memcpy(m_directory + base_length, kDirectoryName, sizeof(kDirectoryName));
        return ::mkdtemp(m_directory) != nullptr;
    };

    const char* runtime_dir = std::getenv("XDG_RUNTIME_DIR");  // "/run/user/$UID/"
    const bool created_in_runtime = runtime_dir && create_directory(runtime_dir);
    if ( !created_in_runtime && !create_directory(kTmpDirectory) ) {
        const int error = errno;
        m_directory[0] = '\0';
        die("mkdtemp for SC control socket failed: %s\n", std::strerror(error));
    }

    m_ipc_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if ( m_ipc_fd < 0 ) {
        const int error = errno;
        cleanup();
        die("create SC control socket failed: %s\n", std::strerror(error));
    }

    m_address.sun_family = AF_UNIX;
    const size_t directory_length = std::strlen(m_directory);
    std::memcpy(m_address.sun_path, m_directory, directory_length);
    std::memcpy(m_address.sun_path + directory_length, kSocketName, sizeof(kSocketName));

    if ( ::bind(m_ipc_fd, reinterpret_cast<sockaddr*>(&m_address), sizeof(m_address)) >= 0
      && ::listen(m_ipc_fd, 4) >= 0 && ::chmod(m_address.sun_path, 0600) >= 0 ) {
        ::setenv("SC_SOCKET", m_address.sun_path, 1);
        return;
    }

    const int error = errno;
    cleanup();
    die("initialize SC control socket failed: %s\n", std::strerror(error));
}

void Shell::init(int pty_fd, pid_t shell_pid)
{
    m_pty_fd = pty_fd;
    m_pid = shell_pid;

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kFirstPrepromptTimeoutMs);
    struct pollfd fds[] = {
        {m_pty_fd, POLLIN, 0},
        {m_ipc_fd, POLLIN, 0},
    };

    // Wait up to kFirstPrepromptTimeoutMs for zsh's first preprompt request while
    // processing any startup output received through the PTY.
    while ( !m_preprompt_requested ) {
        const auto now = std::chrono::steady_clock::now();
        if ( now >= deadline )
            die("SC did not receive the first preprompt request within %d ms; "
                "source sc.zsh from ~/.zshrc\n", kFirstPrepromptTimeoutMs);

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count();
        const int timeout_ms = static_cast<int>(std::max<int64_t>(1, remaining));
        fds[0].revents = fds[1].revents = 0;
        const int result = ::poll(fds, 2, timeout_ms);
        if ( result < 0 ) {
            if ( errno == EINTR ) continue;
            die("waiting for SC's first preprompt failed: %s\n", std::strerror(errno));
        }
        if ( result == 0 ) continue;
        assert( (fds[0].revents & POLLNVAL) == 0 && (fds[1].revents & POLLNVAL) == 0 );

        // Process shell startup output before adjust_padding() reads the terminal
        // cursor.
        if ( fds[0].revents & (POLLIN | POLLERR | POLLHUP) ) ttyread();
        if ( fds[1].revents & POLLIN ) service_ipc();
    }
}

void Shell::service_ipc()
{
    const int client = ::accept4(m_ipc_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if ( client < 0 ) return;

    char request[32]{};
    std::string reply;
    if ( const ssize_t n = ::read(client, request, sizeof(request) - 1); n > 0 ) {
        if ( std::strcmp(request, "selected\n") == 0 ) {
            assert( m_preprompt_requested );
            if ( const auto entry = Comm::selected_entry() )
                reply = std::string(*entry) + "\n";
        }

        else if ( std::strcmp(request, "focused_cwd\n") == 0 ) {
            assert( m_preprompt_requested );
            reply = std::string(Comm::focused_cwd()) + "\n";
        }

        else if ( std::strcmp(request, "reload\n") == 0 ) {
            Comm::reload_panels(get_cwd(), !m_preprompt_requested);
            reply = "\n";
        }

        else if ( std::strncmp(request, "preprompt ", 10) == 0
          && request[n - 1] == '\n' ) {
            int applied_padding;
            const char* first = request + 10;  // 10 == strlen("preprompt ")
            const char* last = request + n - 1;
            const auto result = std::from_chars(first, last, applied_padding);
            if ( result.ec == std::errc{} && result.ptr == last && applied_padding >= 0 ) {
                // A preprompt transaction refreshes panel data before calculating
                // placement from the terminal's prompt cursor.
                Comm::reload_panels(get_cwd(), !m_preprompt_requested);
                reply = std::to_string(Comm::adjust_padding(applied_padding)) + "\n";
                m_preprompt_requested = true;
            }
        }
    }

    for ( size_t n = 0 ; n < reply.size() ; ) {
        const ssize_t written = ::send(client,
            reply.data() + n, reply.size() - n, MSG_NOSIGNAL);
        if ( written <= 0 ) break;
        n += static_cast<size_t>(written);
    }
    ::close(client);
}

void Shell::cleanup() noexcept
{
    if ( m_owner != ::getpid() ) return;

    const int fd = m_ipc_fd;
    m_ipc_fd = -1;
    if ( fd >= 0 ) ::close(fd);

    if ( m_address.sun_path[0] ) {
        ::unlink(m_address.sun_path);
        m_address.sun_path[0] = '\0';
    }
    if ( m_directory[0] ) {
        ::rmdir(m_directory);
        m_directory[0] = '\0';
    }
}

std::string Shell::get_cwd() const
{
    char proc[32];
    std::snprintf(proc, sizeof(proc), "/proc/%d/cwd", static_cast<int>(m_pid));
    char path[PATH_MAX];
    const ssize_t n = ::readlink(proc, path, sizeof(path) - 1);
    if ( n <= 0 )
        die("read shell cwd failed: %s\n", n < 0 ? std::strerror(errno) : "empty path");
    std::string cwd(path, n);
    if ( cwd.back() != '/' ) cwd.push_back('/');
    return cwd;
}

void Shell::send_event(ZleEvent event) const
{
    constexpr const char* sequences[] = {
        "\033[6770~",  // CdParent
        "\033[6771~",  // CdChild
        "\033[6772~",  // InsertName
        "\033[6773~",  // InsertPath
        "\033[6774~",  // RefreshPrompt
        "\033[6775~",  // SwitchPanel
    };
    const char* sequence = sequences[static_cast<unsigned>(event)];
    ttywrite(sequence, std::strlen(sequence), 1);
}
