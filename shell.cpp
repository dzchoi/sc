// See LICENSE for license details.

#include <algorithm>            // for std::max()
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
#include <sys/un.h>             // for sockaddr_un
#include <unistd.h>             // for close(), getpid(), readlink(), ...

#include "panel.hpp"            // for Panel
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
        + std::chrono::milliseconds(kZshReadyTimeoutMs);
    struct pollfd pfd{m_pty_fd, POLLIN, 0};
    while ( !m_zsh_ready ) {
        const auto now = std::chrono::steady_clock::now();
        if ( now >= deadline )
            die("SC zsh adapter did not report readiness within %d ms; "
                "source sc.zsh from ~/.zshrc\n", kZshReadyTimeoutMs);

        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count();
        const int timeout_ms = static_cast<int>(std::max<int64_t>(1, remaining));
        pfd.revents = 0;
        const int result = ::poll(&pfd, 1, timeout_ms);
        if ( result < 0 ) {
            if ( errno == EINTR ) continue;
            die("waiting for SC zsh adapter failed: %s\n", std::strerror(errno));
        }
        if ( result == 0 ) continue;
        if ( pfd.revents & POLLNVAL )
            die("waiting for SC zsh adapter failed: invalid PTY descriptor\n");
        // Preserve all startup output and let st's parser recognize OSC 6770.
        if ( pfd.revents & (POLLIN | POLLERR | POLLHUP) ) ttyread();
    }
}

void Shell::service_ipc(const Panel& panel) const
{
    const int client = ::accept4(m_ipc_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if ( client < 0 ) return;

    char request[32]{};
    std::string reply = "E\n";
    if ( const ssize_t n = ::read(client, request, sizeof(request) - 1); n > 0 ) {
        if ( std::strcmp(request, "selected\n") == 0 ) {
            if ( const Panel::Entry* entry = panel.selected_entry() )
                reply = std::string(entry->is_dir ? "D\t" : "F\t") + entry->name + "\n";
            else
                reply = "N\n";
        }

        else if ( std::strncmp(request, "padding ", 8) == 0 && request[n - 1] == '\n' ) {
            int applied_padding;
            const char* first = request + 8;
            const char* last = request + n - 1;
            const auto result = std::from_chars(first, last, applied_padding);
            if ( result.ec == std::errc{} && result.ptr == last && applied_padding >= 0 )
                reply = "P\t" + std::to_string(panel.prompt_padding(applied_padding)) + "\n";
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
    return std::string(path, n);
}

void Shell::send_event(ZleEvent event) const
{
    constexpr const char* sequences[] = {
        "\033[6770~",  // CdParent
        "\033[6771~",  // CdChild
        "\033[6772~",  // InsertName
        "\033[6773~",  // InsertPath
        "\033[6774~",  // RefreshPrompt
    };
    const char* sequence = sequences[static_cast<unsigned>(event)];
    ttywrite(sequence, std::strlen(sequence), 1);
}
