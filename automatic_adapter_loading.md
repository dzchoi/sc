# Automatic zsh adapter loading

## Goal and readiness invariant

SC loads its required zsh adapter without modifying user startup files or injecting
shell commands through the PTY. Before normal panel polling, IPC service, or input
handling begins:

- the user's applicable zsh startup files have completed;
- `sc.zsh` has installed its hooks, widgets, and private key bindings;
- the first `preprompt` request has established both panels' authoritative directory
  descriptors and nonempty snapshots.

`Shell::init()` services startup PTY output and the private socket during a bounded
wait. A valid first preprompt request is the sole readiness signal.

## Single-file `ZDOTDIR` shim

`Shell::preinit()` requires a configured `$XDG_RUNTIME_DIR` to be absolute, as mandated
by the XDG Base Directory specification. It creates one owner-only `sc-XXXXXX` directory
there, falling back to `/tmp` only when the variable is unset or directory creation
fails. It places the control socket and a generated `.zshenv` in that directory. Before
the shell fork, it preserves both the presence and value of the original `ZDOTDIR`, then
exports this bootstrap environment:

- `ZDOTDIR=<private-directory>` selects the generated `.zshenv`;
- `SC_ZSH_INIT=<executable-directory>/sc.zsh` identifies the adapter;
- `SC_SOCKET=<private-directory>/control` identifies this SC instance.

The generated `.zshenv` contains an identifying comment and one executable line: a
quoted `source "$SC_ZSH_INIT"`. The adapter's shim immediately unsets `SC_ZSH_INIT`,
restores the original `ZDOTDIR` including its unset-versus-empty distinction, and
sources the user's effective `.zshenv`. Any `ZDOTDIR` change made there remains active,
so zsh itself selects the subsequent
`.zprofile`, `.zshrc`, and `.zlogin` in normal order. This single proxy is sufficient;
later startup-file proxies would duplicate zsh's own selection logic.

Non-interactive shells restore and source `.zshenv` but skip adapter installation.
Interactive SC shells register a one-shot `_sc_bootstrap` precmd hook. The first
precmd occurs after applicable startup files; the bootstrap captures the configured
prompt, installs the permanent precmd hook, ZLE widgets, and bindings, then issues the
first synchronous preprompt transaction.

`sc.zsh` accepts only the bootstrap source identified by `SC_ZSH_INIT`. A later manual
source of the current adapter therefore becomes a no-op instead of reinstalling hooks
or re-sourcing `.zshenv`.

## Asset layout and validation

`/proc/self/exe` supplies the executable's canonical directory. Both supported layouts
place both runtime files together:

- development: `.build/sc` and the `.build/sc.zsh` symlink;
- installed: `<prefix>/bin/sc` and `<prefix>/bin/sc.zsh`.

SC validates that `sc.zsh` is a readable regular file before forking. The generated
file remains protected by its owner-only directory and is changed to mode `0600`.
Path resolution, directory creation, and file open failures terminate startup rather
than allowing a partially integrated shell.

## Environment boundary

After the shim has identified an interactive SC shell, `SC_SOCKET` becomes a
non-exported global parameter, so ordinary child processes and nested shells cannot
attach to the outer panel accidentally. Runtime request framing, private ZLE events,
and prompt synchronization are documented in
[SC zsh shell integration](shell_integration.md).

## Resource ownership and recovery

`Shell` owns the socket, generated `.zshenv`, and private directory. It records both
file paths before forking so normal destruction and the `SIGCHLD` path share one
idempotent cleanup using async-signal-safe operations. The creator PID guard prevents
the forked shell from removing its parent's resources.

A crash can bypass cleanup. Before allocating a new directory, SC scans only names of
the exact `sc-XXXXXX` length in the selected runtime parent and `/tmp`. It removes an
entry only when connecting to its control socket returns `ECONNREFUSED`, indicating
that the socket had no listener when probed. `ENOENT` and all other errors are left
untouched because another SC may be between `mkdtemp()` and `bind()`. Another SC can
also briefly return `ECONNREFUSED` between `bind()` and `listen()`; cleanup relies on
that interval not overlapping the scan.

## Expected limitations

SC requires an interactive zsh with normal startup-file loading. `zsh -f`, a different
`$SHELL`, or user startup code that deletes `_sc_bootstrap` from the precmd hooks cannot
complete readiness and fails through the one-second timeout. The timeout covers the
user's complete startup sequence, not just execution time inside `sc.zsh`.

The `_sc_` function namespace and SC bootstrap variables are reserved during startup.
User configuration can replace `SC_USER_COMMANDS` in `.zshrc`; widget bindings consume
its final value when `_sc_bootstrap` runs.

## Verification matrix

Exercise startup with no `.zshrc`, prompt configuration in `.zshrc`, an obsolete
manual adapter source, unset/empty/custom `ZDOTDIR`, a `.zshenv` that changes `ZDOTDIR`,
login and non-login shells, paths containing spaces, missing companion assets, timeout,
nested zsh, startup cwd changes, normal and `SIGCHLD` cleanup, abandoned runtime
directories, and sentinel output from every applicable startup file.
