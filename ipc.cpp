// See LICENSE for license details.

#include <cerrno>               // for errno
#include <charconv>             // for std::from_chars()
#include <cstdlib>              // for std::getenv()
#include <cstring>              // for std::memcpy(), std::strcmp(), ...
#include <string>               // for std::string, std::to_string()

#include <sys/socket.h>         // for accept4(), bind(), listen(), socket()
#include <sys/stat.h>           // for chmod()
#include <sys/un.h>             // for sockaddr_un
#include <unistd.h>             // for close(), mkdtemp(), rmdir(), unlink()

#include "ipc.hpp"
#include "panel.hpp"



void Ipc::cleanup() noexcept
{
    if ( m_owner != ::getpid() ) return;

    const int fd = m_fd;
    m_fd = -1;
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

const char* Ipc::init()
{
    if ( m_fd >= 0 ) return m_address.sun_path;

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

    m_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if ( m_fd < 0 ) {
        const int error = errno;
        cleanup();
        die("create SC control socket failed: %s\n", std::strerror(error));
    }

    m_address.sun_family = AF_UNIX;
    const size_t directory_length = std::strlen(m_directory);
    std::memcpy(m_address.sun_path, m_directory, directory_length);
    std::memcpy(m_address.sun_path + directory_length, kSocketName, sizeof(kSocketName));

    if ( ::bind(m_fd, reinterpret_cast<sockaddr*>(&m_address), sizeof(m_address)) >= 0
      && ::listen(m_fd, 4) >= 0 && ::chmod(m_address.sun_path, 0600) >= 0 )
        return m_address.sun_path;

    const int error = errno;
    cleanup();
    die("initialize SC control socket failed: %s\n", std::strerror(error));
}

void Ipc::service(const Panel& panel)
{
    const int client = ::accept4(m_fd, nullptr, nullptr, SOCK_CLOEXEC);
    if ( client < 0 ) return;

    char request[32]{};
    std::string reply = "E\n";
    if ( const ssize_t n = ::read(client, request, sizeof(request) - 1); n > 0 ) {
        if ( std::strcmp(request, "selected\n") == 0 ) {
            if ( const Panel::Entry* entry = panel.selected_entry() ) {
                reply = std::string(entry->is_dir ? "D\t" : "F\t") + entry->name + "\n";
            }
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
