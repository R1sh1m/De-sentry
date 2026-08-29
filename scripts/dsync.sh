#!/usr/bin/env bash
# ==============================================================================
#  dsync.sh  —  De-Sentry Safe Collaboration Script  (Linux / macOS / Git Bash)
#
#  Usage:
#    ./scripts/dsync.sh init                     # one-time repo setup
#    ./scripts/dsync.sh pull                     # safe pull (stash→rebase→restore)
#    ./scripts/dsync.sh push "your message"      # pull first, then commit & push
#    ./scripts/dsync.sh push                     # prompts for message interactively
#    ./scripts/dsync.sh status                   # rich status overview
#    ./scripts/dsync.sh branch <name>            # create a properly-named branch
#    ./scripts/dsync.sh log                      # pretty commit graph
#    ./scripts/dsync.sh help                     # show this help
#
#  Make executable once:  chmod +x ./scripts/dsync.sh
#  Optional alias:        alias dsync="./scripts/dsync.sh"
# ==============================================================================

set -euo pipefail

# ── Colour helpers ─────────────────────────────────────────────────────────────
RED='\033[0;31m'; YELLOW='\033[1;33m'; GREEN='\033[0;32m'
CYAN='\033[0;36m'; WHITE='\033[1;37m'; GRAY='\033[0;90m'; RESET='\033[0m'

c_header() { echo -e "\n${CYAN}$1${RESET}"; }
c_step()   { echo -e "  ${WHITE}→ $1${RESET}"; }
c_ok()     { echo -e "  ${GREEN}✓ $1${RESET}"; }
c_warn()   { echo -e "  ${YELLOW}⚠ $1${RESET}"; }
c_error()  { echo -e "  ${RED}✗ $1${RESET}"; }
c_info()   { echo -e "  ${GRAY}· $1${RESET}"; }
c_bold()   { echo -e "${WHITE}$1${RESET}"; }
divider()  { echo -e "${GRAY}$(printf '─%.0s' {1..60})${RESET}"; }

# ── Git helpers ────────────────────────────────────────────────────────────────
get_branch()         { git rev-parse --abbrev-ref HEAD 2>/dev/null | tr -d '\n'; }
assert_in_repo()     { git rev-parse --show-toplevel > /dev/null 2>&1 || { c_error "Not inside a git repository."; exit 1; }; }
has_uncommitted()    { [[ -n "$(git status --porcelain 2>/dev/null)" ]]; }
get_stash_count()    { git stash list 2>/dev/null | wc -l | tr -d ' '; }

PROTECTED_BRANCHES=("main" "master" "dev" "release")
FORCE=${FORCE:-0}

assert_not_protected() {
    local branch; branch=$(get_branch)
    for pb in "${PROTECTED_BRANCHES[@]}"; do
        if [[ "$branch" == "$pb" && "$FORCE" -ne 1 ]]; then
            c_error "You are on the protected branch '$branch'."
            c_warn  "Direct pushes to '$branch' are blocked to prevent accidents."
            c_info  "Create a feature branch:  ./scripts/dsync.sh branch <name>"
            c_info  "Or set FORCE=1 to override (NOT recommended for shared branches)."
            exit 1
        fi
    done
}

# ── Command: init ──────────────────────────────────────────────────────────────
cmd_init() {
    assert_in_repo
    c_header "De-Sentry: Repo Initialisation"
    divider

    c_step "Configuring pull strategy: rebase (no merge commits)"
    git config pull.rebase true
    c_ok   "pull.rebase = true"

    c_step "Configuring push: track current branch automatically"
    git config push.autoSetupRemote true 2>/dev/null || git config push.default current
    c_ok   "push tracking configured"

    c_step "Enabling reuse-recorded-resolution (rerere)"
    git config rerere.enabled true
    c_ok   "rerere.enabled = true"

    c_step "Checking your git identity..."
    local name email
    name=$(git config user.name  2>/dev/null || echo "")
    email=$(git config user.email 2>/dev/null || echo "")
    if [[ -n "$name" && -n "$email" ]]; then
        c_ok  "Identity: $name <$email>"
    else
        c_warn "No git identity set. Run:"
        c_info "  git config --global user.name  'Your Name'"
        c_info "  git config --global user.email 'you@email.com'"
    fi

    c_step "Making dsync.sh executable..."
    chmod +x ./scripts/dsync.sh 2>/dev/null || true
    c_ok   "Executable bit set"

    c_step "Fetching latest remote state..."
    git fetch --all --prune
    c_ok   "Remote refs updated"

    divider
    c_bold "\nSetup complete. You're ready to use dsync.\n"
    c_info "Quick reference:"
    c_info "  ./scripts/dsync.sh pull            # safe pull"
    c_info "  ./scripts/dsync.sh push 'message'  # commit + push"
    c_info "  ./scripts/dsync.sh branch my-feat  # new branch"
    c_info "  ./scripts/dsync.sh status          # overview"
}

