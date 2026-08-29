# ==============================================================================
#  dsync.ps1  -  De-Sentry Safe Collaboration Script  (Windows / PowerShell)
#
#  Usage:
#    .\scripts\dsync.ps1 init                       # one-time repo setup
#    .\scripts\dsync.ps1 pull                       # safe pull (stash->rebase->restore)
#    .\scripts\dsync.ps1 push "your message"        # pull first, then commit & push
#    .\scripts\dsync.ps1 push                       # prompts for message interactively
#    .\scripts\dsync.ps1 status                     # rich status overview
#    .\scripts\dsync.ps1 branch <name>              # create a properly-named branch
#    .\scripts\dsync.ps1 log                        # pretty commit graph
#    .\scripts\dsync.ps1 help                       # show this help
#
#  Add an alias to your PowerShell profile for convenience:
#    Set-Alias dsync "$PWD\scripts\dsync.ps1"
# ==============================================================================

param(
    [Parameter(Position = 0)] [string] $Command = "help",
    [Parameter(Position = 1)] [string] $Arg1    = "",
    [Parameter(Position = 2)] [string] $Arg2    = "",
    [switch] $Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---- Colour helpers ----------------------------------------------------------
function C-Header { param($t) Write-Host "" ; Write-Host $t -ForegroundColor Cyan }
function C-Step   { param($t) Write-Host "  -> $t" -ForegroundColor White }
function C-OK     { param($t) Write-Host "  OK  $t" -ForegroundColor Green }
function C-Warn   { param($t) Write-Host "  !!  $t" -ForegroundColor Yellow }
function C-Error  { param($t) Write-Host "  XX  $t" -ForegroundColor Red }
function C-Info   { param($t) Write-Host "  .   $t" -ForegroundColor DarkGray }
function C-Bold   { param($t) Write-Host $t -ForegroundColor White }
function Divider  { Write-Host ("------------------------------------------------------------") -ForegroundColor DarkGray }

# ---- Git helpers -------------------------------------------------------------
function Get-CurrentBranch  { return (git rev-parse --abbrev-ref HEAD 2>$null).Trim() }
function Get-RepoRoot       { return (git rev-parse --show-toplevel 2>$null).Trim() }

function Assert-InRepo {
    $root = Get-RepoRoot
    if (-not $root) { C-Error "Not inside a git repository."; exit 1 }
}

function Assert-NotProtectedBranch {
    $branch = Get-CurrentBranch
    $protected = @("main", "master", "dev", "release")
    if ($protected -contains $branch -and -not $Force) {
        C-Error "You are on the protected branch '$branch'."
        C-Warn  "Direct pushes to '$branch' are blocked to prevent accidents."
        C-Info  "Create a feature branch first:  .\scripts\dsync.ps1 branch your-feature"
        C-Info  "Or use -Force to override (NOT recommended for shared branches)."
        exit 1
    }
}

function Get-HasUncommitted {
    $status = git status --porcelain 2>$null
    return ($null -ne $status -and $status.Length -gt 0)
}

function Get-StashCount {
    $out = git stash list 2>$null
    if (-not $out) { return 0 }
    return ($out | Measure-Object -Line).Lines
}

# ---- Command: init -----------------------------------------------------------
function Cmd-Init {
    Assert-InRepo
    C-Header "De-Sentry: Repo Initialisation"
    Divider

    C-Step "Configuring pull strategy: rebase (no merge commits)"
    git config pull.rebase true
    C-OK   "pull.rebase = true"

    C-Step "Configuring push: track current branch automatically"
    git config push.autoSetupRemote true
    C-OK   "push.autoSetupRemote = true"

    C-Step "Enabling reuse-recorded-resolution (rerere)"
    git config rerere.enabled true
    C-OK   "rerere.enabled = true"

    C-Step "Setting default branch name to main"
    git config init.defaultBranch main
    C-OK   "init.defaultBranch = main"

    C-Step "Setting safe line endings (auto-CRLF)"
    git config core.autocrlf true
    C-OK   "core.autocrlf = true"

    C-Step "Checking your git identity..."
    $name  = (git config user.name  2>$null)
    $email = (git config user.email 2>$null)
    if ($name -and $email) {
        $name  = $name.Trim()
        $email = $email.Trim()
        C-OK  "Identity: $name <$email>"
    } else {
        C-Warn "No git identity set. Run:"
        C-Info "  git config --global user.name  'Your Name'"
        C-Info "  git config --global user.email 'you@email.com'"
    }

    C-Step "Fetching latest remote state..."
    git fetch --all --prune | Out-Null
    C-OK   "Remote refs updated"

    Divider
    C-Bold "Setup complete. You are ready to use dsync."
    C-Info ""
    C-Info "Quick reference:"
    C-Info "  .\scripts\dsync.ps1 pull             safe pull"
    C-Info "  .\scripts\dsync.ps1 push 'message'   commit + push"
    C-Info "  .\scripts\dsync.ps1 branch my-feat   new branch"
    C-Info "  .\scripts\dsync.ps1 status           overview"
    Write-Host ""
}

# ---- Command: pull -----------------------------------------------------------
function Cmd-Pull {
    Assert-InRepo
    $branch = Get-CurrentBranch
    C-Header "De-Sentry: Safe Pull  [$branch]"
    Divider

    # 1. Stash if dirty
    $stashed = $false
    if (Get-HasUncommitted) {
        C-Step "Uncommitted changes detected -- stashing..."
        $stashMsg = "dsync-autostash-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
        git stash push -u -m $stashMsg | Out-Null
        $stashed = $true
        C-OK   "Stashed as: $stashMsg"
    } else {
        C-Info "Working tree is clean -- no stash needed"
    }

    # 2. Fetch
    C-Step "Fetching from origin..."
    git fetch --all --prune 2>&1 | ForEach-Object { C-Info "$_" }
    C-OK   "Fetch complete"

    # 3. Rebase
    C-Step "Rebasing onto origin/$branch..."
    $rebaseOut = git rebase "origin/$branch" 2>&1
    $rebaseFailed = ($LASTEXITCODE -ne 0)

    if ($rebaseFailed) {
        C-Error "Rebase conflict detected!"
        Divider
        $conflicted = git diff --name-only --diff-filter=U 2>$null
        if ($conflicted) {
            C-Warn "Conflicting files:"
            $conflicted | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
        }
        Write-Host ""
        C-Bold "Resolution steps:"
        C-Info "  1. Open each file above and resolve the conflict markers"
        C-Info "     Look for: <<<<<<< HEAD  =======  >>>>>>> origin/..."
        C-Info "  2. Run:  git add <resolved-file>"
        C-Info "  3. Run:  git rebase --continue"
        C-Info "  -- OR to abort and go back to before the pull:"
        C-Info "     Run:  git rebase --abort"
        if ($stashed) {
            C-Warn "Your stash is still saved. After resolving run: git stash pop"
        }
        Divider
        exit 1
    }

    C-OK "Rebase successful"

    # 4. Restore stash
    if ($stashed) {
        C-Step "Restoring your stashed changes..."
        $popOut = git stash pop 2>&1
        if ($LASTEXITCODE -ne 0) {
            C-Warn "Stash pop had conflicts -- your changes are in stash[0]"
            C-Info "Run: git stash show -p   to inspect"
            C-Info "Run: git stash pop       to retry"
        } else {
            C-OK   "Stashed changes restored"
        }
    }

    # 5. Summary
    Divider
    $ahead  = (git rev-list "origin/$branch..HEAD" --count 2>$null).Trim()
    C-OK   "Pull complete. Branch '$branch' is up to date."
    if ([int]$ahead -gt 0) { C-Warn "You are $ahead commit(s) ahead of origin -- remember to push." }
    Write-Host ""
}

# ---- Command: push -----------------------------------------------------------
function Cmd-Push {
    param([string] $Message)
    Assert-InRepo
    Assert-NotProtectedBranch

    $branch = Get-CurrentBranch
    C-Header "De-Sentry: Safe Push  [$branch]"
    Divider

    # 1. Always pull first
    C-Step "Syncing with remote before push..."
    Cmd-Pull
    C-OK   "Sync complete"

    # 2. Check if anything to commit
    $ahead = ([int](git rev-list "origin/$branch..HEAD" --count 2>$null).Trim())

    if (-not (Get-HasUncommitted)) {
        if ($ahead -eq 0) {
            C-Info "Nothing to commit and nothing to push -- already up to date."
            Write-Host ""; return
        } else {
            C-Info "Nothing new to commit, but $ahead local commit(s) will be pushed."
        }
    } else {
        # 3. Show diff summary
        C-Step "Changes to be committed:"
        git status --short | ForEach-Object {
            $flag   = $_.Substring(0, 2)
            $file   = $_.Substring(3)
            $colour = "White"
            if ($flag -match "^M") { $colour = "Yellow" }
            if ($flag -match "^A") { $colour = "Green"  }
            if ($flag -match "^D") { $colour = "Red"    }
            if ($flag -match "^\?") { $colour = "DarkGray" }
            Write-Host "    $flag $file" -ForegroundColor $colour
        }
        Write-Host ""

        # 4. Get commit message
        if (-not $Message) {
            Write-Host "  Enter commit message (Ctrl+C to cancel): " -ForegroundColor Cyan -NoNewline
            $Message = Read-Host
        }
        if (-not $Message.Trim()) {
            C-Error "Commit message cannot be empty."
            exit 1
        }

        # 5. Stage and commit
        C-Step "Staging all changes..."
        git add -A
        C-OK   "All changes staged"

        C-Step "Committing: $Message"
        git commit -m $Message | Out-Null
        C-OK   "Committed"
    }

    # 6. Push
    C-Step "Pushing to origin/$branch..."
    git push 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        C-Warn "Setting upstream and retrying..."
        git push --set-upstream origin $branch 2>&1 | ForEach-Object { C-Info "$_" }
        if ($LASTEXITCODE -ne 0) {
            C-Error "Push failed. Check your connection and permissions."
            exit 1
        }
    }
    C-OK "Pushed to origin/$branch"

    # 7. Summary
    Divider
    $latestHash = (git log --oneline -1 2>$null).Trim()
    C-OK "Push complete! Latest: $latestHash"
    C-Info "View: https://github.com/R1sh1m/De-sentry/tree/$branch"
    Write-Host ""
}

