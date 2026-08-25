# zsh integration for SC. Source this after configuring PROMPT, for example:
#   [[ -n $SC_SOCKET ]] && source /path/to/sc.zsh

[[ -n ${SC_SOCKET-} ]] || return 0

typeset -g _sc_prompt_base=$PROMPT
typeset -g _sc_prompt_padding=0

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

# Enable SC's private input bindings only after ZLE has installed them below.

_scctl() {
    command "${SCCTL:-scctl}" "$@"
}

_sc_get_selected_entry() {
    local reply
    reply=$(_scctl selected) || return 1
    [[ -n $reply ]] || return 1
    REPLY=$reply
}

_sc_update_prompt() {
    local prefix='' reply=$(_scctl preprompt "$_sc_prompt_padding")
    reply=${reply:-0}
    repeat $reply; do prefix+=$'\n'; done
    _sc_prompt_padding=$reply
    PROMPT="${prefix}${_sc_prompt_base}"
}

_sc_precmd() {
    _sc_prompt_padding=0
    _sc_update_prompt
}
autoload -Uz add-zsh-hook
# _sc_precmd will be called just before each new shell prompt is printed.
add-zsh-hook precmd _sc_precmd

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
    builtin cd -- "$1"
    _sc_refresh_prompt
}

_sc_switch_panel() {
    _sc_cd "$(_scctl focused_cwd)"
}

_sc_cd_parent() {
    _sc_cd ".."
}

_sc_cd_child() {
    _sc_get_selected_entry || return
    [[ -d $REPLY ]] || return
    _sc_cd "$REPLY"
}

_sc_insert_selected_name() {
    _sc_get_selected_entry || return
    local selected_path=$REPLY
    BUFFER+="${(q)selected_path:t}"
    CURSOR=${#BUFFER}
}

_sc_insert_selected_path() {
    _sc_get_selected_entry || return
    BUFFER+="${(q)${:-${PWD%/}/$REPLY}}"
    CURSOR=${#BUFFER}
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

# Register Zsh Line Editor (ZLE) widgets.
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
() {
    local key
    for key in "${(@k)SC_USER_COMMANDS}"; do
        bindkey "$key" _sc_run_user_command
    done
}
