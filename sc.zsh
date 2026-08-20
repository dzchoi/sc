# zsh integration for SC. Source this after configuring PROMPT, for example:
#   [[ -n $SC_SOCKET ]] && source /path/to/sc.zsh

[[ -n ${SC_SOCKET-} ]] || return 0

typeset -g _sc_prompt_base=$PROMPT
typeset -g _sc_prompt_padding=0

# Maps ZLE key sequences to user commands for the selected entry. A standalone `{}` is
# replaced by its absolute path; when omitted, the path is appended.
typeset -gA SC_USER_COMMANDS=(
    $'\eOR' 'less -- {}'        # F3
    $'\eOS' 'vi -- {}'          # F4
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
    local reply prefix=''
    reply=$(_scctl preprompt "$_sc_prompt_padding") || return
    [[ $reply == <-> ]] || return  # exit if reply is not a non-negative integer.
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

_sc_refresh_prompt() {
    _sc_update_prompt
    zle reset-prompt
}

_sc_cd_parent() {
    builtin cd -- .. || return
    _sc_refresh_prompt
}

_sc_cd_child() {
    _sc_get_selected_entry || return
    [[ -d $REPLY ]] || return
    builtin cd -- "$REPLY" || return
    _sc_refresh_prompt
}

# Appends the basename of the selected item to the current command line,
# properly shell-quoted.
_sc_insert_selected_name() {
    _sc_get_selected_entry || return
    local selected_path=$REPLY
    BUFFER+="${(q)selected_path:t}"
    CURSOR=${#BUFFER}
}

# Appends the full path of the selected item to the command line.
_sc_insert_selected_path() {
    _sc_get_selected_entry || return
    BUFFER+="${(q)${:-$PWD/$REPLY}}"
    CURSOR=${#BUFFER}
}

_sc_run_user_command() {
    local action=${SC_USER_COMMANDS[$KEYS]-}
    [[ -n $action ]] || return
    _sc_get_selected_entry || return

    local -a argv
    argv=(${(Q)${(z)action}})
    (( $#argv )) || return

    # `path` is Zsh's special array tied to PATH.
    local selected_path=$PWD/$REPLY
    local -i replaced=0 i
    for (( i = 1; i <= $#argv; ++i )); do
        if [[ ${argv[i]} == '{}' ]]; then
            argv[i]=$selected_path
            replaced=1
        fi
    done
    (( replaced )) || argv+=("$selected_path")

    # The redisplay after command output must not reuse this prompt's placement lines.
    _sc_prompt_padding=0
    PROMPT=$_sc_prompt_base
    zle -I
    # Each array element is passed as one argument, so no additional quoting is needed.
    command "${argv[@]}"
}

_sc_enter() {
    # If a command is typed, execute it normally.
    if [[ -n $BUFFER ]]; then
        zle .accept-line
        return
    fi

    # If not, enter the selected directory or execute the selected file.
    if _sc_get_selected_entry; then
        if [[ -d $REPLY ]]; then
            builtin cd -- "$REPLY" && _sc_refresh_prompt
        else
            BUFFER="${(q)${:-$PWD/$REPLY}}"
            CURSOR=${#BUFFER}
            zle .accept-line
        fi
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

bindkey '\e[6770~' _sc_cd_parent
bindkey '\e[6771~' _sc_cd_child
bindkey '\e[6772~' _sc_insert_selected_name
bindkey '\e[6773~' _sc_insert_selected_path
bindkey '\e[6774~' _sc_refresh_prompt
bindkey '^M' _sc_enter
bindkey '^J' _sc_enter
() {
    local key
    for key in "${(@k)SC_USER_COMMANDS}"; do
        bindkey "$key" _sc_run_user_command
    done
}
