# SC — Simple Commander

SC is an [Norton Commander](https://en.wikipedia.org/wiki/Norton_Commander)-inspired
terminal emulator for Linux. It adds keyboard-driven directory panels to
[st](https://st.suckless.org/) while leaving the zsh command line available below
them.

![SC running as an overlay: the shell remains available on the left while the current directory is shown in the top-right panel.](assets/sc-running.png)

## Why SC is different

Most Commander-style file managers follow one of two designs. Graphical commanders
run in a separate desktop window, away from the terminal where shell commands and
their output live. Console commanders take ownership of the terminal screen and
provide their own full-screen interface and command line. Some can open or cooperate
with a subshell, but the commander still controls the screen layout and presents the
prompt as part of its own interface. Command output commonly replaces the panel view
with a separate output screen, followed by a key press to return to the commander.

SC takes a third approach: it is the terminal emulator itself. The panels belong to
the terminal display without replacing the shell that works inside it.

The differences are visible in everyday use:

| | Graphical commander | Full-screen console commander | SC |
| --- | --- | --- | --- |
| File panels | Separate desktop window | Replace the terminal screen | Overlay the terminal |
| Command line | Separate terminal or command launcher | Commander-provided line or subshell | Your live zsh command line |
| Prompt | Not part of the file manager | Recreated or replaced by the commander | Drawn directly by your configured shell |
| Command output | Appears outside the panels | Uses a separate output view | Streams live; panels overlay the result |

SC treats the shell as the primary workspace. Its panels occupy only the upper part of
the terminal, while the real zsh prompt<sup>1</sup> and editable command line
remain available below them. Command output appears as it is produced, without
switching to a separate output screen or waiting for a key press. When the shell is
ready for the next command, the panels overlay the resulting terminal contents without
clearing the output, which remains available in scrollback.<sup>2</sup> When an editor,
pager, or another interactive program needs the terminal, the panels hide automatically
and return when the shell regains control.

This design also preserves the user's prompt theme, command history, completion,
aliases, functions, and shell configuration instead of reproducing those features in
a file-manager command line. To close SC, simply type "exit", just as you would in a regular shell.

SC is not yet a full replacement for the file-management operations offered by other
commanders. Its current strength is combining directory navigation with an ordinary,
unrestricted shell; the remaining scope is listed under [Current limitations](#current-limitations).

<sup>1</sup> Zsh is currently the only supported shell. Support for other shells,
including bash, is being developed.

<sup>2</sup> SC does not provide scrollback directly. It is available through the
[st scrollback patch](https://st.suckless.org/patches/scrollback/).

## Build and run

SC currently requires Linux, an X11 session, and zsh.<sup>1</sup> Building SC also
requires the normal dependencies for `st`: Xlib, Xft, Fontconfig, and FreeType.

Build and run SC from the repository:

```sh
make
./.build/sc
```

SC does not need to be installed. To run it somewhere else, copy `.build/sc` and
`sc.zsh` into the same directory, then run `./sc` from there. The two files must remain
together so SC can load its zsh integration.

SC loads its zsh integration automatically; do not source `sc.zsh` from `.zshrc`.
Zsh startup files and prompt configuration otherwise continue to work normally.
Launching zsh with startup files disabled, as with `zsh -f`, is not supported.

### Installation status

A complete SC installation target has not been implemented yet. The repository still
contains the inherited `make install` target, including support for a custom prefix:

```sh
make install
make PREFIX=/your/prefix install
```

These commands copy the SC executable and zsh integration, but the installed manual
page and other user-facing installation material still describe `st`, not SC. Until
the installation target is converted for SC, use the copy-and-run method above.

## Getting started

SC starts with one panel showing the shell's current directory. The panel occupies the
upper-right part of the terminal, leaving the prompt and command line available below
it. Press `Ctrl+P` to show both panels. Both begin in the starting directory, then
remember their own directories and selections as you use them.

The directory name appears in the panel title. Entries are ordered with `..` first,
then directories, then files; directories and files are sorted by name within their
groups. The columns show the entry name, size or directory marker, modification date,
and modification time. The panel footer shows details for the selected entry.

Panels require a terminal at least 80 columns wide and 12 rows high. They disappear
below that size and return when the terminal is large enough again.

## Panel layout and visibility

`Ctrl+P` switches between a single focused panel and the dual-panel layout. In the
dual-panel layout, `Tab` focuses the other panel and changes the shell directory to
that panel's remembered directory. Returning to the single-panel layout keeps the
focused panel on screen; it does not discard either panel's directory or selection.

`Ctrl+O` hides or restores the panels without changing their layout or contents. SC
also hides the panels automatically while a child program such as an editor, pager,
or other interactive command controls the terminal. They return when control comes
back to the shell.

Panel navigation keys apply only while a panel is visible. For example, `Up` and
`Down` retain their normal shell behavior while the panels are hidden, and `Tab`
retains zsh's normal completion behavior in the single-panel layout.

## Selecting and opening entries

Move the selection with the arrow, paging, `Home`, and `End` keys. Selection stops at
the first or last entry rather than wrapping around.

Use `Ctrl+Page Down` to enter the selected directory and `Ctrl+Page Up` to enter its
parent. Selecting a file and pressing `Ctrl+Page Down` has no effect. After moving to
a parent directory, SC selects the directory you just left when it is still present.

Plain `Enter` depends on the command line:

- If text is already present, SC runs it normally.
- On an empty command line, a selected directory becomes the current directory.
- On an empty command line, a selected file is placed on the command line and run.
- When the panels are hidden, `Enter` retains its normal shell behavior.

These actions preserve any text being edited unless the description above says that
the command line is run.

## Using panel entries in commands

`Ctrl+Enter` inserts the selected entry's name at the cursor. `Ctrl+Shift+Enter`
inserts its full path. SC quotes the inserted text for zsh, so names containing spaces
or shell metacharacters remain one command argument.

The opposite panel's directory is also available through `SC_OTHER_DIR`; see
[Environment variables](#environment-variables).

## Keyboard reference

Except for `Ctrl+O`, the panel controls below apply while a panel is visible. `Tab`
requires the dual-panel layout, and configurable function-key actions require a
visible selected entry.

| Key | Action |
| --- | --- |
| `Ctrl+O` | Hide or restore the panels |
| `Ctrl+P` | Switch between single- and dual-panel layouts |
| `Tab` | Focus the other panel and enter its directory |
| `Up` / `Down` | Select the previous / next entry |
| `Home` / `End` | Select the first / last entry |
| `Page Up` / `Page Down` | Move the selection by one page |
| `Ctrl+Page Up` | Enter the parent directory |
| `Ctrl+Page Down` | Enter the selected directory |
| `Ctrl+Enter` | Insert the selected name at the command cursor |
| `Ctrl+Shift+Enter` | Insert the selected full path at the command cursor |
| `Enter` | Run typed text, or act on the selected entry when the line is empty |
| `F3` | View the selected entry with `less` |
| `F4` | Edit the selected entry with `vi` |
| `Shift+F3` | Display the selected entry with `cat` |

## Configuring function keys

The `SC_USER_COMMANDS` associative array maps function-key sequences to commands for
the selected entry. Define the array in `.zshrc` to replace SC's defaults. A standalone
`{}` argument is replaced with the selected entry's full path. If `{}` is absent, the
path is appended as the final argument.

For example:

```zsh
typeset -gA SC_USER_COMMANDS=(
  $'\eOR'    'less -- {}'       # F3
  $'\eOS'    'vi -- {}'         # F4
  $'\e[1;2R' 'cat -- {}'        # Shift+F3
)
```

The key sequences available for configuration are:

- `F1`–`F4`: `\eOP`, `\eOQ`, `\eOR`, `\eOS`
- `Shift+F1`–`Shift+F4`: `\e[1;2P`, `\e[1;2Q`, `\e[1;2R`, `\e[1;2S`
- `F5`–`F12`: `\e[15~`, `\e[17~`, `\e[18~`, `\e[19~`, `\e[20~`, `\e[21~`,
  `\e[23~`, `\e[24~`
- `Shift+F5`–`Shift+F12`: replace the final `~` in the corresponding sequence with
  `;2~`

## Environment variables

SC provides environment variables for convenient use in commands entered at the
shell prompt.

| Variable | Value | Example |
| --- | --- | --- |
| `SC_OTHER_DIR` | The opposite panel's last active directory | `mv A.txt "$SC_OTHER_DIR/B.txt"` |

`SC_OTHER_DIR` initially names SC's starting directory. Each time `Tab` switches
panels, it changes to the directory of the panel being left. The value remains
available in the single-panel layout and while the panels are hidden.

## Deleted directories and recovery

Each panel remembers its directory even while the panel is inactive. If another
command removes that directory, `Tab` can still switch into it. The panel title marks
the directory as `(deleted)`, and `..` remains available even when every ordinary
entry has gone.

Use `cd ..`, `Ctrl+Page Up`, or a longer relative path such as `cd ../..` to reach an
existing parent. SC does not choose a replacement directory automatically. A custom
zsh prompt may display `.` while the shell is inside a deleted directory; the panel
title still shows which directory was removed.

`SC_OTHER_DIR` is a remembered pathname rather than a live availability check. If its
directory is renamed or removed, commands that use the variable may fail until the
next panel switch updates it.

## Current limitations

- SC supports Linux, X11, and interactive zsh sessions.
- Panels show local filesystem directories; archive and virtual filesystem browsing
  are not supported yet.
- Directory contents refresh when the shell returns to a prompt, not continuously
  while another command is running.
- SC does not yet provide panel-driven copy, move, delete, or directory creation
  commands.
- Mouse-based file management is not supported.

## Community patches

SC does not include patches from the [st patches collection](https://st.suckless.org/patches/).
Users can apply them when needed. Two useful patches are:

- [Boxdraw](https://st.suckless.org/patches/boxdraw/) renders panel lines as crisp
  pixel primitives.
- [CSI 22/23](https://st.suckless.org/patches/csi_22_23/) preserves a window title
  changed temporarily by programs such as Neovim.

## License

See [LICENSE](LICENSE).