# ── Command: pull ──────────────────────────────────────────────────────────────
cmd_pull() {
    assert_in_repo
    local branch; branch=$(get_branch)
    c_header "De-Sentry: Safe Pull  [$branch]"
    divider

    # 1. Stash if dirty
    local stashed=0
    if has_uncommitted; then
        c_step "Uncommitted changes detected — stashing..."
        local stash_msg="dsync-autostash-$(date +%Y%m%d-%H%M%S)"
        git stash push -u -m "$stash_msg"
        stashed=1
        c_ok   "Stashed as: $stash_msg"
    else
        c_info "Working tree is clean — no stash needed"
    fi

    # 2. Fetch
    c_step "Fetching from origin..."
    git fetch --all --prune 2>&1 | while IFS= read -r line; do c_info "$line"; done
    c_ok   "Fetch complete"

    # 3. Rebase
    c_step "Rebasing onto origin/$branch..."
    if ! git rebase "origin/$branch" 2>&1; then
        c_error "Rebase conflict detected!"
        divider
        echo ""
        c_warn "Conflicting files:"
        git diff --name-only --diff-filter=U 2>/dev/null | while IFS= read -r f; do
            echo -e "    ${YELLOW}$f${RESET}"
        done
        echo ""
        c_bold "Resolution steps:"
        c_info "  1. Open each file above and resolve conflict markers (<<<<, ====, >>>>)"
        c_info "  2. Run:  git add <resolved-file>"
        c_info "  3. Run:  git rebase --continue"
        c_info "  --- OR ---"
        c_info "  To abandon and go back to before the pull:"
        c_info "  Run:  git rebase --abort"
        if [[ $stashed -eq 1 ]]; then
            c_warn "Your stash is still saved. After resolving, run: git stash pop"
        fi
        divider
        exit 1
    fi
    c_ok "Rebase successful"

    # 4. Restore stash
    if [[ $stashed -eq 1 ]]; then
        c_step "Restoring your stashed changes..."
        if ! git stash pop 2>&1; then
            c_warn "Stash pop had conflicts — changes are in stash[0]"
            c_info "Run 'git stash show -p' to inspect"
        else
            c_ok   "Stashed changes restored"
        fi
    fi

    # 5. Summary
    divider
    local ahead behind
    ahead=$(git rev-list "origin/$branch..HEAD" --count 2>/dev/null || echo 0)
    c_ok   "Pull complete. Branch '$branch' is up to date."
    [[ "$ahead" -gt 0 ]] && c_warn "You are $ahead commit(s) ahead of origin — remember to push."
    echo ""
}

# ── Command: push ──────────────────────────────────────────────────────────────
cmd_push() {
    local message="${1:-}"
    assert_in_repo
    assert_not_protected

    local branch; branch=$(get_branch)
    c_header "De-Sentry: Safe Push  [$branch]"
    divider

    # 1. Always pull first
    c_step "Syncing with remote before push..."
    cmd_pull
    c_ok   "Sync complete"

    # 2. Check if there's anything to commit
    local ahead
    ahead=$(git rev-list "origin/$branch..HEAD" --count 2>/dev/null || echo 0)

    if ! has_uncommitted; then
        if [[ "$ahead" -eq 0 ]]; then
            c_info "Nothing to commit and nothing to push — already up to date."
            echo ""; return
        else
            c_info "Nothing new to commit, but $ahead local commit(s) will be pushed."
        fi
    else
        # 3. Show diff summary
        c_step "Changes to be committed:"
        git status --short | while IFS= read -r line; do
            local flag="${line:0:2}" file="${line:3}"
            local col="$GRAY"
            [[ "$flag" =~ ^M ]] && col="$YELLOW"
            [[ "$flag" =~ ^A ]] && col="$GREEN"
            [[ "$flag" =~ ^D ]] && col="$RED"
            echo -e "    ${col}${flag} ${file}${RESET}"
        done
        echo ""

        # 4. Get commit message
        if [[ -z "$message" ]]; then
            echo -ne "  ${CYAN}Enter commit message (Ctrl+C to cancel): ${RESET}"
            read -r message
        fi
        if [[ -z "${message// }" ]]; then
            c_error "Commit message cannot be empty."
            exit 1
        fi

        # 5. Stage and commit
        c_step "Staging all changes..."
        git add -A
        c_ok   "All changes staged"

        c_step "Committing: '$message'"
        git commit -m "$message"
        c_ok   "Committed"
    fi

    # 6. Push
    c_step "Pushing to origin/$branch..."
    if ! git push 2>&1; then
        c_warn "Push failed — trying to set upstream..."
        if ! git push --set-upstream origin "$branch" 2>&1; then
            c_error "Push failed. Check your connection and permissions."
            exit 1
        fi
    fi
    c_ok "Pushed to origin/$branch"

    # 7. Summary
    divider
    local latest; latest=$(git log --oneline -1 2>/dev/null)
    c_ok "Push complete! Latest commit: $latest"
    c_info "View on GitHub: https://github.com/R1sh1m/De-sentry/tree/$branch"
    echo ""
}