# ---- Command: status ---------------------------------------------------------
function Cmd-Status {
    Assert-InRepo
    $branch = Get-CurrentBranch

    C-Header "De-Sentry: Workspace Status"
    Divider

    $ahead  = ([int](git rev-list "origin/$branch..HEAD" --count 2>$null).Trim())
    $behind = ([int](git rev-list "HEAD..origin/$branch" --count 2>$null).Trim())

    Write-Host "  Branch   " -NoNewline -ForegroundColor DarkGray
    Write-Host $branch       -ForegroundColor Cyan
    Write-Host "  Remote   origin/$branch" -ForegroundColor DarkGray

    if ($ahead -gt 0 -or $behind -gt 0) {
        Write-Host "  Sync     " -NoNewline -ForegroundColor DarkGray
        if ($ahead  -gt 0) { Write-Host "^$ahead ahead  " -NoNewline -ForegroundColor Yellow }
        if ($behind -gt 0) { Write-Host "v$behind behind" -NoNewline -ForegroundColor Red    }
        Write-Host ""
    } else {
        Write-Host "  Sync     Up to date" -ForegroundColor Green
    }

    $stashCount = Get-StashCount
    if ($stashCount -gt 0) {
        Write-Host "  Stashes  $stashCount saved" -ForegroundColor Yellow
    }
    Write-Host ""

    $statusLines = git status --short 2>$null
    if ($statusLines) {
        Write-Host "  Changed files:" -ForegroundColor White
        $statusLines | ForEach-Object {
            $flag   = $_.Substring(0, 2)
            $file   = $_.Substring(3)
            $colour = "White"
            if ($flag -match "^M") { $colour = "Yellow" }
            if ($flag -match "^A") { $colour = "Green"  }
            if ($flag -match "^D") { $colour = "Red"    }
            if ($flag -match "^\?") { $colour = "DarkGray" }
            Write-Host "    $flag $file" -ForegroundColor $colour
        }
    } else {
        Write-Host "  Working tree: clean" -ForegroundColor Green
    }

    Write-Host ""
    Write-Host "  Recent commits:" -ForegroundColor White
    git log --oneline --graph --decorate -8 2>$null | ForEach-Object {
        Write-Host "    $_" -ForegroundColor DarkGray
    }

    Write-Host ""
    Write-Host "  Remote branches:" -ForegroundColor White
    git branch -r 2>$null | ForEach-Object {
        Write-Host "    $_" -ForegroundColor DarkGray
    }
    Divider; Write-Host ""
}

