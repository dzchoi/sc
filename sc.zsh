# zsh integration for SC. Auto-loaded via a generated .zshenv that SC drops into its
# private runtime directory and points $ZDOTDIR at. Not intended to be sourced manually.

# Only the generated shim supplies SC_ZSH_INIT; later manual sources are no-ops.
[[ -n "${SC_ZSH_INIT-}" ]] || return 0

# ---- User-command defaults (replace SC_USER_COMMANDS in .zshrc if needed) ---

# Maps ZLE key sequences to a command for the selected entry. A standalone `{}` is
# replaced by the entry's absolute path; when omitted, the path is appended.
#
# Key sequences (see config.h for more):
# F1–F4:       \eOP, \eOQ, \eOR, \eOS
# Shift+F1–F4: \e[1;2P, \e[1;2Q, \e[1;2R, \e[1;2S
# F5–F12:      \e[15~, \e[17~, \e[18~, \e[19~, \e[20~, \e[21~, \e[23~, \e[24~
# Shift+F5–F12 use the corresponding number followed by ;2~
typeset -gA SC_USER_COMMANDS=(
    $'\eOR'     'less -- {}'  # F3
    $'\eOS'     'vi -- {}'    # F4
    $'\e[1;2R'  'cat {}'      # Shift+F3 (for testing)
)

# ---- Shim: restore ZDOTDIR and source the user's .zshenv --------------------
# Runs at .zshenv time (before .zshrc). Aliases from /etc/zshenv may be active, so
# command names used by the shim are quoted. Adapter widget installation is deferred
# until the first precmd, after the remaining startup files have run.

'builtin' 'unset' 'SC_ZSH_INIT'

# Distinguish unset from empty so nested zsh matches the user's original state.
if [[ -n "${SC_USER_ZDOTDIR+X}" ]]; then
    'builtin' 'export' 'ZDOTDIR'="$SC_USER_ZDOTDIR"
else
    'builtin' 'unset' 'ZDOTDIR'
fi
'builtin' 'unset' 'SC_USER_ZDOTDIR'

# We hijacked ZDOTDIR, so zsh skipped the user's own .zshenv. Source it now.
_sc_shim_file="${ZDOTDIR-$HOME}/.zshenv"
[[ -r "$_sc_shim_file" ]] && 'builtin' 'source' '--' "$_sc_shim_file"
'builtin' 'unset' '_sc_shim_file'

# Non-interactive shells and shells not launched by SC skip the adapter.
[[ -o 'interactive' && -n "${SC_SOCKET-}" ]] || return 0

# Keep SC's socket private to this shell so child commands and nested shells cannot
# attach to SC. Each request uses zsh builtins and closes its descriptor before return.
typeset -g +x SC_SOCKET

'builtin' 'zmodload' zsh/net/socket || return 1
'builtin' 'zmodload' zsh/system || return 1

# ---- Adapter functions (installed later by _sc_bootstrap) -------------------

