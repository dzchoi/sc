# Suspended user commands

> **Status:** This feature is a design proposal and is not implemented yet.

## User-visible behavior

Each entry in `SC_USER_COMMANDS` may retain the commands that were suspended after
that entry launched them. Launch and resume remain separate actions: the command's
ordinary key always starts a new process for the current selection, while a configured
resume key returns to one of that command's suspended processes.

The initial default would keep F4 as `vi -- {}` and assign Shift+F4 to resume a Vim
previously launched by that F4 entry. Resuming runs inside a ZLE widget, so it does not
accept, replace, or otherwise modify the current command line.

Multiple suspended instances form a last-in, first-out stack for their user-command
entry. If Vim A is suspended and then Vim B is opened and suspended, Shift+F4 resumes
B. B remains at the top when it is suspended again. When B exits, the next resume
returns to A.

A resume action considers only suspended processes. It skips tracked jobs that are
running in the background, removes completed or stale records, and does nothing when
the command has no suspended instance. Thus a running background job is never brought
to the foreground unexpectedly.

## Configuration

The resume binding should refer to an existing `SC_USER_COMMANDS` entry rather than
repeat its command text. One possible backward-compatible interface is:

```zsh
typeset -gA SC_USER_COMMANDS=(
    $'\eOR' 'less -- {}'
    $'\eOS' 'vi -- {}'
)

typeset -gA SC_USER_RESUME_KEYS=(
    $'\e[1;2S' $'\eOS'  # Shift+F4 resumes jobs launched by the F4 entry.
)
```

The exact public name and representation remain to be decided during implementation.
The invariant is that suspended state belongs to a configured user-command entry, not
to the physical function key or the command text.

## Job ownership and identity

Zsh continues to own job control. SC does not signal foreground processes, manage
process groups, or implement an alternative to `fg`.

Before launching a command, the widget snapshots the keys in zsh's `jobstates`
parameter. If the command returns because it was suspended, the widget identifies the
new suspended job and pushes a record onto the command's stack. A record contains the
zsh job number and the job's process identity from `jobstates`.

Before resuming a record, the widget verifies that:

- the job number still exists;
- its state is `suspended`; and
- its recorded process identity still matches.

The process check prevents a stale record from selecting an unrelated job after zsh
reuses a job number. The widget must not infer identity from `%+`, command status, or
`jobtexts`: other shell activity can change the current job, stop statuses are
platform-dependent, and jobs launched inside a ZLE widget may have empty job text.

After `fg` returns, the widget retains the record when the job was suspended again and
removes it when the job completed. Stale records are pruned while finding the newest
eligible suspended job.

## Hiding job-control notifications

Zsh writes foreground-stop notifications such as
`zsh: suspended  vi -- /path/to/file` directly to the controlling terminal. The
`NOTIFY` option, stdout or stderr redirection, `POSIX_JOBS`, and a `TRAPCHLD` hook do
not reliably suppress this output. `fg` can also print the command it resumes.

SC should hide these lines with a terminal screen transaction rather than match their
text. Text filtering would depend on zsh version, locale, command length, and terminal
wrapping. Cursor-up and erase-line sequences have the same ambiguity.

The adapter would emit private terminal control sequences through the PTY. For example,
using an illustrative, not yet assigned OSC number:

```text
ESC ] 6973 ; checkpoint ESC \
ESC ] 6973 ; commit     ESC \
ESC ] 6973 ; rollback   ESC \
```

The launch lifecycle would be:

```text
ZLE invalidates its display
  -> checkpoint
  -> launch or fg
  -> application runs
  -> commit when it exits, or rollback when it suspends
  -> refresh the prompt and panels
```

The checkpoint preserves the primary-screen contents, cursor, and the terminal state
needed to restore a consistent primary screen. Commit discards that saved state and
leaves legitimate command output visible. Rollback restores it after zsh has written
the stop notification, then marks the restored rows dirty for presentation.

These controls use the PTY instead of the control socket because byte ordering is part
of the invariant. SC must parse the notification before it parses the subsequent
rollback. A socket request is carried on an independent channel and cannot establish
that ordering. No new IPC request or Unix signal is needed.

Only one screen transaction may be active because a user-command ZLE widget executes
synchronously. Commit and rollback are ignored when no checkpoint exists. The saved
screen must follow terminal resizes that occur while the command is active.

## Decisions

- Keep job control in zsh and terminal presentation in SC.
- Store a LIFO suspension stack per `SC_USER_COMMANDS` entry.
- Separate launching from resuming so the normal command key always uses the current
  panel selection.
- Resume only jobs verified as suspended; never foreground a running background job.
- Restore a saved screen instead of recognizing or erasing zsh notification text.
- Send screen-transaction controls through the PTY to preserve display ordering.
