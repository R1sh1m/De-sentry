// Guards the native-interaction invariants: dialogs must be real <dialog>
// elements, the context menu must use the popover API, the sidebar tree must
// keep exactly one tab stop (roving tabindex), and focus styling must come
// from tokens rather than literal outlines scattered through the CSS.
//
// These are invisible in review and regress silently -- a div-with-a-class
// renders identically to a <dialog> right up until a keyboard user tabs out
// of the modal into the page behind it.
//
// Run with: npm run check:a11y

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const src = (...parts) => join(here, "..", "src", ...parts);

const css = readFileSync(src("styles.css"), "utf8");
const tokens = readFileSync(src("tokens.css"), "utf8");
const sidebar = readFileSync(src("views", "sidebar.ts"), "utf8");
const unlock = readFileSync(src("views", "unlockModal.ts"), "utf8");
const deleter = readFileSync(src("views", "deleteNodeModal.ts"), "utf8");
const main = readFileSync(src("main.ts"), "utf8");

const failures = [];
const check = (ok, message) => {
  if (!ok) failures.push(message);
};

// 1. Modals are native <dialog> elements, so focus trap / Esc / focus-restore
//    come from the browser instead of hand-rolled listeners.
for (const [name, source] of [["unlockModal.ts", unlock], ["deleteNodeModal.ts", deleter], ["main.ts (About)", main]]) {
  check(
    source.includes('el("dialog"'),
    `${name} does not create its modal with el("dialog" -- a div replacement loses the native focus trap, Esc handling and focus restoration.`
  );
}

// 2. The native dialog backdrop is styled; an unstyled ::backdrop is a
//    full-black default that ignores the frosted-chrome system.
check(
  css.includes("dialog::backdrop"),
  "styles.css has no `dialog::backdrop` rule -- native dialogs would paint the UA-default opaque backdrop."
);

// 3. Context menu uses popover="auto" for native light-dismiss and top-layer
//    stacking instead of document-level capture listeners.
check(
  sidebar.includes('popover: "auto"') && sidebar.includes("showPopover"),
  "sidebar.ts ctx-menu does not use popover=\"auto\" + showPopover() -- light-dismiss falls back to hand-rolled listeners."
);

// 4. Roving tabindex: tree rows must not each be a tab stop. Rows render with
//    tabindex -1 plus a stable data-focus-id; exactly one is promoted to 0
//    after render and on focusin.
check(
  sidebar.includes("data-focus-id") && sidebar.includes("applyRoving"),
  "sidebar.ts has no roving-tabindex machinery (data-focus-id + applyRoving) -- every tree row becomes a tab stop."
);
const hardTabStops = (sidebar.match(/"tabindex":\s*"0"/g) ?? []).length;
check(
  hardTabStops === 0,
  `sidebar.ts hard-codes tabindex "0" ${hardTabStops} time(s) -- tree rows must render tabindex "-1" and let applyRoving() promote exactly one. (Pinned nav uses native buttons, which need no tabindex.)`
);
check(
  sidebar.includes("ArrowDown") && sidebar.includes("ArrowUp"),
  "sidebar.ts has no ArrowUp/ArrowDown tree navigation -- keyboard users must tab through every row."
);

// 5. Focus styling is a token, not a literal scattered across components.
check(
  tokens.includes("--focus-ring"),
  "tokens.css defines no --focus-ring token -- focus styling will drift into per-component literals."
);
check(
  /:focus-visible\s*\{[^}]*var\(--focus-ring\)/.test(css),
  "styles.css :focus-visible does not reference var(--focus-ring) -- the shadcn-style double ring is not actually applied."
);

// 6. Overlay elevation tier exists so dialogs/menus/palette share one surface.
check(
  tokens.includes("--color-surface-popover"),
  "tokens.css defines no --color-surface-popover tier -- overlays will each invent their own surface."
);

if (failures.length > 0) {
  console.error("a11y: CHECKS FAILED\n");
  for (const failure of failures) console.error("  - " + failure + "\n");
  process.exit(1);
}

console.log("a11y: ALL CHECKS PASSED (dialog, popover, roving tabindex, focus tokens)");