# ---- Command: branch ---------------------------------------------------------
function Cmd-Branch {
    param([string] $Name)
    Assert-InRepo

    if (-not $Name) {
        C-Error "Please provide a branch name."
        C-Info  "Usage: .\scripts\dsync.ps1 branch your-feature-name"
        C-Info  "Examples:"
        C-Info  "  .\scripts\dsync.ps1 branch node-a/change-ledger"
        C-Info  "  .\scripts\dsync.ps1 branch feat/routing-algo"
        exit 1
    }

    # Sanitise: lowercase, spaces to dashes, strip bad chars
    $safeName = $Name.ToLower() -replace '\s+', '-' -replace '[^a-z0-9/_-]', ''
    if ($safeName -ne $Name) { C-Warn "Branch name sanitised: '$Name' -> '$safeName'" }

    C-Header "De-Sentry: Create Branch"
    Divider

    C-Step "Fetching latest remote state..."
    git fetch --all --prune | Out-Null

    C-Step "Creating branch '$safeName' from origin/main..."
    git checkout -b $safeName "origin/main" 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        C-Error "Branch creation failed. Does '$safeName' already exist?"
        exit 1
    }
    C-OK   "Switched to new branch '$safeName'"

    C-Step "Pushing branch to origin to track it..."
    git push --set-upstream origin $safeName 2>&1 | Out-Null
    C-OK   "Branch '$safeName' is tracked on origin"

    Divider
    C-Bold "You are now on branch: $safeName"
    C-Info "Make your changes, then: .\scripts\dsync.ps1 push 'your message'"
    Write-Host ""
}

