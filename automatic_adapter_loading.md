# Automatic zsh adapter loading

## Goal

SC should load its required zsh adapter automatically without modifying the user's
startup files or injecting shell commands through the PTY. The user's normal zsh
configuration must retain its ordering and `ZDOTDIR` behavior, and `Shell::init()` must
not return until the adapter has sent the first preprompt request.

## Startup invariant

Before normal panel polling, IPC service, or input handling begins:

- the user's applicable zsh startup files have completed;
- `sc.zsh` has installed its hooks, widgets, and private key bindings;
- the first `scctl preprompt` request has established the panel's authoritative cwd and
  nonempty directory snapshot.

Adapter startup and later panel actions are separate mechanisms. Startup uses zsh's
startup-file mechanism; runtime actions continue to use fixed private ZLE events. SC
must not construct or inject arbitrary shell-language commands through the PTY.

## Private `ZDOTDIR` proxy

`shell_preinit()` runs immediately before the shell is forked. Extend that preparation
to create a private, owner-only startup directory and point `ZDOTDIR` at it for the
initial zsh process. This keeps the existing `st.c` and `x.c` integration unchanged.

Before changing `ZDOTDIR`, preserve both whether it was originally set and its value.
An unset `ZDOTDIR` must retain zsh's normal `$HOME` fallback; it must not be conflated
with an explicitly set empty value.

Create proxy files for the user startup stages selected through `ZDOTDIR`:

- `.zshenv`
- `.zprofile`
- `.zshrc`
- `.zlogin`

Each proxy temporarily restores the user's effective `ZDOTDIR`, sources the matching
user file when readable, records any `ZDOTDIR` change made by that file, and restores
the private directory so zsh selects the next proxy. This preserves changes such as a
user `.zshenv` selecting a different directory for the later startup files.

The `.zshrc` proxy performs these operations in order:

1. Source the user's effective `.zshrc`.
2. Source SC's adapter.
3. For a non-login shell, restore the user's final `ZDOTDIR`.

For a login shell, keep the proxy directory active until `.zlogin`. Its proxy sources
the user's effective `.zlogin` and then restores the user's final `ZDOTDIR`. No
`.zlogout` proxy is required because zsh will subsequently select the user's own
`.zlogout` through the restored `ZDOTDIR`.

System-wide startup files remain under zsh's control and retain their normal ordering
relative to each proxied user file.

## Adapter and helper discovery

Resolve and validate `sc.zsh` and `scctl` before forking. Pass their absolute paths to
the proxy through environment variables instead of interpolating them into generated
shell code; this avoids pathname quoting problems.

Support the two normal layouts:

- Development: `scctl` beside the executable in `.build/`, with `sc.zsh` in its parent
  directory.
- Installed: `scctl` beside the executable, with `sc.zsh` in `../share/sc/`.

Allow explicit environment overrides for unusual or relocated layouts. Fail before
forking with a precise diagnostic when either required asset is missing, unreadable, or
not executable as applicable.

## Readiness ownership

Make `sc.zsh` idempotent so an existing manual integration does not install duplicate
hooks or bindings. The first `precmd` invocation naturally occurs after all applicable
startup files, so its `scctl preprompt` request is the sole readiness signal even when an
existing manual integration sourced the adapter earlier.

`Shell::init()` polls startup PTY output and the control socket within one bounded wait.
It returns only after servicing a valid preprompt request, which initializes the panel cwd
and directory entries before replying to zsh.

## Environment boundary

The launcher supplies the resolved `scctl` pathname to `sc.zsh`; the adapter does not
need to execute an `export` command through the terminal.

After initialization, avoid exporting `SC_SOCKET` to arbitrary child commands and
nested shells. Keep it as a shell parameter and provide it only to `scctl`:

```zsh
_scctl() {
    SC_SOCKET=$SC_SOCKET command "$SCCTL" "$@"
}
```

This confines access to the outer zsh adapter and prevents a nested zsh from
accidentally attaching itself to the outer panel. `SCCTL` likewise only needs to be a
global shell parameter because its value is expanded before the helper is executed.

## Resource ownership and cleanup

Use a cohesive bootstrap owner for the private startup directory and its generated
files rather than making `Shell` own unrelated zsh configuration. Record every fixed
pathname during initialization so cleanup requires only async-signal-safe operations:

- close owned descriptors;
- unlink the generated proxy files;
- remove the private directory.

As with `Shell`, guard cleanup with the creator PID. Normal destruction and the
`SIGCHLD` path must invoke the same idempotent cleanup, while the forked shell must not
remove resources owned by its parent.

## Runtime external commands

Automatic adapter loading must not become a generic command-injection facility. A
future action such as F3 viewing a selected file should add a fixed private ZLE event.
Its zsh widget queries the selected entry over IPC and directly invokes:

```zsh
zle -I
command less -- "$PWD/$REPLY"
zle reset-prompt
```

The command never enters `BUFFER` or shell history. While `less` owns the foreground
PTY, `Panel::visible()` hides the panel; the panel returns when zsh
regains the PTY.

## Expected limitations

Invocations such as `zsh -f`, or user startup code that disables `RCS` before the
`.zshrc` stage, bypass the mechanism SC requires. They should fail through the existing
prompt-readiness deadline with a configuration diagnostic rather than fall back to a
partially integrated shell.

The prompt timeout covers the user's complete applicable startup sequence, not only the
execution time of `sc.zsh`.

## Verification

Cover at least these cases:

- no user `.zshrc`;
- prompt and theme configuration in `.zshrc`;
- an existing manual `source sc.zsh` line;
- initially unset and explicitly set `ZDOTDIR`;
- `ZDOTDIR` changed by `.zshenv` or `.zprofile`;
- login and non-login shells;
- paths containing spaces;
- missing `sc.zsh` or `scctl`;
- the one-second first-preprompt timeout;
- nested zsh processes not inheriting the outer integration;
- final cwd changes made during startup;
- normal-exit and `SIGCHLD` cleanup;
- exact startup ordering using sentinel commands in every startup file.
