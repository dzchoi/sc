# SC zsh shell integration

SC remains a terminal overlay. The shell retains the full PTY size and owns its
working directory, prompt, and editable command line. Its required zsh adapter
lets panel actions cooperate with that state without injecting `cd ...` into the
command line.

## Channels

```
zsh / sc.zsh  --- socket request ------------->  SC IPC server
zsh / sc.zsh  <-- socket reply ----------------  SC IPC server
zsh ZLE       <-- private sequences via PTY ---  SC
```

| Component | Responsibility |
| --- | --- |
| `sc.zsh` | Owns ZLE's command buffer, `cd`, prompt construction, and Enter behavior. |
| `comm.cpp` | Coordinates focus, layout, visibility, shared redraw decisions, and IPC-facing panel state. |
| `panel.cpp` | Owns each pane's directory snapshot, selection, geometry, and cached rendering state. |
| `shell.cpp` | Owns the managed-shell socket protocol, PTY events, and shell state reads. |
| `st.c` | Starts the managed shell and connects its PTY lifecycle to shell initialization. |
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
   bindings, then continues processing its startup files.
5. `Shell::init()` polls both the PTY and control socket, processing startup output while
   waiting up to one second for the first preprompt request.
6. The first `precmd` hook calls `scctl preprompt`. SC synchronously establishes both
   panels' initial cwd and directory snapshots, replies with the prompt padding, and
   completes shell initialization before zsh prints the prompt.

The first preprompt handshake establishes both adapter availability and both panels'
data invariants before normal polling or input handling begins. SC exits with a
configuration error if the mandatory adapter does not prepare a prompt within the
deadline.

## Panel keys and ZLE

`Comm::handle_key()` handles coordinator shortcuts and delegates selection movement to
the focused panel. For an operation requiring shell state, Comm sends a fixed private
sequence to the PTY:

| Key | Sequence | ZLE widget |
| --- | --- | --- |
| `Ctrl+PgUp` | `ESC [ 6770 ~` | `_sc_cd_parent` |
| `Ctrl+PgDn` | `ESC [ 6771 ~` | `_sc_cd_child` |
| `Ctrl+Enter` | `ESC [ 6772 ~` | `_sc_insert_selected_name` |
| `Ctrl+Shift+Enter` | `ESC [ 6773 ~` | `_sc_insert_selected_path` |
| panel refresh | `ESC [ 6774 ~` | `_sc_refresh_prompt` |
| `Tab` in dual mode | `ESC [ 6775 ~` | `_sc_switch_panel` |

The sequences contain no pathname and are not shell commands. They only select
a ZLE widget. `_sc_cd_child`, for example, queries SC for the selected item and
runs `builtin cd -- "$REPLY"` only when that item is a directory. ZLE retains
the current `BUFFER` throughout this operation.

The insertion widgets quote the selected name or absolute path, insert it at
ZLE's current `CURSOR` position, and advance the cursor past the inserted text.

Ctrl+P switches between single and dual layout without changing focus. In dual mode,
Tab focuses the other panel; `_sc_switch_panel` queries that panel's retained cwd,
changes the shell to it, and refreshes the prompt. Ctrl+O hides or restores both panels
without discarding their layout, focus, directories, or selections.

F3 and F4 remain ordinary terminal key sequences and are bound directly by `sc.zsh`.
`SC_USER_COMMANDS` maps those sequences to command arguments. The shared widget queries
SC for the selected name, substitutes its absolute path for a standalone `{}` argument
(or appends it when absent), and executes the resulting argument vector without
evaluating it as shell code. It invalidates the active ZLE display before execution and
passes the command status through `_sc_refresh_prompt`.

Plain Enter is deliberately not consumed by the panel. `_sc_enter` owns it:

- non-empty `BUFFER`: invoke `zle .accept-line`;
- empty `BUFFER`, selected directory: change directory;
- empty `BUFFER`, selected file: place its shell-quoted path in `BUFFER` and
  accept the line.
- no active selection (including a force-hidden panel): invoke `zle .accept-line`.

This makes ZLE's buffer, rather than a terminal-side heuristic, the authority
for Enter behavior.

## Control socket

The Zsh adapter uses `scctl selected`, `scctl focused_cwd`, and
`scctl preprompt <applied_padding>`. `scctl` connects to the socket named by
`SC_SOCKET`, sends the request, and prints the response.

