#!/usr/bin/env python3
"""
dsync.py - De-Sentry Safe Collaboration Script
Cross-platform: Windows, macOS, Linux

Usage:
    python scripts/dsync.py init
    python scripts/dsync.py pull
    python scripts/dsync.py push "your commit message"
    python scripts/dsync.py push                        (interactive prompt)
    python scripts/dsync.py status
    python scripts/dsync.py branch node-a/my-feature
    python scripts/dsync.py log
    python scripts/dsync.py help

Shortcuts (from repo root):
    Windows:   .\\scripts\\dsync.ps1 <command>
    Mac/Linux: ./scripts/dsync.sh   <command>
"""

import sys
import os
import subprocess
import platform
from datetime import datetime

# ── Enable ANSI colours + UTF-8 on Windows ───────────────────────────────────
if platform.system() == "Windows":
    # Enable VT100 ANSI processing
    try:
        import ctypes
        kernel32 = ctypes.windll.kernel32
        kernel32.SetConsoleMode(kernel32.GetStdHandle(-11), 7)
    except Exception:
        pass
    # Force UTF-8 output so Unicode chars don't crash on cp1252 consoles
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

# ── Colour codes ───────────────────────────────────────────────────────────────
RED    = "\033[0;31m"
YELLOW = "\033[1;33m"
GREEN  = "\033[0;32m"
CYAN   = "\033[0;36m"
WHITE  = "\033[1;37m"
GRAY   = "\033[0;90m"
BOLD   = "\033[1m"
RESET  = "\033[0m"

# ── Constants ──────────────────────────────────────────────────────────────────
GITHUB_REPO        = "https://github.com/R1sh1m/De-sentry"
PROTECTED_BRANCHES = {"main", "master", "dev", "release"}

# ── Print helpers ──────────────────────────────────────────────────────────────
def c_header(t): print(f"\n{CYAN}{t}{RESET}")
def c_step(t):   print(f"  {WHITE}-> {t}{RESET}")
def c_ok(t):     print(f"  {GREEN}OK  {t}{RESET}")
def c_warn(t):   print(f"  {YELLOW}!!  {t}{RESET}")
def c_error(t):  print(f"  {RED}XX  {t}{RESET}")
def c_info(t):   print(f"  {GRAY}.   {t}{RESET}")
def c_bold(t):   print(f"{WHITE}{t}{RESET}")
def divider():   print(f"{GRAY}{'-' * 60}{RESET}")

def file_colour(flag: str) -> str:
    f = flag.strip()
    if f.startswith("M"): return YELLOW
    if f.startswith("A"): return GREEN
    if f.startswith("D"): return RED
    if f.startswith("?"): return GRAY
    return WHITE

# ── Git subprocess helpers ─────────────────────────────────────────────────────
def _run(args: list[str], capture: bool = True) -> tuple[str, str, int]:
    """Run a command. Returns (stdout, stderr, returncode)."""
    result = subprocess.run(
        args,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
        text=True,
    )
    stdout = result.stdout.strip() if result.stdout else ""
    stderr = result.stderr.strip() if result.stderr else ""
    return stdout, stderr, result.returncode


def git(*args, capture: bool = True) -> tuple[str, str, int]:
    return _run(["git"] + list(args), capture=capture)


def git_out(*args) -> str:
    """Convenience: return stdout of a git command, ignoring errors."""
    out, _, _ = git(*args)
    return out


def git_ok(*args) -> bool:
    """Return True if exit code is 0."""
    _, _, code = git(*args)
    return code == 0

# ── Repo state helpers ─────────────────────────────────────────────────────────
def get_branch() -> str:
    return git_out("rev-parse", "--abbrev-ref", "HEAD")

def assert_in_repo():
    out, _, code = git("rev-parse", "--show-toplevel")
    if code != 0:
        c_error("Not inside a git repository.")
        sys.exit(1)

def assert_not_protected(force: bool = False):
    branch = get_branch()
    if branch in PROTECTED_BRANCHES and not force:
        c_error(f"You are on the protected branch '{branch}'.")
        c_warn( f"Direct pushes to '{branch}' are blocked to prevent accidents.")
        c_info( f"Create a feature branch:  dsync branch your-feature")
        c_info( f"Or pass --force to override (NOT recommended on shared branches).")
        sys.exit(1)

def has_uncommitted() -> bool:
    out, _, _ = git("status", "--porcelain")
    return bool(out)

def stash_count() -> int:
    out, _, _ = git("stash", "list")
    return len([l for l in out.splitlines() if l.strip()])

def ahead_behind(branch: str) -> tuple[int, int]:
    a = git_out("rev-list", f"origin/{branch}..HEAD", "--count")
    b = git_out("rev-list", f"HEAD..origin/{branch}", "--count")
    try:
        return int(a), int(b)
    except ValueError:
        return 0, 0

