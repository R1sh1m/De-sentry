// Guards one CSS invariant that is invisible in review and fatal at runtime.
//
// The app hides the wizard with the `hidden` attribute (`element.hidden =
// true`). That attribute has no power of its own: it works only through the
// user-agent stylesheet's `[hidden] { display: none }`, and a user-agent
// declaration loses to *any* author declaration for the same property. So the
// moment a component rule sets `display` -- `.sheet { display: grid }` -- the
// element the code believes is hidden renders anyway.
//
// For `.sheet` that meant a full-window scrim at `inset: 0` and `z-index: 40`
// painted over the entire app, eating every click. The app started, drew
// correctly underneath, and was completely unusable.
//
// Run with: npm run check:css

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const cssPath = join(here, "..", "src", "styles.css");
const tsFiles = [
  join(here, "..", "src", "views", "wizard.ts"),
  join(here, "..", "src", "main.ts"),
];

const css = readFileSync(cssPath, "utf8");
const failures = [];

// 1. The global rule has to exist, and it has to be !important.
//
// Not pedantry: `[hidden]` and a single class have identical specificity, so
// without !important whether an element hides depends on which rule happens to
// sit later in the file. A correctness property must not hinge on edit order.
const hiddenRule = /\[hidden\]\s*\{[^}]*display\s*:\s*none\s*!important/;
if (!hiddenRule.test(css)) {
  failures.push(
    "styles.css has no `[hidden] { display: none !important }` rule. Any class " +
      "that sets `display` will override the user agent's `hidden` handling, " +
      "and elements the code hides will render anyway."
  );
}

// 2. That rule must come before the component rules it has to beat. It wins on
//    !important regardless, but a reader who finds it at the bottom of the file
//    will reasonably assume order matters somewhere.
const hiddenAt = css.search(hiddenRule);
// A class rule, anchored to the start of a line -- not just the text ".sheet",
// which also occurs inside comments (including the one above this rule).
const firstComponentAt = css.search(/^\.[\w-]+[^{}]*\{/m);
if (hiddenAt >= 0 && firstComponentAt >= 0 && hiddenAt > firstComponentAt) {
  failures.push(
    "the `[hidden]` rule appears after the component rules; keep it in the " +
      "reset block at the top where it reads as a base guarantee."
  );
}

// 3. Anything toggled through the `hidden` attribute is covered by the rule
//    above -- this just reports what depends on it, so a reviewer changing the
//    reset can see the cost.
const users = [];
for (const file of tsFiles) {
  const source = readFileSync(file, "utf8");
  if (/\.hidden\s*=|hidden:\s*(true|false|!)/.test(source)) {
    users.push(file.split(/[\\/]/).slice(-2).join("/"));
  }
}

if (failures.length > 0) {
  console.error("styles.css: CHECKS FAILED\n");
  for (const failure of failures) console.error("  - " + failure + "\n");
  process.exit(1);
}

console.log(
  `styles.css: ALL CHECKS PASSED (hidden-attribute users: ${users.join(", ") || "none"})`
);