# ── Command: status ────────────────────────────────────────────────────────────
cmd_status() {
    assert_in_repo
    local branch; branch=$(get_branch)

    c_header "De-Sentry: Workspace Status"
    divider

    local ahead behind
    ahead=$(git rev-list  "origin/$branch..HEAD" --count 2>/dev/null || echo 0)
    behind=$(git rev-list "HEAD..origin/$branch" --count 2>/dev/null || echo 0)

    echo -e "  ${GRAY}Branch   ${CYAN}${branch}${RESET}"
    echo -e "  ${GRAY}Remote   origin/${branch}${RESET}"

    if [[ "$ahead" -gt 0 || "$behind" -gt 0 ]]; then
        echo -ne "  ${GRAY}Sync     ${RESET}"
        [[ "$ahead"  -gt 0 ]] && echo -ne "${YELLOW}↑${ahead} ahead  ${RESET}"
        [[ "$behind" -gt 0 ]] && echo -ne "${RED}↓${behind} behind${RESET}"
        echo ""
    else
        echo -e "  ${GRAY}Sync     ${GREEN}Up to date ✓${RESET}"
    fi

    local stash_count; stash_count=$(get_stash_count)
    [[ "$stash_count" -gt 0 ]] && echo -e "  ${GRAY}Stashes  ${YELLOW}${stash_count} saved${RESET}"
    echo ""

    if has_uncommitted; then
        echo -e "  ${WHITE}Changed files:${RESET}"
        git status --short | while IFS= read -r line; do
            local flag="${line:0:2}" file="${line:3}"
            local col="$GRAY"
            [[ "$flag" =~ ^M ]] && col="$YELLOW"
            [[ "$flag" =~ ^A ]] && col="$GREEN"
            [[ "$flag" =~ ^D ]] && col="$RED"
            echo -e "    ${col}${flag} ${file}${RESET}"
        done
    else
        echo -e "  ${GRAY}Working tree: ${GREEN}clean${RESET}"
    fi

    echo ""
    echo -e "  ${WHITE}Recent commits:${RESET}"
    git log --oneline --graph --decorate -8 2>/dev/null | while IFS= read -r line; do
        echo -e "    ${GRAY}${line}${RESET}"
    done

    echo ""
    echo -e "  ${WHITE}Remote branches:${RESET}"
    git branch -r 2>/dev/null | while IFS= read -r b; do
        echo -e "    ${GRAY}${b}${RESET}"
    done

    divider; echo ""
}