# ── Command: init ──────────────────────────────────────────────────────────────
def cmd_init():
    assert_in_repo()
    c_header("De-Sentry: Repo Initialisation")
    divider()

    settings = {
        "pull.rebase":          "true",
        "push.autoSetupRemote": "true",
        "rerere.enabled":       "true",
        "init.defaultBranch":   "main",
        "core.autocrlf":        "input",   # 'input' works on both Win & Mac
    }
    for key, val in settings.items():
        c_step(f"Setting {key} = {val}")
        git("config", key, val)
        c_ok(f"{key} = {val}")

    # On Windows also set autocrlf to true
    if platform.system() == "Windows":
        git("config", "core.autocrlf", "true")
        c_ok("core.autocrlf = true  (Windows override)")

    c_step("Making dsync.sh executable (Mac/Linux)...")
    sh_path = os.path.join(os.path.dirname(__file__), "dsync.sh")
    if os.path.exists(sh_path):
        try:
            os.chmod(sh_path, 0o755)
            c_ok("dsync.sh is executable")
        except Exception:
            c_info("Could not chmod dsync.sh (Windows — OK, bash will handle it)")
    else:
        c_info("dsync.sh not found — skipping")

    c_step("Checking your git identity...")
    name  = git_out("config", "user.name")
    email = git_out("config", "user.email")
    if name and email:
        c_ok(f"Identity: {name} <{email}>")
    else:
        c_warn("No git identity found. Run:")
        c_info("  git config --global user.name  'Your Name'")
        c_info("  git config --global user.email 'you@example.com'")

    c_step("Fetching latest remote state...")
    git("fetch", "--all", "--prune")
    c_ok("Remote refs updated")

    divider()
    c_bold("Setup complete. You are ready to use dsync.")
    print()
    c_info("Quick reference:")
    c_info("  Windows : .\\scripts\\dsync.ps1 <command>")
    c_info("  Mac/Linux: ./scripts/dsync.sh  <command>")
    c_info("  Direct  : python scripts/dsync.py <command>")
    print()
    c_info("Commands: init | pull | push [msg] | status | branch <name> | log | help")
    print()

# ── Command: pull ──────────────────────────────────────────────────────────────
def cmd_pull():
    assert_in_repo()
    branch = get_branch()
    c_header(f"De-Sentry: Safe Pull  [{branch}]")
    divider()

    # 1. Stash if dirty
    stashed = False
    if has_uncommitted():
        c_step("Uncommitted changes detected — stashing...")
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        stash_msg = f"dsync-autostash-{stamp}"
        git("stash", "push", "-u", "-m", stash_msg)
        stashed = True
        c_ok(f"Stashed as: {stash_msg}")
    else:
        c_info("Working tree is clean — no stash needed")

    # 2. Fetch
    c_step("Fetching from origin...")
    out, err, _ = git("fetch", "--all", "--prune")
    for line in (out + "\n" + err).splitlines():
        if line.strip():
            c_info(line)
    c_ok("Fetch complete")

    # 3. Rebase
    c_step(f"Rebasing onto origin/{branch}...")
    out, err, code = git("rebase", f"origin/{branch}")

    if code != 0:
        c_error("Rebase conflict detected!")
        divider()
        conflicts, _, _ = git("diff", "--name-only", "--diff-filter=U")
        if conflicts:
            c_warn("Conflicting files:")
            for f in conflicts.splitlines():
                print(f"    {YELLOW}{f}{RESET}")
        print()
        c_bold("Resolution steps:")
        c_info("  1. Open each file above and resolve the conflict markers")
        c_info("     Look for:  <<<<<<< HEAD  =======  >>>>>>> origin/...")
        c_info("  2. git add <resolved-file>")
        c_info("  3. git rebase --continue")
        c_info("  --- OR to abort and go back ---")
        c_info("     git rebase --abort")
        if stashed:
            c_warn("Your stash is still saved. After resolving run: git stash pop")
        divider()
        sys.exit(1)

    c_ok("Rebase successful")

    # 4. Restore stash
    if stashed:
        c_step("Restoring your stashed changes...")
        _, _, pop_code = git("stash", "pop")
        if pop_code != 0:
            c_warn("Stash pop had conflicts — your changes are in stash[0]")
            c_info("Run: git stash show -p   to inspect")
            c_info("Run: git stash pop       to retry")
        else:
            c_ok("Stashed changes restored")

    # 5. Summary
    divider()
    ahead, _ = ahead_behind(branch)
    c_ok(f"Pull complete. Branch '{branch}' is up to date.")
    if ahead > 0:
        c_warn(f"You are {ahead} commit(s) ahead of origin — remember to push.")
    print()