# ---- Command: log ------------------------------------------------------------
function Cmd-Log {
    Assert-InRepo
    C-Header "De-Sentry: Commit History"
    Divider
    git log --graph --pretty=format:"%C(yellow)%h%Creset %C(cyan)%ad%Creset %C(white)%s%Creset %C(dim)- %an%Creset" --date=short -20 2>$null
    Write-Host ""; Divider; Write-Host ""
}

# ---- Command: help -----------------------------------------------------------
function Cmd-Help {
    Write-Host ""
    Write-Host "  De-Sentry  dsync  -  Safe Collaboration Script" -ForegroundColor Cyan
    Write-Host "  ----------------------------------------------------------" -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "  COMMANDS" -ForegroundColor White
    Write-Host "    init                  One-time repo setup (run this first!)"  -ForegroundColor DarkGray
    Write-Host "    pull                  Safe pull: stash -> rebase -> restore"   -ForegroundColor DarkGray
    Write-Host "    push [message]        Pull first, then commit all + push"     -ForegroundColor DarkGray
    Write-Host "    status                Rich overview of branch and changes"     -ForegroundColor DarkGray
    Write-Host "    branch NAME           Create a properly-named feature branch"  -ForegroundColor DarkGray
    Write-Host "    log                   Pretty commit graph (last 20)"          -ForegroundColor DarkGray
    Write-Host "    help                  Show this message"                       -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "  FLAGS" -ForegroundColor White
    Write-Host "    -Force                Override branch protection (use carefully)" -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "  EXAMPLES" -ForegroundColor White
    Write-Host "    .\scripts\dsync.ps1 init" -ForegroundColor Yellow
    Write-Host "    .\scripts\dsync.ps1 branch node-a/ledger-hash" -ForegroundColor Yellow
    Write-Host "    .\scripts\dsync.ps1 push 'feat: add SHA-256 chain verification'" -ForegroundColor Yellow
    Write-Host "    .\scripts\dsync.ps1 pull" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  BRANCH NAMING" -ForegroundColor White
    Write-Host "    node-a/NAME    Work owned by Node A member" -ForegroundColor DarkGray
    Write-Host "    node-b/NAME    Work owned by Node B member" -ForegroundColor DarkGray
    Write-Host "    node-c/NAME    Work owned by Node C member" -ForegroundColor DarkGray
    Write-Host "    feat/NAME      Shared feature (coordinate first)" -ForegroundColor DarkGray
    Write-Host "    fix/NAME       Bug fix" -ForegroundColor DarkGray
    Write-Host "    docs/NAME      Documentation only" -ForegroundColor DarkGray
    Write-Host ""
    Write-Host "  PROTECTED BRANCHES  (direct push blocked)" -ForegroundColor White
    Write-Host "    main   dev   release" -ForegroundColor Red
    Write-Host ""
}

# ---- Router ------------------------------------------------------------------
switch ($Command.ToLower()) {
    "init"   { Cmd-Init }
    "pull"   { Cmd-Pull }
    "push"   { Cmd-Push -Message $Arg1 }
    "status" { Cmd-Status }
    "branch" { Cmd-Branch -Name $Arg1 }
    "log"    { Cmd-Log }
    "help"   { Cmd-Help }
    default  { C-Error "Unknown command: '$Command'"; Cmd-Help; exit 1 }
}
