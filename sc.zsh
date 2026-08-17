# zsh integration for SC. Source this after configuring PROMPT, for example:
#   [[ -n $SC_SOCKET ]] && source /path/to/sc.zsh

[[ -n ${SC_SOCKET-} ]] || return 0

typeset -g _sc_prompt_base=$PROMPT
typeset -g _sc_prompt_padding=0

# Enable SC's private input bindings only after ZLE has installed them below.

_scctl() {
    command "${SCCTL:-scctl}" "$@"
}

_sc_get_selected_entry() {
    local reply
    reply=$(_scctl selected) || return 1
    [[ $reply == $'D\t'* || $reply == $'F\t'* ]] || return 1
    _sc_selected_entry_kind=${reply%%$'\t'*}
    REPLY=${reply#*$'\t'}
}

_sc_update_prompt() {
    local reply pad prefix=''
    reply=$(_scctl padding "$_sc_prompt_padding") || return
    pad=${reply#*$'\t'}
    [[ $pad == <-> ]] || return  # exit if $pad is not a non-negative integer.
    repeat $pad; do prefix+=$'\n'; done
    _sc_prompt_padding=$pad
    PROMPT="${prefix}${_sc_prompt_base}"
}

_sc_precmd() {
    _sc_prompt_padding=0
    _sc_update_prompt
}
autoload -Uz add-zsh-hook
# _sc_precmd will be called just before each new shell prompt is printed.
add-zsh-hook precmd _sc_precmd

_sc_cwd_changed() {
    printf '\e]6771\a'
}
add-zsh-hook chpwd _sc_cwd_changed

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
    [[ $_sc_selected_entry_kind == D ]] || return
    builtin cd -- "$REPLY" || return
    _sc_refresh_prompt
}

# Appends the basename of the selected item to the current command line,
# properly shell-quoted.
_sc_insert_selected_name() {
    _sc_get_selected_entry || return
    local path=$REPLY
    BUFFER+="${(q)path:t}"
    CURSOR=${#BUFFER}
}

# Appends the full path of the selected item to the command line.
_sc_insert_selected_path() {
    _sc_get_selected_entry || return
    BUFFER+="${(q)${:-$PWD/$REPLY}}"
    CURSOR=${#BUFFER}
}

_sc_enter() {
    # If a command is typed, execute it normally.
    if [[ -n $BUFFER ]]; then
        zle .accept-line
        return
    fi

    # If not, enter the selected directory or execute the selected file.
    if _sc_get_selected_entry; then
        if [[ $_sc_selected_entry_kind == D ]]; then
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
zle -N _sc_enter

bindkey '\e[6770~' _sc_cd_parent
bindkey '\e[6771~' _sc_cd_child
bindkey '\e[6772~' _sc_insert_selected_name
bindkey '\e[6773~' _sc_insert_selected_path
bindkey '\e[6774~' _sc_refresh_prompt
bindkey '^M' _sc_enter
bindkey '^J' _sc_enter

# Emit OSC escape code to indicate SC zsh adapter ready.
printf '\e]6770\a'
