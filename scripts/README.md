# dsync — De-Sentry Collaboration Scripts

Custom push/pull scripts for safe team collaboration. Prevents merge conflicts,
protects shared branches, and handles dirty working trees automatically.

---

## First-Time Setup (everyone does this once)

**Windows (PowerShell):**
```powershell
cd "Database Project"
.\scripts\dsync.ps1 init
```

**Linux / macOS / Git Bash:**
```bash
cd "Database Project"
chmod +x ./scripts/dsync.sh
./scripts/dsync.sh init
```

---

## Daily Workflow

### Pull latest changes from teammates
```powershell
# Windows
.\scripts\dsync.ps1 pull

# Linux/Mac
./scripts/dsync.sh pull
```

What it does:
1. Stashes any uncommitted work so it's not lost
2. Fetches latest from `origin`
3. Rebases your branch onto `origin/<branch>` (clean linear history, no merge commits)
4. Restores your stashed work
5. If there are rebase conflicts → prints exact commands to resolve and exits cleanly

---

### Commit and push your work
```powershell
# Windows — message as argument
.\scripts\dsync.ps1 push "feat: add SHA-256 chain verification"

# Windows — interactive message prompt
.\scripts\dsync.ps1 push

# Linux/Mac
./scripts/dsync.sh push "feat: add SHA-256 chain verification"
```

What it does:
1. Runs a safe pull first (always sync before pushing)
2. Shows a colour-coded diff summary of your changes
3. Stages all changes (`git add -A`)
4. Commits with your message
5. Pushes to your branch on origin

> **Note:** Direct pushes to `main`, `dev`, or `release` are blocked. Always work on a feature branch.

---

### Create a new branch
```powershell
# Windows
.\scripts\dsync.ps1 branch node-a/ledger-hash-verification

# Linux/Mac
./scripts/dsync.sh branch node-a/ledger-hash-verification
```

Branch is created from the latest `origin/main` and immediately tracked on origin.

---

### See what's going on
```powershell
.\scripts\dsync.ps1 status   # Rich overview: sync state, changes, recent log
.\scripts\dsync.ps1 log      # Pretty commit graph (last 20)
```

---

## Branch Naming Convention

| Prefix | Who/What |
|---|---|
| `node-a/` | Rishi's work (Node A — ROOT, image specialist) |
| `node-b/` | Teammate B's work (Node B — CSV/XML specialist) |
| `node-c/` | Teammate C's work (Node C — generalist) |
| `feat/` | Shared feature (coordinate with team before creating) |
| `fix/` | Bug fix |
| `docs/` | Documentation only |

**Examples:**
```
node-a/change-ledger-sha256
node-b/columnar-storage-engine
node-c/transit-handoff-protocol
feat/brain-file-generation
docs/update-architecture
fix/rebase-conflict-in-sync
```

---

## Protected Branches

These branches are protected — `dsync push` will refuse to push directly to them:

- `main` — stable, always working
- `dev` — integration branch
- `release` — tagged releases

**Workflow for merging into main:**
1. Create a PR on GitHub from your feature branch → `main`
2. Get at least one teammate to review
3. Merge via GitHub UI (squash merge preferred for clean history)

---

## Conflict Resolution Guide

If `dsync pull` fails with a rebase conflict:

```
  ✗ Rebase conflict detected!
  ⚠ Conflicting files:
      src/core/ledger/ledger.cpp
```

**Steps:**
1. Open the conflicting file(s) in your editor
2. Find and resolve the conflict markers:
   ```
   <<<<<<< HEAD
   your version
   =======
   teammate's version
   >>>>>>> origin/main
   ```
3. Delete the markers and keep the correct code
4. Run:
   ```bash
   git add src/core/ledger/ledger.cpp
   git rebase --continue
   ```
5. If it's too messy, abort and start fresh:
   ```bash
   git rebase --abort      # back to before the pull
   git stash pop           # restore your work
   ```

---

## Config: `node_profile.toml`

Your local node config (`node_profile.toml`) is **gitignored** — it has your local IP/port and is different for each teammate. Instead:

1. Copy the template: `cp node_profile.example.toml node_profile.toml`
2. Edit it with your node ID and your peers' addresses
3. Never commit `node_profile.toml`

The example template [`node_profile.example.toml`](../node_profile.example.toml) is committed and shows all available options.