# ── Command: push ──────────────────────────────────────────────────────────────
def cmd_push(message: str = "", force: bool = False):
    assert_in_repo()
    assert_not_protected(force)

    branch = get_branch()
    c_header(f"De-Sentry: Safe Push  [{branch}]")
    divider()

    # 1. Always pull first
    c_step("Syncing with remote before push...")
    cmd_pull()
    c_ok("Sync complete")

    # 2. Check if there's anything to do
    ahead, _ = ahead_behind(branch)

    if not has_uncommitted():
        if ahead == 0:
            c_info("Nothing to commit and nothing to push — already up to date.")
            print(); return
        else:
            c_info(f"Nothing new to commit, but {ahead} local commit(s) will be pushed.")
    else:
        # 3. Show diff summary
        c_step("Changes to be committed:")
        status_out, _, _ = git("status", "--short")
        for line in status_out.splitlines():
            if len(line) >= 3:
                flag = line[:2]
                fname = line[3:]
                col = file_colour(flag)
                print(f"    {col}{flag} {fname}{RESET}")
        print()

        # 4. Get commit message
        if not message:
            try:
                message = input(f"  {CYAN}Enter commit message (Ctrl+C to cancel): {RESET}").strip()
            except (KeyboardInterrupt, EOFError):
                print()
                c_info("Cancelled.")
                sys.exit(0)

        if not message:
            c_error("Commit message cannot be empty.")
            sys.exit(1)

        # 5. Stage and commit
        c_step("Staging all changes...")
        git("add", "-A")
        c_ok("All changes staged")

        c_step(f"Committing: {message}")
        _, _, code = git("commit", "-m", message)
        if code != 0:
            c_error("Commit failed.")
            sys.exit(1)
        c_ok("Committed")

    # 6. Push
    c_step(f"Pushing to origin/{branch}...")
    out, err, code = git("push")

    if code != 0:
        # git uses stderr for progress even on success — check if it's really an error
        combined = (out + err).lower()
        if "error" in combined or "fatal" in combined or "rejected" in combined:
            c_warn("Setting upstream and retrying...")
            _, _, retry_code = git("push", "--set-upstream", "origin", branch)
            if retry_code != 0:
                c_error("Push failed. Check your connection and permissions.")
                sys.exit(1)

    c_ok(f"Pushed to origin/{branch}")

    # 7. Summary
    divider()
    latest = git_out("log", "--oneline", "-1")
    c_ok(f"Push complete! Latest: {latest}")
    c_info(f"View: {GITHUB_REPO}/tree/{branch}")
    print()

# ── Command: status ────────────────────────────────────────────────────────────
def cmd_status():
    assert_in_repo()
    branch = get_branch()

    c_header("De-Sentry: Workspace Status")
    divider()

    ahead, behind = ahead_behind(branch)

    print(f"  {GRAY}Branch   {CYAN}{branch}{RESET}")
    print(f"  {GRAY}Remote   origin/{branch}{RESET}")
    print(f"  {GRAY}System   {platform.system()} / Python {platform.python_version()}{RESET}")

    if ahead > 0 or behind > 0:
        print(f"  {GRAY}Sync     ", end="")
        if ahead  > 0: print(f"{YELLOW}^{ahead} ahead  {RESET}", end="")
        if behind > 0: print(f"{RED}v{behind} behind{RESET}",  end="")
        print()
    else:
        print(f"  {GRAY}Sync     {GREEN}Up to date{RESET}")

    sc = stash_count()
    if sc > 0:
        print(f"  {GRAY}Stashes  {YELLOW}{sc} saved{RESET}")

    print()

    # Changed files
    status_out, _, _ = git("status", "--short")
    if status_out:
        print(f"  {WHITE}Changed files:{RESET}")
        for line in status_out.splitlines():
            if len(line) >= 3:
                flag = line[:2]
                fname = line[3:]
                col = file_colour(flag)
                print(f"    {col}{flag} {fname}{RESET}")
    else:
        print(f"  {GRAY}Working tree: {GREEN}clean{RESET}")

    # Recent commits
    print()
    print(f"  {WHITE}Recent commits:{RESET}")
    log_out, _, _ = git("log", "--oneline", "--graph", "--decorate", "-8")
    for line in log_out.splitlines():
        print(f"    {GRAY}{line}{RESET}")

    # Remote branches
    print()
    print(f"  {WHITE}Remote branches:{RESET}")
    branches_out, _, _ = git("branch", "-r")
    for line in branches_out.splitlines():
        print(f"    {GRAY}{line}{RESET}")

    divider()
    print()