# ── Command: branch ────────────────────────────────────────────────────────────
cmd_branch() {
    local name="${1:-}"
    assert_in_repo

    if [[ -z "$name" ]]; then
        c_error "Please provide a branch name."
        c_info  "Usage: ./scripts/dsync.sh branch <name>"
        c_info  "Examples:"
        c_info  "  ./scripts/dsync.sh branch node-a/change-ledger"
        c_info  "  ./scripts/dsync.sh branch feat/routing-algo"
        exit 1
    fi

    # Sanitise: lowercase, spaces→dashes, strip bad chars
    local safe_name
    safe_name=$(echo "$name" | tr '[:upper:]' '[:lower:]' | tr ' ' '-' | tr -cd 'a-z0-9/_-')
    [[ "$safe_name" != "$name" ]] && c_warn "Branch name sanitised: '$name' → '$safe_name'"

    c_header "De-Sentry: Create Branch"
    divider

    c_step "Fetching latest remote state..."
    git fetch --all --prune 2>/dev/null
    c_ok   "Fetched"

    c_step "Creating branch '$safe_name' from origin/main..."
    if ! git checkout -b "$safe_name" "origin/main" 2>&1; then
        c_error "Branch creation failed. Does '$safe_name' already exist?"
        exit 1
    fi
    c_ok   "Switched to new branch '$safe_name'"

    c_step "Pushing branch to origin to track it..."
    git push --set-upstream origin "$safe_name" 2>&1
    c_ok   "Branch '$safe_name' tracked on origin"

    divider
    c_bold "\nYou are now on branch: $safe_name"
    c_info "Make your changes, then: ./scripts/dsync.sh push 'your message'"
    echo ""
}

# ── Command: log ───────────────────────────────────────────────────────────────
cmd_log() {
    assert_in_repo
    c_header "De-Sentry: Commit History"
    divider
    git log --graph \
        --pretty=format:"%C(yellow)%h%Creset %C(cyan)%ad%Creset %C(white)%s%Creset %C(dim)— %an%Creset" \
        --date=short -20 2>/dev/null
    echo ""
    divider; echo ""
}

# ── Command: help ──────────────────────────────────────────────────────────────
cmd_help() {
    echo -e ""
    echo -e "  ${CYAN}De-Sentry  dsync  —  Safe Collaboration Script${RESET}"
    echo -e "  ${GRAY}─────────────────────────────────────────────────────────${RESET}"
    echo -e ""
    echo -e "  ${WHITE}COMMANDS${RESET}"
    echo -e "    ${GRAY}init              One-time repo setup (run this first!)${RESET}"
    echo -e "    ${GRAY}pull              Safe pull: stash → rebase → restore${RESET}"
    echo -e "    ${GRAY}push [message]    Pull first, then commit all + push${RESET}"
    echo -e "    ${GRAY}status            Rich overview of branch + changes${RESET}"
    echo -e "    ${GRAY}branch <name>     Create a properly-named feature branch${RESET}"
    echo -e "    ${GRAY}log               Pretty commit graph (last 20)${RESET}"
    echo -e "    ${GRAY}help              Show this message${RESET}"
    echo -e ""
    echo -e "  ${WHITE}FLAGS${RESET}"
    echo -e "    ${GRAY}FORCE=1           Override branch protection (use carefully)${RESET}"
    echo -e ""
    echo -e "  ${WHITE}EXAMPLES${RESET}"
    echo -e "    ${YELLOW}./scripts/dsync.sh init${RESET}"
    echo -e "    ${YELLOW}./scripts/dsync.sh branch node-b/csv-storage-engine${RESET}"
    echo -e "    ${YELLOW}./scripts/dsync.sh push 'feat: add SHA-256 chain verification'${RESET}"
    echo -e "    ${YELLOW}./scripts/dsync.sh pull${RESET}"
    echo -e ""
    echo -e "  ${WHITE}BRANCH NAMING CONVENTION${RESET}"
    echo -e "    ${GRAY}node-a/<feature>    Work owned by Node A team member${RESET}"
    echo -e "    ${GRAY}node-b/<feature>    Work owned by Node B team member${RESET}"
    echo -e "    ${GRAY}node-c/<feature>    Work owned by Node C team member${RESET}"
    echo -e "    ${GRAY}feat/<feature>      Shared feature (coordinate with team)${RESET}"
    echo -e "    ${GRAY}fix/<bug>           Bug fix${RESET}"
    echo -e "    ${GRAY}docs/<topic>        Documentation only${RESET}"
    echo -e ""
    echo -e "  ${WHITE}PROTECTED BRANCHES  (direct push blocked)${RESET}"
    echo -e "    ${RED}main   dev   release${RESET}"
    echo -e ""
}

# ── Router ─────────────────────────────────────────────────────────────────────
COMMAND="${1:-help}"
shift || true

case "$COMMAND" in
    init)   cmd_init ;;
    pull)   cmd_pull ;;
    push)   cmd_push "${1:-}" ;;
    status) cmd_status ;;
    branch) cmd_branch "${1:-}" ;;
    log)    cmd_log ;;
    help|--help|-h) cmd_help ;;
    *)
        c_error "Unknown command: '$COMMAND'"
        cmd_help
        exit 1
        ;;
esac
