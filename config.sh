#!/usr/bin/env bash

# Minimal OS dynamically discovered module configuration TUI.

set -u

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
MODULE_ROOT="$SCRIPT_DIR/src/modules"
CONFIG_FILE="$SCRIPT_DIR/config.mk"

readonly RESET=$'\033[0m'
readonly BOLD=$'\033[1m'
readonly DIM=$'\033[2m'
readonly CYAN=$'\033[36m'
readonly GREEN=$'\033[32m'
readonly RED=$'\033[31m'
readonly YELLOW=$'\033[33m'

# Known relationships are keyed by relative paths. New modules default to no dependencies.
declare -A dependencies values visiting
 dependencies["Audio/audio.c"]="Audio/audio_manager.c Audio/ac97_driver.c"
dependencies["Audio/ac97_driver.c"]="Audio/audio_manager.c PCI/pci.c PCI/pci_write.c"
dependencies["GPU/gpu.c"]="GPU/gpu_manager.c PCI/pci.c"
dependencies["GPU/vgaterm.c"]="GPU/gpu_manager.c"
dependencies["PCI/pci.c"]="PCI/pci_manager.c PCI/pci_write.c GPU/gpu.c"
dependencies["USB/usb_manager.c"]="USB/usb/usb_stack.c USB/usb/usb_uhci.c"
dependencies["USB/usb/usb_stack.c"]="USB/usbkeyboard.c USB/usb/usb_uhci.c"
dependencies["USB/usb/usb_uhci.c"]="PCI/pci.c PCI/pci_write.c"
dependencies["USB/usbkeyboard.c"]="USB/usb/usb_stack.c"
dependencies["USB/unifiedkeyboardbridge.c"]="USB/usb/usb_stack.c USB/usbkeyboard.c"

files=()
categories=()
category_files=()
category_selected=0
module_selected=0
screen=categories