# ── Command: branch ────────────────────────────────────────────────────────────
def cmd_branch(name: str = ""):
    assert_in_repo()

    if not name:
        c_error("Please provide a branch name.")
        c_info("Usage: dsync branch your-feature-name")
        c_info("Examples:")
        c_info("  dsync branch node-a/change-ledger")
        c_info("  dsync branch feat/routing-algo")
        sys.exit(1)

    # Sanitise
    import re
    safe = re.sub(r"[^a-z0-9/_-]", "", name.lower().replace(" ", "-"))
    if safe != name:
        c_warn(f"Branch name sanitised: '{name}' -> '{safe}'")
    name = safe

    c_header("De-Sentry: Create Branch")
    divider()

    c_step("Fetching latest remote state...")
    git("fetch", "--all", "--prune")
    c_ok("Fetched")

    c_step(f"Creating branch '{name}' from origin/main...")
    _, err, code = git("checkout", "-b", name, "origin/main")
    if code != 0:
        c_error(f"Branch creation failed. Does '{name}' already exist?")
        if err: c_info(err)
        sys.exit(1)
    c_ok(f"Switched to new branch '{name}'")

    c_step("Pushing branch to origin to track it...")
    git("push", "--set-upstream", "origin", name)
    c_ok(f"Branch '{name}' tracked on origin")

    divider()
    c_bold(f"You are now on branch: {name}")
    c_info("Make your changes, then: dsync push 'your message'")
    print()

# ── Command: log ───────────────────────────────────────────────────────────────
def cmd_log():
    assert_in_repo()
    c_header("De-Sentry: Commit History")
    divider()
    # Use subprocess directly so colour codes pass through
    subprocess.run([
        "git", "log", "--graph",
        "--pretty=format:%C(yellow)%h%Creset %C(cyan)%ad%Creset %C(white)%s%Creset %C(dim)- %an%Creset",
        "--date=short", "-20"
    ])
    print()
    divider()
    print()

# ── Command: help ──────────────────────────────────────────────────────────────
def cmd_help():
    print(f"""
  {CYAN}De-Sentry  dsync  -  Safe Collaboration Script{RESET}
  {GRAY}{'─' * 54}{RESET}

  {WHITE}PLATFORM{RESET}
    {GRAY}Windows  : .\\scripts\\dsync.ps1 <command>{RESET}
    {GRAY}Mac/Linux: ./scripts/dsync.sh  <command>{RESET}
    {GRAY}Direct   : python scripts/dsync.py <command>{RESET}

  {WHITE}COMMANDS{RESET}
    {GRAY}init                 One-time repo setup (everyone runs this first!){RESET}
    {GRAY}pull                 Safe pull: stash -> rebase -> restore{RESET}
    {GRAY}push [message]       Pull first, then commit all changes + push{RESET}
    {GRAY}status               Rich overview of branch, sync state, changes{RESET}
    {GRAY}branch NAME          Create a properly-named feature branch{RESET}
    {GRAY}log                  Pretty commit graph (last 20 commits){RESET}
    {GRAY}help                 Show this message{RESET}

  {WHITE}FLAGS{RESET}
    {GRAY}--force              Override branch protection (use carefully){RESET}

  {WHITE}EXAMPLES{RESET}
    {YELLOW}dsync init{RESET}
    {YELLOW}dsync branch node-b/columnar-storage{RESET}
    {YELLOW}dsync push "feat: add SHA-256 chain verification"{RESET}
    {YELLOW}dsync pull{RESET}
    {YELLOW}dsync status{RESET}

  {WHITE}BRANCH NAMING CONVENTION{RESET}
    {GRAY}node-a/NAME    Work owned by Node A team member{RESET}
    {GRAY}node-b/NAME    Work owned by Node B team member{RESET}
    {GRAY}node-c/NAME    Work owned by Node C team member{RESET}
    {GRAY}feat/NAME      Shared feature (coordinate with team first){RESET}
    {GRAY}fix/NAME       Bug fix{RESET}
    {GRAY}docs/NAME      Documentation only{RESET}

  {WHITE}PROTECTED BRANCHES  {GRAY}(direct push blocked){RESET}
    {RED}main   dev   release   master{RESET}
""")

# ── Entry point ────────────────────────────────────────────────────────────────
def main():
    args  = sys.argv[1:]
    force = "--force" in args or "-f" in args
    args  = [a for a in args if a not in ("--force", "-f")]

    cmd  = args[0].lower() if args else "help"
    arg1 = args[1] if len(args) > 1 else ""

    dispatch = {
        "init":   cmd_init,
        "pull":   cmd_pull,
        "push":   lambda: cmd_push(arg1, force),
        "status": cmd_status,
        "branch": lambda: cmd_branch(arg1),
        "log":    cmd_log,
        "help":   cmd_help,
        "--help": cmd_help,
        "-h":     cmd_help,
    }

    if cmd not in dispatch:
        c_error(f"Unknown command: '{cmd}'")
        cmd_help()
        sys.exit(1)

    dispatch[cmd]()

if __name__ == "__main__":
    main()