During initialization, `Shell::init()` services the first preprompt request directly.
Afterward, `x.c` adds `shell_ipc_fd()` to its `pselect()` fd set and calls
`shell_service_ipc()` when a request is ready. Requests dispatch to:

- `Shell::service_ipc()` replies to `selected` with the focused panel's entry name, or
  no payload when no panel is effectively visible. The Zsh widget checks the selected
  path's current type before acting on it;
- `focused_cwd` returns the focused panel's retained directory so a Tab focus change
  can make it the shell working directory;
- `preprompt <applied_padding>` synchronously reads the shell cwd, calls
  `Comm::reload_panels()`, then returns the result of
  `Comm::adjust_padding()`: the total number of prompt-owned newlines needed
  after replacing the adapter's existing prefix;

The process that creates the socket remains its sole cleanup owner across `fork()`.
Normal `exit()` paths release it through `Shell`'s destructor. The `SIGCHLD` path calls
the same idempotent cleanup explicitly before `_exit()`; that cleanup uses only
async-signal-safe system calls. The forked shell cannot close or unlink the parent's
socket through its inherited `Shell` state.

`$XDG_RUNTIME_DIR` is preferred because it is private to the logged-in user and intended
for runtime sockets. SC falls back to its owner-only directory under `/tmp` when that
variable is absent, relative, unusable, or too long for a Unix-domain socket address.

## Directory refresh path

Each preprompt request reads `/proc/<shell-pid>/cwd` and rebuilds the focused panel's
corresponding directory snapshot before replying. The inactive panel retains its own
directory and snapshot until Tab focuses it and ZLE changes the shell to that directory.
Preprompt requests cover ordinary accepted commands, panel-driven directory changes,
successful F-key commands, and prompt refreshes after geometry or visibility changes.
Failed widget actions do not send a refresh request. The synchronous request avoids
ordering state across the PTY and control-socket channels.

## Prompt padding and redraw

`Comm` reads the terminal cursor through `tgetcursor()` when it determines whether a
prompt needs padding below the panels' shared vertical extent.

Before zsh renders or refreshes a prompt, `_sc_update_prompt` calls
`scctl preprompt <applied_padding>`. SC refreshes the focused panel snapshot, discounts
the adapter's existing prompt-owned newlines from the terminal cursor row, and returns
the total number of real newlines needed in `PROMPT`; `_sc_refresh_prompt` then calls
`zle reset-prompt`. `_sc_precmd` starts each new prompt with `applied_padding` set to
zero. The adapter treats a failed request as zero required padding, leaving an ordinary
unpadded prompt.

ZLE emits and tracks these newlines itself, so its display model remains valid.
SC never moves the terminal cursor or fakes a `SIGWINCH` to uncover a prompt.

Directory and user-command widgets call `zle -I` before taking control of the terminal.
Before invalidation, they add the current `BUFFERLINES` to the applied-padding count so
the cursor row released below a multiline or wrapped editing display can be discounted.
They then pass the action's status to `_sc_refresh_prompt`. On success, that function
sends a preprompt request and resets the active prompt. On failure, it clears the stored
padding and restores the unpadded `PROMPT` value without resetting the active ZLE
prompt. In both cases the widget returns the action's status.

`zle reset-prompt` repaints the current editing line in place: it re-expands and
replaces the existing prompt, then redraws the unchanged `$BUFFER`. It does not accept
the line or add a new prompt.

## Bash

For the full design, Bash is substantially harder than zsh. I’d make zsh the supported shell first.
Bash can cover part of it cleanly:
- bind -x gives a handler access to READLINE_LINE, READLINE_POINT, and READLINE_MARK; changes are reflected back into the active editing buffer. So a Ctrl+PgDn handler can query SC, verify the selected entry is a directory, run cd, and preserve the draft. Bash bind -x documentation
- PROMPT_COMMAND can add calculated prompt padding before Bash begins the next input line. Bash interactive-shell behavior

But it lacks ZLE’s equivalent of “change the prompt and re-render this currently edited line.” Bash/Readline has redraw-current-line, but that refreshes the existing Readline display; it does not rerun PROMPT_COMMAND or reliably re-expand a changed PS1. Readline commands

There is a second problem: a bind -x handler can inspect whether READLINE_LINE is empty, but it has no clean shell-level way to say “otherwise invoke Readline’s normal accept-line now.” Manually evaluating the buffer would diverge from normal Bash history, multiline input, completion, and error behavior.