module_key() {
    local path=$1
    path=${path//\//_}
    path=${path//./_}
    printf 'MODULE_%s' "$path"
}

module_label() {
    local path=$1 name
    name=${path##*/}
    name=${name%.*}
    name=${name//_/ }
    printf '%s' "$name"
}

discover_modules() {
    local path category found
    files=()
    categories=()
    while IFS= read -r path; do
        files+=("$path")
        category=${path%%/*}
        found=0
        for existing in "${categories[@]}"; do
            [[ "$existing" == "$category" ]] && found=1 && break
        done
        (( found == 0 )) && categories+=("$category")
    done < <(cd "$MODULE_ROOT" && find . -type f \( -name '*.c' -o -name '*.asm' \) -printf '%P\n' | sort)

    if (( ${#files[@]} == 0 )); then
        printf 'Error: no module source files found under %s\n' "$MODULE_ROOT" >&2
        exit 1
    fi
}

read_config() {
    local path key value
    for path in "${files[@]}"; do
        key=$(module_key "$path")
        value=$(sed -n "s/^${key}[[:space:]]*:=[[:space:]]*\([01]\).*$/\1/p" "$CONFIG_FILE" | head -n 1)
        if [[ -z "$value" ]]; then
            value=1
        fi
        values[$path]=$value
    done
}

enable_with_dependencies() {
    local path=$1 dependency
    [[ ${visiting[$path]:-0} == 1 ]] && return
    visiting[$path]=1
    values[$path]=1
    for dependency in ${dependencies[$path]:-}; do
        [[ -v values[$dependency] ]] && enable_with_dependencies "$dependency"
    done
    visiting[$path]=0
}

resolve_dependencies() {
    local path
    visiting=()
    for path in "${files[@]}"; do
        [[ ${values[$path]} == 1 ]] && enable_with_dependencies "$path"
    done
}

disable_with_dependents() {
    local path=$1 candidate dependency
    values[$path]=0
    for candidate in "${files[@]}"; do
        [[ ${values[$candidate]} == 1 ]] || continue
        for dependency in ${dependencies[$candidate]:-}; do
            if [[ "$dependency" == "$path" ]]; then
                disable_with_dependents "$candidate"
                break
            fi
        done
    done
}

toggle_module() {
    local path=${category_files[$module_selected]}
    if [[ ${values[$path]} == 1 ]]; then
        disable_with_dependents "$path"
    else
        visiting=()
        enable_with_dependencies "$path"
    fi
}

set_paths() {
    local value=$1 path
    shift
    for path in "$@"; do
        if (( value == 1 )); then
            visiting=()
            enable_with_dependencies "$path"
        else
            disable_with_dependents "$path"
        fi
    done
}

write_config() {
    local temporary_file path key value
    temporary_file=$(mktemp "$CONFIG_FILE.tmp.XXXXXX") || return 1
    {
        printf '# Module Configuration for Minimal OS\n'
        printf '# Generated from source files under src/modules by config.sh.\n'
        printf '# Dependencies are resolved by config.sh before saving.\n\n'
        for path in "${files[@]}"; do
            key=$(module_key "$path")
            value=${values[$path]}
            printf '%s := %s\n' "$key" "$value"
        done
    } > "$temporary_file"
    if ! mv -- "$temporary_file" "$CONFIG_FILE"; then
        rm -f -- "$temporary_file"
        return 1
    fi
}

restore_terminal() {
    printf '\033[?25h\033[0m\033[?1049l'
}

quit_without_saving() {
    restore_terminal
    printf 'Configuration unchanged.\n'
    exit 0
}

trap restore_terminal EXIT
trap 'exit 130' INT TERM

refresh_category_files() {
    local path category=${categories[$category_selected]}
    category_files=()
    for path in "${files[@]}"; do
        [[ ${path%%/*} == "$category" ]] && category_files+=("$path")
    done
    (( module_selected < ${#category_files[@]} )) || module_selected=0
}

draw_categories() {
    local index category enabled path
    printf '\033[2J\033[H'
    printf '%s%sMinimal OS  /  Module Categories%s\n' "$BOLD" "$CYAN" "$RESET"
    printf '%s--------------------------------------------------------%s\n' "$DIM" "$RESET"
    printf '  %sSelect a category to configure its discovered modules.%s\n\n' "$DIM" "$RESET"
    for index in "${!categories[@]}"; do
        category=${categories[$index]}
        enabled=0
        for path in "${files[@]}"; do
            [[ ${path%%/*} == "$category" && ${values[$path]} == 1 ]] && (( enabled++ ))
        done
        if (( index == category_selected )); then
            printf ' %s> %-18s %s(%d/%d enabled)%s\n' "$CYAN" "$category" "$DIM" "$enabled" \
                "$(printf '%s\n' "${files[@]}" | awk -F/ -v category="$category" '$1 == category { count++ } END { print count + 0 }')" "$RESET"
        else
            printf '   %-18s %s(%d/%d enabled)%s\n' "$category" "$DIM" "$enabled" \
                "$(printf '%s\n' "${files[@]}" | awk -F/ -v category="$category" '$1 == category { count++ } END { print count + 0 }')" "$RESET"
        fi
    done
    printf '\n%s--------------------------------------------------------%s\n' "$DIM" "$RESET"
    printf '  %sEnter%s open   %sA%s all on   %sN%s all off   %sQ%s cancel\n' \
        "$BOLD" "$RESET" "$BOLD" "$RESET" "$BOLD" "$RESET" "$BOLD" "$RESET"
}

draw_modules() {
    local index path value marker color dependency_text
    printf '\033[2J\033[H'
    printf '%s%sMinimal OS  /  %s Modules%s\n' "$BOLD" "$CYAN" "${categories[$category_selected]}" "$RESET"
    printf '%s--------------------------------------------------------%s\n' "$DIM" "$RESET"
    printf '  %sDiscovered source files in this category.%s\n\n' "$DIM" "$RESET"
    for index in "${!category_files[@]}"; do
        path=${category_files[$index]}
        marker=' '
        [[ $index == $module_selected ]] && marker='>'
        value=${values[$path]}
        if (( value == 1 )); then color=$GREEN; value='ON '; else color=$RED; value='OFF'; fi
        printf ' %s %s%-2s%s  %-24s %s%s%s\n' "$CYAN$marker" "$color" "$value" "$RESET" \
            "$(module_label "$path")" "$DIM" "$path" "$RESET"
    done
    dependency_text=${dependencies[${category_files[$module_selected]}]:-none}
    printf '\n%s--------------------------------------------------------%s\n' "$DIM" "$RESET"
    printf '  %sDependencies:%s %s\n' "$BOLD" "$RESET" "$dependency_text"
    printf '  %sSpace%s toggle   %sA%s category on   %sN%s category off   %sB%s back   %sEnter%s apply\n' \
        "$BOLD" "$RESET" "$BOLD" "$RESET" "$BOLD" "$RESET" "$BOLD" "$RESET" "$BOLD" "$RESET"
}

main() {
    local input path
    [[ -f "$CONFIG_FILE" ]] || : > "$CONFIG_FILE"
    [[ -d "$MODULE_ROOT" ]] || { printf 'Error: module directory not found: %s\n' "$MODULE_ROOT" >&2; exit 1; }
    if [[ ! -t 0 || ! -t 1 ]]; then
        printf 'Error: config.sh requires an interactive terminal.\n' >&2
        exit 1
    fi

    discover_modules
    read_config
    resolve_dependencies
    printf '\033[?1049h\033[?25l'

    while true; do
        if [[ "$screen" == categories ]]; then
            draw_categories
        else
            refresh_category_files
            draw_modules
        fi
        IFS= read -r -s -n 1 input || break
        case "$input" in
            $'\033')
                IFS= read -r -s -n 2 input
                case "$input" in
                    '[A')
                        if [[ "$screen" == categories ]]; then (( category_selected > 0 )) && (( category_selected-- )); else (( module_selected > 0 )) && (( module_selected-- )); fi ;;
                    '[B')
                        if [[ "$screen" == categories ]]; then (( category_selected < ${#categories[@]} - 1 )) && (( category_selected++ )); else (( module_selected < ${#category_files[@]} - 1 )) && (( module_selected++ )); fi ;;
                esac ;;
            k|K)
                if [[ "$screen" == categories ]]; then (( category_selected > 0 )) && (( category_selected-- )); else (( module_selected > 0 )) && (( module_selected-- )); fi ;;
            j|J)
                if [[ "$screen" == categories ]]; then (( category_selected < ${#categories[@]} - 1 )) && (( category_selected++ )); else (( module_selected < ${#category_files[@]} - 1 )) && (( module_selected++ )); fi ;;
            ' ')
                [[ "$screen" == modules ]] && toggle_module ;;
            a|A)
                if [[ "$screen" == categories ]]; then set_paths 1 "${files[@]}"; else set_paths 1 "${category_files[@]}"; fi ;;
            n|N)
                if [[ "$screen" == categories ]]; then set_paths 0 "${files[@]}"; else set_paths 0 "${category_files[@]}"; fi ;;
            b|B) screen=categories ;;
            q|Q) quit_without_saving ;;
            '')
                if [[ "$screen" == categories ]]; then screen=modules; module_selected=0; else
                    if write_config; then restore_terminal; trap - EXIT; printf 'Configuration saved to %s.\n' "$CONFIG_FILE"; exit 0; fi
                fi ;;
        esac
    done
}

main "$@"
