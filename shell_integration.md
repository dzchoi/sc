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

## Activation boundary

The first successful `preprompt` request proves that the adapter is installed and
establishes both panels' directory and snapshot invariants before normal input handling
begins. The generated `ZDOTDIR` shim, startup-file ordering, readiness timeout, runtime
asset validation, and cleanup ownership are documented in
[Automatic zsh adapter loading](automatic_adapter_loading.md).

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
Tab focuses the other panel and `_sc_switch_panel` synchronizes the shell cwd from its
retained directory descriptor. Ctrl+O hides or restores both panels without discarding
their layout, focus, directories, or selections. Descriptor handoff and invalidated-
directory recovery are documented in
[FD-backed panel directories](fd_backed_panel_directories.md).

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

The Zsh adapter sends `selected`, `directory_for_shell`, and
`preprompt <cwd> <old_padding>` through `_scctl`. The function uses zsh's
socket and system modules to connect to the non-exported `SC_SOCKET`, send one request,
read its response through EOF, close the descriptor, and place the response in `REPLY`.
Callers consume `REPLY` directly, so requests require neither an external helper nor a
command-substitution subshell. Ordinary child commands and nested shells cannot attach
to the outer SC process because they do not inherit the socket path.

During initialization, `Shell::init()` services the first preprompt request directly.
Afterward, `x.c` adds `shell_ipc_fd()` to its `pselect()` fd set and calls
`shell_service_ipc()` when a request is ready. Requests dispatch to:

- `Shell::service_ipc()` replies to `selected` with the focused panel's entry name, or
  no payload when no panel is effectively visible. The Zsh widget checks the selected
  path's current type before acting on it;
- `directory_for_shell` returns the focused panel's logical cwd with an `L` prefix while
  it names the retained inode; otherwise it returns the procfs target with a `P` prefix.
  `_sc_switch_panel` uses the prefix to select logical or physical `cd` semantics;
- `preprompt <cwd> <old_padding>` synchronously captures the shell cwd,
  validates the logical name against its descriptor, calls `Comm::reload_panels()`,
  then returns the result of
  `Comm::adjust_padding()`: the total number of prompt-owned newlines needed
  after replacing the adapter's existing prefix;

## Directory refresh path

Every ordinary `precmd` boundary and every successful widget action sends `preprompt`,
which captures the shell directory and rebuilds the focused panel before replying.
Failed widget actions send no refresh. Keeping this transaction synchronous avoids
ordering state across the PTY and control socket. Descriptor capture and reload follow
the fd-backed semantics linked above.

## Prompt padding and redraw

`Comm` reads the terminal cursor through `tgetcursor()` when it determines whether a
prompt needs padding below the panels' shared vertical extent. Reading it on demand
avoids maintaining a second cursor position alongside the terminal's authoritative
state.

Before zsh renders or refreshes a prompt, `_sc_update_prompt` sends
`preprompt <cwd> <old_padding>`. SC refreshes the focused panel snapshot,
discounts the adapter's existing prompt-owned newlines from the terminal cursor row,
and returns the total number of real newlines needed in `PROMPT`; `_sc_refresh_prompt`
then calls `zle reset-prompt`. `_sc_precmd` sends zero as `old_padding` for each new
prompt. The adapter treats a failed request as zero required padding, leaving an
ordinary unpadded prompt.

ZLE emits and tracks these newlines itself, so its display model remains valid.
SC never moves the terminal cursor or fakes a `SIGWINCH` to uncover a prompt.

Directory and user-command widgets call `zle -I` before taking control of the terminal.
Before invalidation, they add the current `BUFFERLINES` to the value later sent as
`old_padding`, so the cursor row released below a multiline or wrapped editing display
can be discounted. If a successful command leaves primary-screen output, that cursor
instead marks an output boundary rather than the active prompt boundary.
They then pass the action's status to `_sc_refresh_prompt`. On success, that function
sends a preprompt request and resets the active prompt. On failure, it clears the stored
padding and restores the unpadded `PROMPT` value without resetting the active ZLE
prompt. In both cases the widget returns the action's status.

`zle reset-prompt` repaints the current editing line in place: it re-expands and
replaces the existing prompt, then redraws the unchanged `$BUFFER`. It does not accept
the line or add a new prompt.

Assigning `PROMPT` does not re-expand the active prompt. After a failed widget clears
`_sc_prompt_padding`, a later in-place refresh can therefore report zero even though
the displayed prompt still contains SC-owned padding from its earlier expansion.

## Shell support

SC currently supports zsh only. Its integration depends on ZLE widgets and hooks to
inspect the live editing buffer, change directories without discarding that buffer, and
re-expand an active prompt after panel-driven changes. Bash and other shells are not
supported because they do not provide the same integration boundary.
