# SC zsh shell integration

SC remains a terminal overlay. The shell retains the full PTY size and owns its
working directory, prompt, and editable command line. Its required zsh adapter
lets panel actions cooperate with that state without injecting `cd ...` into the
command line.

## Channels

```
zsh / sc.zsh  --- OSC notifications via PTY -->  SC
zsh / sc.zsh  --- socket request ------------->  SC IPC server
zsh / sc.zsh  <-- socket reply ----------------  SC IPC server
zsh ZLE       <-- private sequences via PTY ---  SC
```

| Component | Responsibility |
| --- | --- |
| `sc.zsh` | Owns ZLE's command buffer, `cd`, prompt construction, and Enter behavior. |
| `panel.cpp` | Owns selection, panel geometry, and visibility; supplies panel state to IPC replies. |
| `shell.cpp` | Owns communication with the managed shell: socket protocol, PTY events, and shell state reads. |
| `st.c` | Creates `SC_SOCKET` before starting zsh and parses private OSC notifications. |
| `x.c` | Watches the control socket in the main event loop. |
| `scctl` | Queries the private socket for zsh. |

## Activation and startup

SC requires this adapter. Put it after prompt/theme configuration in `~/.zshrc`:

```zsh
if [[ -n ${SC_SOCKET-} ]]; then
  export SCCTL=/usr/local/bin/scctl
  source /usr/local/share/sc/sc.zsh
fi
```

`SC_SOCKET` exists only in zsh started by SC. Do not source the adapter in the
parent shell before launching SC: the condition is false there.

Startup proceeds as follows:

1. `st.c:ttynew()` calls `shell_preinit()` before it forks.
2. `Shell::preinit()` creates an owner-only Unix socket in a private `sc-*` directory
   under `$XDG_RUNTIME_DIR`, or under `/tmp` when the runtime directory is unavailable.
3. `shell_preinit()` exports its path as `SC_SOCKET`; the child zsh inherits it.
4. The child zsh reads `.zshrc`, sources `sc.zsh`, installs ZLE widgets and
   bindings, then emits OSC `6770`.
5. `Shell::init()` reads startup output through the normal terminal parser while waiting
   up to one second for the adapter.
6. `st.c:strhandle()` handles OSC `6770` with `shell_notify_zsh_ready()`, completing
   shell initialization; `shell_init()` then calls `Panel::init()` to load the initial directory.

The readiness handshake establishes that the adapter is available before panel polling,
IPC service, or input handling begins. SC exits with a configuration error if the
mandatory adapter does not report readiness within the deadline.

## Panel keys and ZLE

`Panel::handle_key()` continues to handle selection movement locally. For an
operation requiring shell state, it writes a fixed private sequence to the PTY:

| Key | Sequence | ZLE widget |
| --- | --- | --- |
| `Ctrl+PgUp` | `ESC [ 6770 ~` | `_sc_cd_parent` |
| `Ctrl+PgDn` | `ESC [ 6771 ~` | `_sc_cd_child` |
| `Ctrl+Enter` | `ESC [ 6772 ~` | `_sc_insert_selected_name` |
| `Ctrl+Shift+Enter` | `ESC [ 6773 ~` | `_sc_insert_selected_path` |
| panel refresh | `ESC [ 6774 ~` | `_sc_refresh_prompt` |

The sequences contain no pathname and are not shell commands. They only select
a ZLE widget. `_sc_cd_child`, for example, queries SC for the selected item and
runs `builtin cd -- "$REPLY"` only when that item is a directory. ZLE retains
the current `BUFFER` throughout this operation.

Plain Enter is deliberately not consumed by the panel. `_sc_enter` owns it:

- non-empty `BUFFER`: invoke `zle .accept-line`;
- empty `BUFFER`, selected directory: change directory;
- empty `BUFFER`, selected file: place its shell-quoted path in `BUFFER` and
  accept the line.

This makes ZLE's buffer, rather than a terminal-side heuristic, the authority
for Enter behavior.

## Control socket

`sc.zsh` calls `scctl selected` or `scctl padding <applied_padding>`. `scctl` connects to the
socket named by `SC_SOCKET`, sends the request, and prints the response.

`x.c` adds `shell_ipc_fd()` to its `pselect()` fd set. On readiness it calls
`shell_service_ipc()`, which dispatches to:

- `Shell::service_ipc()` replies to `selected` with the type (`D` or `F`) plus entry name;
- `Panel::prompt_padding(applied_padding)` for `padding <applied_padding>`: total
  number of prompt-owned newlines needed after replacing the adapter's existing prefix.

The process that creates the socket remains its sole cleanup owner across `fork()`.
Normal `exit()` paths release it through `Shell`'s destructor. The `SIGCHLD` path calls
the same idempotent cleanup explicitly before `_exit()`; that cleanup uses only
async-signal-safe system calls. The forked shell cannot close or unlink the parent's
socket through its inherited `Shell` state.

`$XDG_RUNTIME_DIR` is preferred because it is private to the logged-in user and intended
for runtime sockets. SC falls back to its owner-only directory under `/tmp` when that
variable is absent, relative, unusable, or too long for a Unix-domain socket address.

## Cwd update path

`sc.zsh` installs a `chpwd` hook. Every successful zsh directory change emits
OSC `6771`; `st.c:strhandle()` turns that into `shell_notify_cwd_changed()`.
The panel then marks `cwd_changed_` and reconciles its cached directory against
`/proc/<shell-pid>/cwd` on the next poll.

The panel updates its cached directory when zsh reports a directory change, rather
than sampling `/proc/.../cwd` on every redraw.

## Prompt padding and redraw

The panel reads the terminal cursor through `tgetcursor()` when it determines whether
a prompt needs padding.

Before zsh renders or refreshes a prompt, `_sc_update_prompt` calls
`scctl padding <applied_padding>`. SC removes the adapter's existing prompt-owned
newlines from the terminal cursor row and returns the total number of real newlines
needed in `PROMPT`; `_sc_refresh_prompt` then calls `zle reset-prompt`. `_sc_precmd`
starts each new prompt with `applied_padding` set to zero.

ZLE emits and tracks these newlines itself, so its display model remains valid.
SC never moves the terminal cursor or fakes a `SIGWINCH` to uncover a prompt.

zle reset-prompt repaints the current editing line in place: it re-expands and replaces the existing prompt, then redraws the unchanged
  $BUFFER. It does not accept the line or add a new newline/prompt.

## Bash

For the full design, Bash is substantially harder than zsh. I’d make zsh the supported shell first.
Bash can cover part of it cleanly:
- bind -x gives a handler access to READLINE_LINE, READLINE_POINT, and READLINE_MARK; changes are reflected back into the active editing buffer. So a Ctrl+PgDn handler can query SC, verify the selected entry is a directory, run cd, and preserve the draft. Bash bind -x documentation
- PROMPT_COMMAND can add calculated prompt padding before Bash begins the next input line. Bash interactive-shell behavior

But it lacks ZLE’s equivalent of “change the prompt and re-render this currently edited line.” Bash/Readline has redraw-current-line, but that refreshes the existing Readline display; it does not rerun PROMPT_COMMAND or reliably re-expand a changed PS1. Readline commands

There is a second problem: a bind -x handler can inspect whether READLINE_LINE is empty, but it has no clean shell-level way to say “otherwise invoke Readline’s normal accept-line now.” Manually evaluating the buffer would diverge from normal Bash history, multiline input, completion, and error behavior.
