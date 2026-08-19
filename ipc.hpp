// See LICENSE for license details.
//
// Owner for SC's private Unix-domain control socket.

#pragma once

#include <sys/types.h>
#include <sys/un.h>

class Panel;

class Ipc {
public:
    Ipc() = default;
    Ipc(const Ipc&) = delete;
    Ipc& operator=(const Ipc&) = delete;
    ~Ipc() { cleanup(); }

    // Creates the owner-only control socket before the shell forks. Panel exports the
    // returned path through SC_SOCKET so the child can reach this socket.
    const char* init();
    int fd() const { return m_fd; }

    // Releases the socket using only async-signal-safe operations. The process that
    // called init() is the sole owner; forked children leave the parent's socket alone.
    // Once init() returns, cleanup() is the only operation that mutates this state.
    void cleanup() noexcept;

    // Services one pending client request using the panel's current selection state.
    void service(const Panel& panel);

private:
    inline static constexpr char kTmpDirectory[] = "/tmp";
    inline static constexpr char kDirectoryName[] = "sc-XXXXXX";
    inline static constexpr char kSocketName[] = "/control";

    pid_t m_owner = 0;
    int m_fd = -1;
    char m_directory[sizeof(sockaddr_un{}.sun_path)]{};
    sockaddr_un m_address{};
};
