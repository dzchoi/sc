// See LICENSE for license details.
//
// Boundary for communication with SC's managed shell.

#pragma once

#include <termios.h>            // for tcgetpgrp()
#include <sys/types.h>          // for pid_t
#include <sys/un.h>             // for sockaddr_un

#include <string>               // for std::string



class Panel;

enum class ZleEvent {
    CdParent,
    CdChild,
    InsertName,
    InsertPath,
    RefreshPrompt,
};

class Shell {
public:
    Shell() = default;
    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;
    ~Shell() { cleanup(); }

    // Creates the owner-only control socket and exports its path as SC_SOCKET before
    // the shell forks.
    void preinit();

    // Associates the shell with its PTY and waits for the required adapter to report
    // readiness before normal input handling begins.
    void init(int pty_fd, pid_t shell_pid);

    // The control socket watched by the main event loop.
    int ipc_fd() const { return m_ipc_fd; }

    // Services one pending client request using the panel's current selection state.
    void service_ipc(const Panel& panel) const;

    // Releases the socket using only async-signal-safe operations. The process that
    // called preinit() is the sole owner; forked children leave the parent's socket alone.
    void cleanup() noexcept;

    // Whether the shell owns the PTY's foreground process group.
    bool owns_pty() const { return ::tcgetpgrp(m_pty_fd) == m_pid; }

    // Returns the shell's cwd via /proc/<shell-pid>/cwd. Failure terminates SC because
    // a valid directory is required for the panel snapshot.
    std::string get_cwd() const;

    // Delivers a fixed SC control event to the shell's ZLE input stream.
    void send_event(ZleEvent event) const;

    // Records the adapter's OSC readiness notification.
    void notify_zsh_ready() { m_zsh_ready = true; }

private:
    inline static constexpr char kTmpDirectory[] = "/tmp";
    inline static constexpr char kDirectoryName[] = "sc-XXXXXX";
    inline static constexpr char kSocketName[] = "/control";

    pid_t m_owner = 0;
    int m_ipc_fd = -1;
    int m_pty_fd = -1;
    pid_t m_pid = 0;
    bool m_zsh_ready = false;
    char m_directory[sizeof(sockaddr_un{}.sun_path)]{};
    sockaddr_un m_address{};
};
