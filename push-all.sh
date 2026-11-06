#!/usr/bin/env bash
# Commits and pushes every repo: new, changed and deleted files (anything
# .gitignore lists stays out). Submodules go first, so each parent commit
# records its submodules' new commits.
#
#   ./push-all.sh             write each commit message in the editor
#   ./push-all.sh "message"   use this message for every repo
set -euo pipefail
cd "$(dirname "$0")"

# Innermost first. MOSdesk's own Minimal-OS / Minimal-OS-SDK folders are
# extra copies of these repos and are not pushed from.
repos=(Minimal-OS-SDK/libraries/minimaSSL Minimal-OS-SDK MOSdesk .)
message="${1:-}"

for repo in "${repos[@]}"; do
    name=$([ "$repo" = . ] && echo Minimal-OS || echo "$repo")
    if [ ! -e "$repo/.git" ]; then
        echo "== $name: not checked out, skipped"
        continue
    fi
    echo "== $name"
    git -C "$repo" add -A
    branch=$(git -C "$repo" branch --show-current)
    if [ -z "$branch" ]; then
        if git -C "$repo" diff --cached --quiet; then
            echo "detached HEAD with no changes, skipped"
            git -C "$repo" reset -q
            continue
        fi
        echo "$name: detached HEAD with changes; check out a branch first:" >&2
        echo "  git -C $repo switch main" >&2
        exit 1
    fi
    if git -C "$repo" diff --cached --quiet; then
        echo "nothing to commit"
    elif [ -n "$message" ]; then
        git -C "$repo" commit -m "$message"
    else
        git -C "$repo" commit
    fi
    git -C "$repo" push -u origin "$branch"
done
