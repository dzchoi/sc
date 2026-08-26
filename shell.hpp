// See LICENSE for license details.
//
// Boundary for communication with SC's managed shell.

#pragma once

#include <sys/types.h>          // for pid_t
#include <sys/un.h>             // for sockaddr_un
#include <unistd.h>             // for tcgetpgrp()

#include <string>               // for std::string



enum class ZleEvent {
    CdParent,
    CdChild,
    InsertName,
    InsertPath,
    RefreshPrompt,
    SwitchPanel,
};

class Shell {
public:
    Shell() = default;
    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;
    ~Shell() { cleanup(); }

    // Creates the owner-only control socket and zsh startup shim, then exports the
    // bootstrap environment before the shell forks.
    void preinit();

    // Associates the shell with its PTY and services startup I/O until the first
    // preprompt request has established both panel snapshots.
    void init(int pty_fd, pid_t shell_pid);

    // The control socket watched by the main event loop.
    int ipc_fd() const { return m_ipc_fd; }

    // Services one pending client request through Comm's focused panel.
    void service_ipc();

    // Releases the socket and startup shim using only async-signal-safe operations. The
    // process that called preinit() is the sole owner; forked children leave them alone.
    void cleanup() noexcept;

    // Whether the shell owns the PTY's foreground process group.
    bool owns_pty() const { return ::tcgetpgrp(m_pty_fd) == m_shell_pid; }

    // Returns the shell's cwd via /proc/<shell-pid>/cwd, always ending with '/'. Failure
    // terminates SC because a valid directory is required for the panel snapshot.
    std::string get_cwd() const;

    // Delivers a fixed SC control event to the shell's ZLE input stream.
    void send_event(ZleEvent event) const;

private:
    inline static constexpr char kTmpDirectory[] = "/tmp";
    inline static constexpr char kDirTemplate[] = "sc-XXXXXX";
    inline static constexpr char kSocketName[] = "/control";
    inline static constexpr char kZshEnvName[] = "/.zshenv";

    pid_t m_my_pid = 0;
    int m_ipc_fd = -1;
    int m_pty_fd = -1;
    pid_t m_shell_pid = 0;
    // The first preprompt request establishes the initial panel snapshots.
    bool m_preprompt_requested = false;
    char m_runtime_dir[sizeof(sockaddr_un{}.sun_path)]{};
    char m_zshenv_path[sizeof(sockaddr_un{}.sun_path)]{};
    sockaddr_un m_socket{};

    // Validates the adjacent sc.zsh, writes the private startup shim, and exports its
    // bootstrap environment for the shell fork.
    void setup_zsh_environment();

    // Sweeps abandoned sc-XXXXXX directories under `base`.
    static void cleanup_stale(const char* base);
};
