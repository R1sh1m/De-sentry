---
version: 2.0
name: De-Sentry Control Room
description: An Apple-design-language desktop control room for a peer-to-peer database mesh. Frosted chrome recedes; the mesh is the artifact. Monochrome surfaces, hairline separation, one Action Blue accent, and a single semantic status hue set (converged / lagging / offline) that is the only color permitted to carry meaning.
source: >
  Tokens below are inherited verbatim from the Apple design analysis installed by
  `npx getdesign@latest add apple` (https://getdesign.md/apple/design-md and its kit).
  The full unmodified analysis is kept at docs/apple-reference.md. This file is the
  *application* of that language to a dense, always-on desktop tool -- which is a
  different problem from a marketing page, and the deviations are stated explicitly
  in "Where this departs from the source" below.

colors:
  primary: "#0066cc"
  primary-focus: "#0071e3"
  primary-on-dark: "#2997ff"

  ink: "#1d1d1f"
  body: "#1d1d1f"
  body-on-dark: "#f5f5f7"
  body-muted: "#cccccc"
  ink-muted-80: "#333333"
  ink-muted-48: "#7a7a7a"

  divider-soft: "#f0f0f0"
  hairline: "#e0e0e0"
  hairline-on-dark: "#2f2f31"

  canvas: "#ffffff"
  canvas-parchment: "#f5f5f7"
  surface-pearl: "#fafafc"
  surface-raised: "#ffffff"
  surface-tile-1: "#272729"
  surface-tile-2: "#2a2a2c"
  surface-tile-3: "#252527"
  surface-black: "#000000"
  surface-chip-translucent: "#d2d2d7"

  on-primary: "#ffffff"
  on-dark: "#ffffff"

  status-converged: "#1d7f4e"
  status-converged-on-dark: "#30d158"
  status-lagging: "#8a6100"
  status-lagging-on-dark: "#ffd60a"
  status-offline: "#a1252b"
  status-offline-on-dark: "#ff453a"
  status-supervisor: "#5a4bb8"
  status-supervisor-on-dark: "#bf5af2"

typography:
  hero-display:
    fontFamily: "SF Pro Display, system-ui, -apple-system, sans-serif"
    fontSize: 56px
    fontWeight: 600
    lineHeight: 1.07
    letterSpacing: -0.28px
  display-lg:
    fontFamily: "SF Pro Display, system-ui, -apple-system, sans-serif"
    fontSize: 40px
    fontWeight: 600
    lineHeight: 1.1
    letterSpacing: 0
  display-md:
    fontFamily: "SF Pro Text, system-ui, -apple-system, sans-serif"
    fontSize: 34px
    fontWeight: 600
    lineHeight: 1.47
    letterSpacing: -0.374px
  lead:
    fontFamily: "SF Pro Display, system-ui, -apple-system, sans-serif"
    fontSize: 28px
    fontWeight: 400
    lineHeight: 1.14
    letterSpacing: 0.196px
  tagline:
    fontFamily: "SF Pro Display, system-ui, -apple-system, sans-serif"
    fontSize: 21px
    fontWeight: 600
    lineHeight: 1.19
    letterSpacing: 0.231px
  body-strong:
    fontFamily: "SF Pro Text, system-ui, -apple-system, sans-serif"
    fontSize: 15px
    fontWeight: 600
    lineHeight: 1.33
    letterSpacing: -0.24px
  body:
    fontFamily: "SF Pro Text, system-ui, -apple-system, sans-serif"
    fontSize: 15px
    fontWeight: 400
    lineHeight: 1.47
    letterSpacing: -0.24px
  caption:
    fontFamily: "SF Pro Text, system-ui, -apple-system, sans-serif"
    fontSize: 13px
    fontWeight: 400
    lineHeight: 1.38
    letterSpacing: -0.08px
  caption-strong:
    fontFamily: "SF Pro Text, system-ui, -apple-system, sans-serif"
    fontSize: 13px
    fontWeight: 600
    lineHeight: 1.31
    letterSpacing: -0.08px
  button-utility:
    fontFamily: "SF Pro Text, system-ui, -apple-system, sans-serif"
    fontSize: 13px
    fontWeight: 500
    lineHeight: 1.0
    letterSpacing: -0.08px
  fine-print:
    fontFamily: "SF Pro Text, system-ui, -apple-system, sans-serif"
    fontSize: 11px
    fontWeight: 400
    lineHeight: 1.27
    letterSpacing: 0
  section-label:
    fontFamily: "SF Pro Text, system-ui, -apple-system, sans-serif"
    fontSize: 11px
    fontWeight: 600
    lineHeight: 1.0
    letterSpacing: 0.06em
    textTransform: uppercase
  mono:
    fontFamily: "SF Mono, ui-monospace, SFMono-Regular, Menlo, Consolas, monospace"
    fontSize: 12px
    fontWeight: 400
    lineHeight: 1.5
    letterSpacing: 0
  mono-strong:
    fontFamily: "SF Mono, ui-monospace, SFMono-Regular, Menlo, Consolas, monospace"
    fontSize: 12px
    fontWeight: 600
    lineHeight: 1.5
    letterSpacing: 0

rounded:
  none: 0px
  xs: 5px
  sm: 8px
  md: 11px
  lg: 18px
  pill: 9999px
  full: 9999px

spacing:
  xxs: 4px
  xs: 8px
  sm: 12px
  md: 17px
  lg: 24px
  xl: 32px
  xxl: 48px
  section: 80px

elevation:
  flat: "none"
  hairline: "inset 0 0 0 1px rgba(0, 0, 0, 0.08)"
  card: "0 1px 2px rgba(0, 0, 0, 0.04), 0 8px 24px rgba(0, 0, 0, 0.06)"
  popover: "0 4px 12px rgba(0, 0, 0, 0.08), 0 16px 48px rgba(0, 0, 0, 0.12)"
  artifact: "rgba(0, 0, 0, 0.22) 3px 5px 30px 0"

motion:
  instant: "90ms cubic-bezier(0.4, 0, 0.2, 1)"
  quick: "160ms cubic-bezier(0.4, 0, 0.2, 1)"
  settle: "280ms cubic-bezier(0.32, 0.72, 0, 1)"
  reduced: "0ms"

components:
  chrome-header:
    backgroundColor: "rgba(255, 255, 255, 0.72)"
    backdropFilter: "saturate(180%) blur(20px)"
    borderBottom: "1px solid {colors.hairline}"
    height: 52px
    padding: "0 {spacing.md}"
  chrome-sidebar:
    backgroundColor: "rgba(245, 245, 247, 0.72)"
    backdropFilter: "saturate(180%) blur(20px)"
    borderRight: "1px solid {colors.hairline}"
    width: 280px
    padding: "{spacing.sm}"
  tree-row:
    typography: "{typography.body}"
    rounded: "{rounded.sm}"
    padding: "6px {spacing.xs}"
    height: 30px
  tree-row-selected:
    backgroundColor: "{colors.primary}"
    textColor: "{colors.on-primary}"
    rounded: "{rounded.sm}"
  card:
    backgroundColor: "{colors.surface-raised}"
    rounded: "{rounded.md}"
    padding: "{spacing.md}"
    boxShadow: "{elevation.hairline}"
  card-elevated:
    backgroundColor: "{colors.surface-raised}"
    rounded: "{rounded.md}"
    padding: "{spacing.lg}"
    boxShadow: "{elevation.card}"
  inspector:
    backgroundColor: "rgba(255, 255, 255, 0.72)"
    backdropFilter: "saturate(180%) blur(20px)"
    borderLeft: "1px solid {colors.hairline}"
    width: 340px
    padding: "{spacing.md}"
  button-primary:
    backgroundColor: "{colors.primary}"
    textColor: "{colors.on-primary}"
    typography: "{typography.button-utility}"
    rounded: "{rounded.pill}"
    padding: "7px 16px"
  button-secondary:
    backgroundColor: "{colors.surface-pearl}"
    textColor: "{colors.ink}"
    typography: "{typography.button-utility}"
    rounded: "{rounded.pill}"
    padding: "7px 16px"
    boxShadow: "{elevation.hairline}"
  button-destructive:
    backgroundColor: "transparent"
    textColor: "{colors.status-offline}"
    typography: "{typography.button-utility}"
    rounded: "{rounded.pill}"
    padding: "7px 16px"
    boxShadow: "{elevation.hairline}"
  segmented:
    backgroundColor: "{colors.canvas-parchment}"
    rounded: "{rounded.sm}"
    padding: "2px"
    height: 28px
  badge:
    typography: "{typography.fine-print}"
    rounded: "{rounded.pill}"
    padding: "2px 8px"
  badge-engine:
    backgroundColor: "{colors.canvas-parchment}"
    textColor: "{colors.ink-muted-80}"
    typography: "{typography.fine-print}"
    rounded: "{rounded.pill}"
    padding: "2px 8px"
  hash-chip:
    backgroundColor: "{colors.canvas-parchment}"
    textColor: "{colors.ink-muted-80}"
    typography: "{typography.mono}"
    rounded: "{rounded.xs}"
    padding: "1px 6px"
  table:
    typography: "{typography.caption}"
    rowHeight: 32px
    headerTypography: "{typography.section-label}"
    borderBottom: "1px solid {colors.divider-soft}"
  canvas-node:
    backgroundColor: "{colors.surface-raised}"
    rounded: "{rounded.md}"
    padding: "10px 14px"
    boxShadow: "{elevation.card}"
    minWidth: 168px
  canvas-edge:
    stroke: "{colors.hairline}"
    strokeWidth: 1.5
  wizard-step:
    backgroundColor: "{colors.surface-raised}"
    rounded: "{rounded.lg}"
    padding: "{spacing.xl}"
    boxShadow: "{elevation.popover}"
    maxWidth: 720px
  confidence-meter:
    trackColor: "{colors.canvas-parchment}"
    fillColor: "{colors.primary}"
    rounded: "{rounded.pill}"
    height: 6px
  toast:
    backgroundColor: "rgba(29, 29, 31, 0.92)"
    textColor: "{colors.on-dark}"
    backdropFilter: "blur(20px)"
    rounded: "{rounded.md}"
    padding: "10px {spacing.md}"
---

# De-Sentry Control Room — design system

The engine is a peer-to-peer database. The app is the **only** way a person
touches it: nobody hand-edits `node.json`, nobody runs `desentryd` from a
terminal. That makes this a control room, not a dashboard — the difference is
that a control room is where you *act*, under pressure, on state you did not
choose.

Everything below follows from that.

---

## 1. Principles

**1. The mesh is the artifact; the chrome is the wall.**
Apple's site makes the product the only thing with visual weight and lets the
UI disappear. Here the "product" is the user's own network of nodes. Header,
sidebar and inspector are frosted and quiet; the canvas carries the elevation.
When you look at the window, you should see *your nodes*, not our furniture.

**2. Color means exactly one thing: health.**
The Apple language has a single accent and no decorative color. We keep that
discipline and spend the freed budget on one job — a node's convergence state.
Action Blue is *interaction* and only interaction (selection, primary buttons,
focus). Green/amber/red are *state* and only state. Nothing in this app is
colored to be pretty. If you see color, it is telling you something.

**3. Truth over reassurance.**
A node that has not been heard from renders as offline, not as "syncing". An
unverified ledger tip renders as unverified, not blank. `under_replicated`
gets a visible badge. A number we do not have shows an em-dash, never a zero.
The engine is scrupulous about not overstating what it knows (see the ledger's
`unsigned_entries` counter, the placement layer's `skipped` list); the UI must
not undo that by rounding uncertainty into confidence.

**4. Dense, but never cramped.**
This is where we knowingly depart from the source: Apple's marketing pages run
at ~80px section rhythm and 17px body. A control room showing 50 nodes cannot.
We drop to 15px body and a 8/12/17/24 rhythm, and we buy the density back with
*hairlines and whitespace* rather than boxes — no nested cards, no borders
inside borders, no rules between every row.

**5. Destructive actions look destructive and read plainly.**
Reclaiming a node, releasing transit bytes and forcing a checkpoint are all
irreversible. They are never the visually dominant control, they always name
what will happen in words ("Reclaim — its data is re-replicated to 2 other
nodes first"), and they never sit adjacent to a benign action of the same
shape.

---

## 2. Where this departs from the source, and why

| Apple source | Control room | Reason |
|---|---|---|
| Body 17px | Body 15px | 17px is a reading pace. A node table is a scanning surface; 15px is the largest size that fits a 12-column row without truncation at 1280px. |
| Caption 14px | Caption 13px | Same reason, one step down, kept above 12px so it never becomes fine print. |
| Exactly one shadow (on product photography) | Three elevation levels | A marketing page has one plane. A control room has chrome, canvas and popovers, and a popover that does not lift is a popover you cannot tell is open. The shadows stay whisper-soft and the *chrome* still has none. |
| Section rhythm 80px | 24px between cards, 32px between regions | Density, as above. `{spacing.section}` survives in the wizard, which is the one screen that should feel unhurried. |
| No status color | Four semantic hues | Apple sells one product per page; we report on 50 machines. Convergence state is the single fact the user opens this app to learn, and encoding it in shape alone would be slower to read and worse for accessibility, not better. |
| Pill buttons at 11px/22px | 7px/16px | Scaled to 15px body so the pill's optical weight matches. |
| SF Pro Display for headlines | SF Pro Display only above 21px | Below that, SF Pro Text is what Apple itself uses, and it is what stays legible in a 30px table row. |

Everything else — the palette, the radius ladder, the pill grammar, the
frosted chrome, the negative display tracking, the refusal of gradients — is
inherited unchanged.

---

## 3. Foundations

### Type
`SF Pro Display` / `SF Pro Text` resolve natively on macOS. On Windows and
Linux the stack falls through to `system-ui` (Segoe UI Variable / Inter / Cantarell).
Per the source's substitution note, when Inter is what resolves we tighten
display tracking by a further `-0.01em`; the CSS does this with a
`@supports`-free static rule because the difference is invisible when SF Pro
*is* present.

Monospace (`SF Mono` → `ui-monospace` → Consolas) is reserved for values the
user may need to compare character by character: node ids, ledger hashes,
signatures, checksums, keys. It is never used for prose.

### Color and dark mode
Every color is a CSS custom property on `:root`, redefined under
`:root[data-theme="dark"]` and under `@media (prefers-color-scheme: dark)`
guarded by `:root:not([data-theme="light"])`. The window follows the OS by
default and the header exposes an explicit override, because a control room is
often on a second monitor whose ambient light does not match the laptop's.

Dark mode is not an inversion. Surfaces move to the `surface-tile-*` ladder,
hairlines move to `hairline-on-dark`, and every status hue switches to its
`-on-dark` sibling — the light-mode greens and reds are chosen for contrast on
white and go muddy on `#272729`.

### Contrast floor
Body and caption text meets WCAG AA (4.5:1) on its surface in both themes.
Status hues meet AA as *text*, which is why the light-mode set is darker than
the familiar iOS system colors — `#30d158` on white is 1.9:1 and would be
unreadable. Status is additionally encoded in the badge's label text, never in
the dot alone.

### Motion
`{motion.quick}` for anything the user initiated (selection, panel open),
`{motion.settle}` for anything the system initiated (a node appearing, an edge
re-routing), `{motion.instant}` for hover. Everything collapses to
`{motion.reduced}` under `prefers-reduced-motion`. The mesh canvas never
animates continuously — a constantly-drifting graph is unreadable and, on a
laptop, expensive.

---

## 4. Components

### Chrome
- **`chrome-header`** — 52px, frosted, hairline bottom. Left: window title +
  mesh name. Center: nothing (a control room's title bar is not a nav). Right:
  Tree/Mesh segmented control, theme override, "New node" primary pill.
- **`chrome-sidebar`** — 280px, frosted, hairline right. Scrolls independently.
- **`inspector`** — 340px, frosted, hairline left. Collapsible; state persists.

The three frosted planes are the only `backdrop-filter` surfaces in the app.
The canvas between them is opaque `{colors.canvas-parchment}` so the blur has
something to blur.

### `tree-row`
30px tall, 8px radius, indent 16px per level. Three slots: a 16px status dot,
a label (truncates from the middle for node ids, so both ends stay readable),
and an optional trailing badge. Selection is a filled Action Blue row with
white text — the one place a large blue fill appears.

Disclosure triangles are 10px chevrons at `{colors.ink-muted-48}`, rotating
90° over `{motion.quick}`.

### `card`
The workhorse. `{rounded.md}` (11px), white, `{elevation.hairline}`, 17px
padding. **Cards never nest.** A section inside a card is a
`{typography.section-label}` heading plus 12px of space — not another box.

### `hash-chip`
Monospace, 5px radius, parchment fill. Renders a 64-character hash as
`a1b2c3d4…9f8e7d6c` (8 + ellipsis + 8). Click copies the full value and shows
a toast. Every hash, signature, node id and checksum in the app uses this
component, so "click to copy" is learned once.

### `badge`
Pill, 11px. Four families, and a badge is never used for anything else:
- **status** — `converged` / `lagging Nn` / `offline` / `verifying`
- **engine** — `kv`, `columnar_lite`, `ts_rollup`, `vector_hnsw_lite`,
  `graph_adj`, `sqlite`, `duckdb`, `lmdb`, `sqlite_vec`
- **role** — `supervisor` (violet; the only violet in the app)
- **warning** — `under-replicated`, `over quota`, `unsigned entries`

### `canvas-node`
The mesh view's vertex. White card, `{elevation.card}`, 11px radius. Contains
a 6px status dot, the node's name in `{typography.body-strong}`, and its
ledger lag as `+0` / `−12` in `{typography.mono}`. Selected nodes get a 2px
Action Blue ring, not a fill — a filled node would fight the status dot.

### `canvas-edge`
1.5px hairline. Edges are drawn from `GET /_peers`, and their opacity encodes
the peer's fitness score (0.25 → 1.0), which is the one place a continuous
value is encoded visually rather than numerically. Hovering an edge shows
latency and success rate.

### `confidence-meter`
6px pill track, Action Blue fill, used only for the AI sizing proposal's
confidence. Below 0.35 the fill turns `{colors.status-lagging}` and the meter
is accompanied by the words "Low confidence — pick an engine yourself", because
a bar at 20% and a bar at 34% look the same at a glance and the difference
matters.

### `wizard-step`
The one place that keeps the source's generous rhythm: 18px radius, 32px
padding, `{spacing.section}`-scale breathing between blocks, max 720px wide and
centered. Creating a node is a decision, not a form.

---

## 5. Screens

### 5.1 Sidebar — the tree
Three levels, grouped by *where the bytes physically are*, because that is the
question the user is actually asking when a node misbehaves:

```
▾ This Mac                         ← device
  ▾ ~/Library/…/DeSentry           ← directory group (data_dir siblings)
      ● notes            kv
      ● sensors          ts_rollup
  ▾ /Volumes/FIELD-01     USB      ← mount group (removable)
      ◐ field-cache      kv   ⚠ offline
▾ Network                          ← remote, by mDNS name
  ▾ studio-imac.local
      ● archive          columnar_lite
      ◐ embeddings       vector_hnsw_lite
  ▾ pi-shed.local
      ● telemetry        ts_rollup   ⚠ under-replicated
▾ Supervisors                      ← always last, always separated
      ● supervisor-1     loopback
```

Grouping rules, in order: `data_dir` siblings on this device → detected mount
paths (removable ones labelled USB) → remote peers by advertised hostname →
supervisors, always in their own group at the bottom so they are never mistaken
for data nodes.

### 5.2 Canvas — Tree ⇄ Mesh
A segmented control in the header switches between two views of the same
selection; the selection survives the switch.

**Tree** is the physical hierarchy: device → directory/mount → node →
collection. Good for "what is on this disk".

**Mesh** is the logical graph: nodes as `canvas-node`, edges from
`GET /_peers`. Node color comes from comparing `/_ledger/tip` against the
highest tip in `/_brain`'s peer list:

| Condition | Hue | Badge |
|---|---|---|
| tip == network max | `status-converged` | `converged` |
| tip < max, seen recently | `status-lagging` | `lagging −N` |
| not seen within 3 gossip intervals | `status-offline` | `offline` |
| `supervisor: true` | `status-supervisor` | `supervisor` |

Layout is deterministic (a fixed radial arrangement seeded by node id), so the
graph does not rearrange itself between refreshes. A mesh that reshuffles every
poll is unreadable, and nodes must stay where the user left them.

### 5.3 Inspector
Stacked `card`s, no nesting:

1. **Status** — `/_status`. node_id (`hash-chip`), uptime, engine, quota bar,
   broadcast counters. The quota bar turns amber at 90% and red at 100%,
   matching the supervisor's own degraded threshold so the UI and the engine
   never disagree.
2. **Ledger** — `/_ledger/tip`. entry_id, entry_hash, signature, all as
   `hash-chip`s, plus a **Verify** secondary button that runs
   `POST /_ledger/verify` and reports `entries_checked`, `signed_entries` and
   `unsigned_entries` — all three, because "verified" without the signed count
   overstates the guarantee.
3. **Collections** — a `table`: name, engine badge, document count, checksum
   chip. Checksums differing from the same collection on a peer are marked.
4. **Peers** — a `table`: node, latency, success rate, fitness score, state.
5. **Transit** — only when non-empty. "Holding N documents for M offline
   peers", with the soonest TTL expiry, because held bytes silently expiring is
   the failure mode worth surfacing early.

### 5.4 File explorer
`GET /db/:collection?start=&limit=`, a virtualised list, monospace keys. Right
pane shows the document pretty-printed from `ToJson()`. Inline PUT (edit +
save) and DELETE, with delete confirming in words and stating that a delete is
a CRDT tombstone that will replicate.

Live updates come from `GET /_changes?since=` — the app holds one long poll per
node and applies deltas, so an edit made on another machine appears here within
its round trip, with no polling timer. A `truncated: true` response drops the
cursor and re-reads from scratch rather than pretending continuity.

### 5.5 Creation wizard
Five steps, each a `wizard-step`:

1. **Hardware** — `GET /_supervisor/scan` + `/_supervisor/mounts`. Existing
   nodes are offered for adoption; empty directories for creation. Removable
   volumes are marked and warned about ("this node lives on a drive you can
   unplug").
2. **Shape** — quota slider, at-rest encryption toggle, and a free-text
   description of what the collection is for.
3. **Proposal** — the description is embedded and matched against the seven
   shipped prototypes. Shows the recommended engine, its `confidence-meter`,
   the proposed quota split, shard key, RF and retention, plus the draft
   collections and the exact `PUT /_schema` payloads. **Nothing is applied
   yet.** Below 0.35 confidence the recommendation is replaced by a manual
   engine picker, as specified.
4. **Confirm** — a plain-language summary of every change, then spawn.
5. **Recovery key** — QR + printable text. The wizard cannot be finished until
   the user confirms they have saved it; the supervisor records *that* they did
   (`POST /_supervisor/nodes/:id/recovery-key-exported`) and never the key
   itself. There is no escrow, and the screen says so.

---

## 6. Empty, loading and error states

Every list has all three, and they are written, not generic:

- **Empty (no nodes)** — "No nodes yet. A node is a folder that holds a slice
  of your data." + primary "Create your first node".
- **Empty (no peers)** — "No other nodes found on this network. That is fine —
  this node works completely offline and will sync when it meets one."
  Offline-first is a feature; the empty state should not read as a failure.
- **Loading** — skeleton rows at the real row height. Never a spinner in a
  place that will become a list; the layout must not jump.
- **Error** — the engine's own `Status` message verbatim in
  `{typography.caption}`, plus one concrete next action. Engine messages are
  written to be shown to people (`"engine 'duckdb' is not compiled into this
  build; rebuild with -DDESENTRY_WITH_DUCKDB=ON"`), so paraphrasing them into
  "Something went wrong" would destroy real information.

---

## 7. Implementation notes

- One stylesheet, `app/src/tokens.css`, holds every token as a custom
  property. Component CSS references properties only — no literal hex outside
  that file, so a token change is one edit.
- No CSS framework and no UI framework: the app is TypeScript against the DOM
  inside the Tauri webview, matching the engine's zero-fetched-dependency
  discipline. `tools/dashboard.html` remains a dependency-free fallback for
  driving a single node without the app.
- Icons are inline SVG, 16px, `currentColor`, 1.5px stroke. No icon font.
- The window's minimum size is 1024×640; below that the inspector collapses to
  a sheet and the sidebar to an overlay.