_scctl() {
    [[ -n ${SC_SOCKET-} && $# -gt 0 ]] || { REPLY=''; return 1; }

    local request="${(j: :)@}" fd chunk response=''
    local -i read_status

    'builtin' 'zsocket' -- "$SC_SOCKET" 2>/dev/null || { REPLY=''; return 1; }
    fd=$REPLY

    if ! 'builtin' 'print' -r -u "$fd" -- "$request"; then
        'builtin' 'exec' {fd}>&-
        REPLY=''
        return 1
    fi

    while (( 1 )); do
        'builtin' 'sysread' -i "$fd" -s 4096 chunk
        read_status=$?
        (( read_status == 0 )) || break
        response+=$chunk
    done
    'builtin' 'exec' {fd}>&-

    # Status 5 is EOF. A recognized request always returns a nonempty response and EOF
    # frames it.
    if (( read_status != 5 )) || [[ -z $response ]]; then
        REPLY=''
        return 1
    fi

    REPLY=$response
}

_sc_get_selected_entry() {
    _scctl selected && [[ -n $REPLY ]]
}

_sc_update_prompt() {
    local prefix=''
    _scctl preprompt "$_sc_prompt_padding" || REPLY=0
    repeat $REPLY; do prefix+=$'\n'; done
    _sc_prompt_padding=$REPLY
    PROMPT="${prefix}${_sc_prompt_base}"
}

_sc_precmd() {
    _sc_prompt_padding=0
    _sc_update_prompt
}

# Must be called immediately following a shell command to capture its status ($?).
# Refreshes the prompt while transparently passing that exit status back to the caller.
# Accepts an optional status via $1 if deferred capture is needed.
_sc_refresh_prompt() {
    local -i prev_status="${1:-$status}"

    if (( prev_status == 0 )); then
        _sc_update_prompt
        zle reset-prompt
    else
        _sc_prompt_padding=0
        PROMPT=$_sc_prompt_base
    fi

    return $prev_status
}

_sc_cd() {
    [[ -n "$1" ]] || return
    # Total line count of the current command line (== 0 usually).
    local -i prompt_lines=$BUFFERLINES
    zle -I && (( _sc_prompt_padding += prompt_lines ))
    builtin cd ${2:+"$2"} -- "$1"
    _sc_refresh_prompt
}

_sc_switch_panel() {
    _scctl focused_directory && _sc_cd "$REPLY" -P
}

_sc_cd_parent() {
    _sc_cd ".."
}

_sc_cd_child() {
    _sc_get_selected_entry || return
    [[ -d $REPLY ]] || return
    _sc_cd "$REPLY"
}

_sc_insert_at_cursor() {
    local insertion=$1
    BUFFER="${BUFFER[1,CURSOR]}${insertion}${BUFFER[CURSOR+1,-1]}"
    (( CURSOR += ${#insertion} ))
}

_sc_insert_selected_name() {
    _sc_get_selected_entry || return
    _sc_insert_at_cursor "${(q)REPLY:t}"
}

_sc_insert_selected_path() {
    _sc_get_selected_entry || return
    _sc_insert_at_cursor "${(q)${:-${PWD%/}/$REPLY}}"
}

_sc_run_user_command() {
    local action=${SC_USER_COMMANDS[$KEYS]-}
    [[ -n $action ]] || return

    local -a argv
    argv=(${(Q)${(z)action}})
    (( $#argv )) || return

    _sc_get_selected_entry || return

    local selected_path=${PWD%/}/$REPLY
    local -i replaced=0 i
    for (( i = 1; i <= $#argv; ++i )); do
        if [[ ${argv[i]} == '{}' ]]; then
            argv[i]=$selected_path
            replaced=1
        fi
    done
    (( replaced )) || argv+=("$selected_path")

    local -i prompt_lines=$BUFFERLINES
    zle -I && (( _sc_prompt_padding += prompt_lines ))
    # Each array element is passed as one argument, so no additional quoting is needed.
    command "${argv[@]}"
    _sc_refresh_prompt
}

_sc_enter() {
    # If a command is typed, execute it normally.
    if [[ -n $BUFFER ]]; then
        zle .accept-line
        return
    fi

    # A hidden panel has no active selection, so an empty line retains ZLE's ordinary
    # accept-line behavior.
    if ! _sc_get_selected_entry; then
        zle .accept-line
        return
    fi

    # Enter the selected directory or execute the selected file.
    if [[ -d $REPLY ]]; then
        _sc_cd "$REPLY"
    else
        BUFFER="${(q)${:-${PWD%/}/$REPLY}}"
        CURSOR=${#BUFFER}
        zle .accept-line
    fi
}

# ---- Bootstrap: install the adapter after the user's .zshrc has run ---------

autoload -Uz add-zsh-hook
_sc_bootstrap() {
    add-zsh-hook -d precmd _sc_bootstrap
    unfunction _sc_bootstrap

    typeset -g _sc_prompt_base=$PROMPT
    typeset -g _sc_prompt_padding=0

    add-zsh-hook precmd _sc_precmd

    zle -N _sc_refresh_prompt
    zle -N _sc_cd_parent
    zle -N _sc_cd_child
    zle -N _sc_insert_selected_name
    zle -N _sc_insert_selected_path
    zle -N _sc_run_user_command
    zle -N _sc_enter
    zle -N _sc_switch_panel

    bindkey '\e[6770~' _sc_cd_parent
    bindkey '\e[6771~' _sc_cd_child
    bindkey '\e[6772~' _sc_insert_selected_name
    bindkey '\e[6773~' _sc_insert_selected_path
    bindkey '\e[6774~' _sc_refresh_prompt
    bindkey '\e[6775~' _sc_switch_panel
    bindkey '^M' _sc_enter
    bindkey '^J' _sc_enter

    local key
    for key in "${(@k)SC_USER_COMMANDS}"; do
        bindkey "$key" _sc_run_user_command
    done

    # First preprompt sends the padding request that Shell::init() awaits.
    _sc_precmd
}
add-zsh-hook precmd _sc_bootstrap
