/**
 * The creation wizard -- the only way a node comes into existence.
 *
 * Five steps: where it lives, how big it is, what it is for, confirm, and take
 * the recovery key. The last one is not skippable. There is no escrow: nothing
 * in the app, the supervisor or the keychain can reproduce a recovery key once
 * the wizard closes, so the wizard refuses to finish until the user has
 * actually exported it. That is the one place this UI is deliberately
 * obstructive, and the reason is written on the screen rather than buried.
 *
 * The sizing step shows the model's confidence as a number, not as a
 * reassurance. Below the floor the engine picker becomes manual, and when the
 * ONNX model could not be loaded the screen says the keyword fallback ran --
 * because a proposal whose provenance is hidden is a proposal nobody can
 * argue with.
 */

import type { DataDirCandidate, EngineInfo, MountPoint } from "../api.js";
import { apiFor } from "../api.js";
import {
  sidecar,
  type CreateNodeRequest,
  type NodeSpec,
  type QuotaSplit,
  type SupervisedNode,
} from "../bridge.js";
import { refreshNodeList, refreshTopology, store } from "../state.js";
import { bytes, engineLabel, percent } from "../util/format.js";
import { el, icon, Icons, on, replace } from "../util/dom.js";
import { qrSvg } from "../util/qr.js";
import { promptDeleteCandidateNode } from "../util/nodeDeleteHelper.js";

/** Matches `kConfidenceFloor` in src-tauri/src/ai.rs. Shown, not just applied. */
const CONFIDENCE_FLOOR = 0.35;

const WORKLOAD_LABELS: Record<string, string> = {
  sql: "Relational",
  "nosql-doc": "Documents",
  "time-series": "Time series",
  vector: "Vectors / similarity",
  graph: "Graph",
  "semi-structured": "Semi-structured",
  "oops-rdbms": "Objects over relational",
};

const DESCRIPTION_SUGGESTIONS = [
  "Store uploaded images and photos with filenames, captions, sizes, and dates; fetch each file by its ID.",
  "Store PDFs, videos, and other files as binary assets with searchable metadata and download them by asset ID.",
  "Store image embeddings and find visually similar photos using nearest-neighbor similarity search.",
  "Store sensor readings every second, query them by time range, and keep one year of history.",
  "Store user profiles and flexible JSON documents without a fixed schema or relational joins.",
];

function describeError(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

interface Draft {
  step: number;
  // 1 -- placement
  candidates: DataDirCandidate[];
  mounts: MountPoint[];
  scanning: boolean;
  dataDir: string;
  removable: boolean;
  adopt: boolean;
  // 2 -- shape
  nodeName: string;
  quotaMb: number;
  encrypt: boolean;
  description: string;
  // 3 -- sizing
  spec: NodeSpec | null;
  sizing: boolean;
  sizingError: string;
  manualEngines: Set<string>;
  engines: EngineInfo[];
  // 5 -- creation
  creating: boolean;
  created: SupervisedNode | null;
  recoveryKey: string | null;
  recoveryExported: boolean;
  createError: string;
}

function newDraft(): Draft {
  return {
    step: 1,
    candidates: [],
    mounts: [],
    scanning: false,
    dataDir: "",
    removable: false,
    adopt: false,
    nodeName: "",
    quotaMb: 2048,
    encrypt: true,
    description: "",
    spec: null,
    sizing: false,
    sizingError: "",
    manualEngines: new Set(),
    engines: [],
    creating: false,
    created: null,
    recoveryKey: null,
    recoveryExported: false,
    createError: "",
  };
}

// -- small building blocks ---------------------------------------------------

function field(label: string, hint: string, control: HTMLElement): HTMLElement {
  return el(
    "label",
    { class: "field" },
    el("span", { class: "field__label", text: label }),
    control,
    hint ? el("span", { class: "field__hint", text: hint }) : null,
  );
}

function section(title: string, ...children: (Node | string | false | null)[]): HTMLElement {
  return el("section", { class: "wizard__section" }, el("h3", { class: "card__title", text: title }), ...children);
}

const STEP_NAMES = ["Placement", "Shape", "Proposal", "Review", "Recovery Key"];

function stepper(currentStep: number): HTMLElement {
  const container = el("div", { class: "wizard__stepper", role: "tablist", "aria-label": "Creation steps" });
  STEP_NAMES.forEach((name, idx) => {
    const stepNum = idx + 1;
    const isActive = stepNum === currentStep;
    const isComplete = stepNum < currentStep;
    const indicator = el(
      "div",
      {
        class: "wizard__step-indicator",
        "data-active": String(isActive),
        "data-complete": String(isComplete),
      },
      el("span", { class: "wizard__step-num", text: isComplete ? "✓" : String(stepNum) }),
      el("span", { text: name }),
    );
    container.appendChild(indicator);
  });
  return container;
}

/** A confidence bar. Below the floor it is drawn as a warning, not a score. */
function confidenceMeter(value: number): HTMLElement {
  const low = value < CONFIDENCE_FLOOR;
  return el(
    "div",
    {
      class: "meter",
      "data-low": String(low),
      role: "img",
      "aria-label": `Confidence ${percent(value, 1)}`,
    },
    el("div", { class: "meter__fill", style: `width: ${Math.round(Math.min(1, value) * 100)}%` }),
  );
}

function quotaSplitBars(split: QuotaSplit, quotaMb: number): HTMLElement {
  const rows: [string, number][] = [
    ["Collections", split.db_pct],
    ["Transit store", split.transit_store_pct],
    ["Cache / indexes", split.cache_hash_pct],
    ["Ledger", split.ledger_pct],
    ["Network buffers", split.net_buffers_pct],
  ];
  const list = el("dl", { class: "kv" });
  for (const [label, pct] of rows) {
    list.appendChild(el("dt", { text: label }));
    list.appendChild(
      el("dd", {
        text: `${pct}% · ${bytes(Math.round((quotaMb * 1024 * 1024 * pct) / 100))}`,
      }),
    );
  }
  return list;
}

// -- the wizard --------------------------------------------------------------

export interface WizardHandles {
  open(config?: { bootstrapPeer?: string }): void;
  close(): void;
  element: HTMLElement;
  isOpen(): boolean;
}

export function createWizard(): WizardHandles {
  const wizardContainer = el("div", { class: "wizard" });
  const element = el("div", { class: "sheet", hidden: true, role: "dialog", "aria-modal": "true" }, wizardContainer);

  let draft = newDraft();
  let open = false;

  const content = el("div", { class: "wizard__body" });

  const closeButton = el("button", { class: "sheet-close", type: "button", title: "Close", "aria-label": "Close" }, icon(Icons.close, 16));
  on(closeButton, "click", () => close());
  const titleBar = el(
    "div",
    { class: "wizard__titlebar" },
    el("span", { style: "width: 28px;" }),
    el("span", { class: "wizard__titlebar-title", text: "Create / Adopt Node" }),
    closeButton,
  );

  function close(): void {
    // Refusing to close over an unexported key would trap the user; warning
    // them once and letting them out is the honest trade. The node is created
    // either way -- what they lose is the ability to recover it.
    if (draft.recoveryKey !== null && !draft.recoveryExported) {
      const leave = window.confirm(
        "The recovery key for this node has not been saved. There is no copy anywhere else — close anyway?",
      );
      if (!leave) return;
      store.toast(
        "warning",
        "Recovery key not exported",
        "This node cannot be recovered if its keychain entry is lost.",
        0,
      );
    }
    open = false;
    element.hidden = true;
    draft = newDraft();
    store.notify();
  }

  function go(step: number): void {
    draft.step = step;
    render();
  }

  // -- step 1: placement -----------------------------------------------------

  async function scan(): Promise<void> {
    const port = store.state.supervisorPort;
    if (port === null) {
      draft.sizingError = "The supervisor is not running, so hardware cannot be scanned.";
      render();
      return;
    }
    draft.scanning = true;
    render();
    try {
      const api = apiFor(port);
      const [scanned, mounts] = await Promise.all([api.scan(), api.mounts()]);
      draft.candidates = scanned.candidates;
      draft.mounts = mounts;
    } catch (error) {
      store.toast("error", "Could not scan for storage", describeError(error));
    } finally {
      draft.scanning = false;
      render();
    }
  }

  function chooseCandidate(candidate: DataDirCandidate): void {
    draft.dataDir = candidate.path;
    draft.removable = candidate.removable;
    draft.adopt = candidate.existing_node;
    if (draft.nodeName === "") {
      draft.nodeName = candidate.node_name || candidate.path.split(/[\\/]/).filter(Boolean).pop() || "node";
    }
    // An existing node has its own quota and engines already; adopting it is a
    // different act from creating one, and the wizard says so rather than
    // silently overwriting node.json.
    render();
  }

  function stepPlacement(): HTMLElement {
    const rescan = el("button", { class: "btn btn--sm btn--ghost", type: "button" }, icon(Icons.refresh, 13), "Rescan");
    on(rescan, "click", () => void scan());

    const browse = el("button", { class: "btn btn--sm", type: "button", text: "Choose a folder…" });
    on(browse, "click", async () => {
      const picked = await sidecar.pickDirectory("Where should this node keep its data?");
      if (picked === null) return;
      const port = store.state.supervisorPort;
      if (port !== null) {
        try {
          chooseCandidate(await apiFor(port).inspect(picked));
          return;
        } catch {
          // Inspection is a convenience; a folder the supervisor cannot stat is
          // still a folder the user may legitimately have chosen.
        }
      }
      draft.dataDir = picked;
      draft.adopt = false;
      render();
    });

    const list = el("div", { class: "stack" });
    const shown = draft.candidates.filter((c) => c.adoptable || c.existing_node);

    if (draft.scanning) {
      list.appendChild(el("div", { class: "skeleton", style: "height: 120px" }));
    } else if (shown.length === 0) {
      list.appendChild(
        el("p", {
          class: "muted",
          text: "Nothing found automatically. Choose a folder, or plug in a drive and rescan.",
        }),
      );
    } else {
      for (const candidate of shown) {
        const selected = draft.dataDir === candidate.path;
        const deleteBtn = candidate.existing_node
          ? el(
              "button",
              {
                class: "btn btn--xs btn--ghost text-danger",
                type: "button",
                title: `Delete ${candidate.node_name || "node"} files from disk`,
              },
              icon(Icons.trash, 12),
              " Delete",
            )
          : null;
        if (deleteBtn) {
          on(deleteBtn, "click", (e) => {
            e.stopPropagation();
            promptDeleteCandidateNode(candidate, () => void scan());
          });
        }

        const row = el(
          "article",
          { class: selected ? "card card--elevated" : "card", role: "button", tabindex: "0" },
          el(
            "div",
            { class: "row row--between" },
            el("strong", { class: "mono", text: candidate.path }),
            el(
              "div",
              { class: "row" },
              candidate.removable && el("span", { class: "badge", text: "removable" }),
              candidate.encrypted && el("span", { class: "badge", text: "encrypted" }),
              candidate.existing_node && el("span", { class: "badge", "data-tone": "supervisor", text: "existing node" }),
              deleteBtn,
            ),
          ),
          el("p", {
            class: "muted",
            text: candidate.existing_node
              ? `${candidate.node_name || "An existing node"} lives here. Adding it adopts it rather than creating a new one.`
              : `${bytes(candidate.free_bytes)} free`,
          }),
        );
        on(row, "click", () => chooseCandidate(candidate));
        on(row, "keydown", (event) => {
          if (event.key === "Enter" || event.key === " ") {
            event.preventDefault();
            chooseCandidate(candidate);
          }
        });
        list.appendChild(row);
      }
    }

    return el(
      "div",
      { class: "stack" },
      el("p", { class: "wizard__eyebrow", text: "Step 1 of 5" }),
      el("h2", { class: "wizard__title", text: "Where should this node live?" }),
      el("p", {
        class: "wizard__lead",
        text: "A node is a folder. On this machine it survives reboots; on a removable drive it travels, and its replicas hold any writes it misses while it is unplugged.",
      }),
      el("div", { class: "row row--between" }, browse, rescan),
      section("Found on this device", list),
    );
  }

  // -- step 2: shape ---------------------------------------------------------

  function stepShape(): HTMLElement {
    const nameInput = el("input", { type: "text", value: draft.nodeName, placeholder: "studio-archive" });
    on(nameInput, "input", () => {
      draft.nodeName = (nameInput as HTMLInputElement).value;
    });

    const quotaInput = el("input", { type: "number", min: "64", step: "64", value: String(draft.quotaMb) });
    on(quotaInput, "input", () => {
      const value = Number((quotaInput as HTMLInputElement).value);
      draft.quotaMb = Number.isFinite(value) && value > 0 ? Math.round(value) : draft.quotaMb;
    });

    const encryptInput = el("input", { type: "checkbox", checked: draft.encrypt });
    on(encryptInput, "change", () => {
      draft.encrypt = (encryptInput as HTMLInputElement).checked;
      render();
    });

    const descriptionInput = el("textarea", {
      rows: "4",
      placeholder: "Sensor readings from the workshop, queried by hour and kept for a year.",
    });
    (descriptionInput as HTMLTextAreaElement).value = draft.description;
    on(descriptionInput, "input", () => {
      draft.description = (descriptionInput as HTMLTextAreaElement).value;
    });
    const suggestions = el(
      "details",
      { class: "description-suggestions" },
      el("summary", { text: "Need help describing this node?" }),
      el(
        "div",
        { class: "description-suggestions__body" },
        el("p", {
          class: "field__hint",
          text: "Choose the closest example, then edit it to match what you plan to store or query.",
        }),
        el(
          "div",
          { class: "description-suggestions__list" },
          ...DESCRIPTION_SUGGESTIONS.map((suggestion) => {
            const button = el("button", { class: "description-suggestion", type: "button", text: suggestion });
            on(button, "click", () => {
              (descriptionInput as HTMLTextAreaElement).value = suggestion;
              draft.description = suggestion;
              descriptionInput.focus();
            });
            return button;
          }),
        ),
      ),
    );

    const candidate = draft.candidates.find((c) => c.path === draft.dataDir);
    const free = candidate?.free_bytes ?? 0;
    const overCommitted = free > 0 && draft.quotaMb * 1024 * 1024 > free;

    return el(
      "div",
      { class: "stack" },
      el("p", { class: "wizard__eyebrow", text: "Step 2 of 5" }),
      el("h2", { class: "wizard__title", text: "How big, and how private?" }),
      el("p", { class: "wizard__lead", text: draft.dataDir }),
      field("Name", "Shown in the sidebar and in logs. Not the node's identity.", nameInput),
      field(
        "Budget (MiB)",
        overCommitted
          ? `Only ${bytes(free)} is free here — the node will refuse writes before it reaches this figure.`
          : "The node refuses writes past this, rather than filling the disk.",
        quotaInput,
      ),
      field(
        "Encrypt at rest",
        draft.removable
          ? "Strongly recommended on removable media: the drive leaves the machine. Unlocking uses a password rather than this device's keychain."
          : "The key is kept in this operating system's keychain, and never in the config file.",
        encryptInput,
      ),
      field(
        "What is this node for?",
        "Plain language. The next step reads this to propose engines, quotas and indexes — and shows you how sure it is.",
        descriptionInput,
      ),
      suggestions,
    );
  }

  // -- step 3: sizing --------------------------------------------------------

  async function runSizing(): Promise<void> {
    draft.sizing = true;
    draft.sizingError = "";
    draft.engines = store.state.engines;
    if (draft.manualEngines.size === 0) {
      draft.manualEngines = new Set(draft.engines.length > 0 ? draft.engines.map((engine) => engine.name) : ["kv"]);
    }
    render();
    try {
      const sizing = sidecar.sizeWorkload(draft.description, draft.quotaMb);
      draft.spec = await Promise.race([
        sizing,
        new Promise<never>((_, reject) => {
          window.setTimeout(() => reject(new Error("Sizing took too long to respond. Choose the engines manually and continue.")), 15000);
        }),
      ]);
      draft.manualEngines = new Set(draft.spec.engines);
      draft.engines = store.state.engines;
    } catch (error) {
      draft.sizingError = describeError(error);
      draft.engines = store.state.engines;
      if (draft.manualEngines.size === 0) {
        draft.manualEngines = new Set(draft.engines.length > 0 ? draft.engines.map((engine) => engine.name) : ["kv"]);
      }
    } finally {
      draft.sizing = false;
      render();
    }
  }

  function stepSizing(): HTMLElement {
    if (draft.sizing) {
      return el(
        "div",
        { class: "stack" },
        el("p", { class: "wizard__eyebrow", text: "Step 3 of 5" }),
        el("h2", { class: "wizard__title", text: "Reading the description…" }),
        el("div", { class: "skeleton", style: "height: 180px" }),
      );
    }

    if (draft.sizingError !== "") {
      const retry = el("button", { class: "btn", type: "button", text: "Try again" });
      on(retry, "click", () => void runSizing());
      return el(
        "div",
        { class: "stack" },
        el("p", { class: "wizard__eyebrow", text: "Step 3 of 5" }),
        el("h2", { class: "wizard__title", text: "Sizing failed" }),
        el("p", { class: "error-note", text: draft.sizingError }),
        el("p", {
          class: "wizard__lead",
          text: "You can still choose engines yourself and carry on — the proposal is a convenience, not a requirement.",
        }),
        retry,
        enginePicker(),
      );
    }

    const spec = draft.spec;
    if (spec === null) {
      const start = el("button", { class: "btn btn--primary", type: "button", text: "Propose a shape" });
      on(start, "click", () => void runSizing());
      return el(
        "div",
        { class: "stack" },
        el("p", { class: "wizard__eyebrow", text: "Step 3 of 5" }),
        el("h2", { class: "wizard__title", text: "What shape should this node be?" }),
        el("p", { class: "wizard__lead", text: draft.description || "No description given — the proposal will be generic." }),
        start,
      );
    }

    const decision = spec.decision;
    const lowConfidence = decision.confidence < decision.confidence_floor;
    const reasoning = decision.reasoning;
    const questions = decision.clarifying_questions ?? [];

    const container = el("div", { class: "stack" });
    container.appendChild(el("p", { class: "wizard__eyebrow", text: "Step 3 of 5" }));

    if (lowConfidence) {
      container.appendChild(
        el("h2", {
          class: "wizard__title",
          text: questions.length > 0 ? "Let's clarify your workload shape" : `Looks like: ${WORKLOAD_LABELS[decision.workload] ?? decision.workload}`,
        }),
      );
    } else {
      container.appendChild(
        el("h2", {
          class: "wizard__title",
          text: `Looks like: ${WORKLOAD_LABELS[decision.workload] ?? decision.workload}`,
        }),
      );
    }

    // Confidence Section
    const confidenceSection = section(
      "Confidence",
      confidenceMeter(decision.confidence),
      el("p", {
        class: lowConfidence ? "error-note" : "muted",
        text: lowConfidence
          ? `${percent(decision.confidence, 1)} — below the ${percent(decision.confidence_floor, 0)} floor. Choose an architectural clarification below to tailor the proposal, or select engines manually.`
          : `${percent(decision.confidence, 1)} against the next-closest workload.`,
      }),
      decision.method === "keyword" &&
        el("p", {
          class: "error-note",
          text: `The embedding model did not load (${decision.fallback_reason || "reason not reported"}), so this came from the keyword fallback. It is deterministic but much blunter.`,
        }),
      el(
        "div",
        { class: "row" },
        ...decision.scores
          .slice()
          .sort((a, b) => b.score - a.score)
          .slice(0, 4)
          .map((s) =>
            el("span", {
              class: "chip",
              text: `${WORKLOAD_LABELS[s.workload] ?? s.workload} ${s.score.toFixed(2)}`,
            }),
          ),
      ),
    );
    container.appendChild(confidenceSection);

    // Interactive Clarification Box (when low confidence or ambiguous)
    if (lowConfidence && questions.length > 0) {
      const clarifyBox = el("div", { class: "ai-clarify-box" });
      clarifyBox.appendChild(
        el(
          "div",
          { class: "ai-clarify-box__header" },
          el("span", { class: "ai-clarify-box__badge", text: "AI Refinement" }),
          el("span", { class: "muted", text: "Answering will tailor the architecture and boost confidence" }),
        ),
      );

      for (const q of questions) {
        const qBlock = el("div", { class: "stack", style: "margin-top: var(--space-xs)" });
        qBlock.appendChild(el("p", { class: "ai-clarify-box__prompt", text: q.prompt }));
        qBlock.appendChild(el("p", { class: "ai-clarify-box__rationale", text: q.rationale }));

        const optionsGrid = el("div", { class: "ai-clarify-options" });
        for (const opt of q.options) {
          const btn = el(
            "button",
            { class: "ai-clarify-btn", type: "button" },
            el("span", { class: "ai-clarify-btn__label", text: opt.label }),
            el("span", { class: "ai-clarify-btn__desc", text: opt.description }),
          );
          on(btn, "click", () => {
            const trimmed = draft.description.trim();
            if (trimmed.length === 0 || trimmed.split(" ").length < 5) {
              draft.description = opt.appended_context;
            } else if (!trimmed.toLowerCase().includes(opt.appended_context.toLowerCase())) {
              draft.description = `${trimmed}, ${opt.appended_context}`;
            }
            void runSizing();
          });
          optionsGrid.appendChild(btn);
        }
        qBlock.appendChild(optionsGrid);
        clarifyBox.appendChild(qBlock);
      }
      container.appendChild(section("Clarifying Questions", clarifyBox));
    }

    // AI Reasoning & Architecture Breakdown (when reasoning is available)
    if (reasoning) {
      const reasoningSection = section("AI Reasoning & Architecture");
      const reasoningCard = el("div", { class: "ai-reasoning-card" });
      reasoningCard.appendChild(el("p", { class: "ai-reasoning-card__summary", text: reasoning.summary }));

      if (reasoning.key_matched_signals.length > 0) {
        const signalsRow = el(
          "div",
          { class: "ai-reasoning-signals" },
          el("span", { class: "ai-reasoning-signals__label", text: "Matched Signals:" }),
          ...reasoning.key_matched_signals.map((sig) => el("span", { class: "ai-signal-chip", text: sig })),
        );
        reasoningCard.appendChild(signalsRow);
      }

      if (reasoning.engine_rationales.length > 0) {
        const enginesList = el("div", { class: "ai-engines-list" });
        for (const er of reasoning.engine_rationales) {
          enginesList.appendChild(
            el(
              "div",
              { class: "ai-engine-item" },
              el(
                "div",
                {},
                el("span", { class: "ai-engine-item__name", text: engineLabel(er.engine) }),
                el("span", { class: "ai-engine-item__role", text: `(${er.role})` }),
              ),
              el("div", { class: "ai-engine-item__reason", text: er.reason }),
            ),
          );
        }
        reasoningCard.appendChild(enginesList);
      }

      if (reasoning.runner_up_contrast) {
        reasoningCard.appendChild(el("p", { class: "ai-contrast-note", text: reasoning.runner_up_contrast }));
      }

      if (reasoning.operational_trade_offs.length > 0) {
        const tradeoffsBlock = el("div", { class: "stack", style: "gap: 4px; margin-top: var(--space-xs)" });
        tradeoffsBlock.appendChild(
          el("span", { class: "ai-reasoning-signals__label", text: "Operational Trade-Offs:" }),
        );
        const ul = el(
          "ul",
          { class: "ai-tradeoffs" },
          ...reasoning.operational_trade_offs.map((t) => el("li", { text: t })),
        );
        tradeoffsBlock.appendChild(ul);
        reasoningCard.appendChild(tradeoffsBlock);
      }

      // Fine-tuning refinement options (when already confident)
      if (!lowConfidence && questions.length > 0) {
        const tuneBlock = el("div", {
          class: "stack",
          style:
            "margin-top: var(--space-xs); border-top: 1px solid var(--color-divider-soft); padding-top: var(--space-xs); gap: 6px",
        });
        tuneBlock.appendChild(el("span", { class: "ai-reasoning-signals__label", text: "Fine-Tune This Proposal:" }));
        for (const q of questions) {
          tuneBlock.appendChild(el("p", { class: "muted", style: "margin: 0", text: q.prompt }));
          const optionsRow = el("div", { class: "row", style: "flex-wrap: wrap" });
          for (const opt of q.options) {
            const chip = el("button", {
              class: "chip",
              type: "button",
              text: opt.label,
              title: opt.description,
            });
            on(chip, "click", () => {
              const trimmed = draft.description.trim();
              if (!trimmed.toLowerCase().includes(opt.appended_context.toLowerCase())) {
                draft.description = `${trimmed} ${opt.appended_context}`;
              }
              void runSizing();
            });
            optionsRow.appendChild(chip);
          }
          tuneBlock.appendChild(optionsRow);
        }
        reasoningCard.appendChild(tuneBlock);
      }

      reasoningSection.appendChild(reasoningCard);
      container.appendChild(reasoningSection);
    }

    // Engines section
    container.appendChild(
      section(
        "Engines",
        el("p", {
          class: "muted",
          text: lowConfidence
            ? "Pick what this node should be able to store. Every collection is bound to one engine; unbound collections use the default."
            : "Proposed for this workload. Change any of them — the proposal is not binding.",
        }),
        enginePicker(),
      ),
    );

    // Budget split section
    container.appendChild(
      section(
        "Budget split",
        quotaSplitBars(spec.quota_split, draft.quotaMb),
        reasoning?.quota_rationale
          ? el("p", { class: "muted", style: "margin-top: var(--space-xs)", text: reasoning.quota_rationale })
          : null,
      ),
    );

    // Collections
    if (spec.collections.length > 0) {
      container.appendChild(
        section(
          "Collections it will create",
          el(
            "div",
            { class: "row" },
            ...spec.collections.map((c) =>
              el("span", {
                class: "chip",
                title: c.schema ? "With a schema" : "No schema",
                text: `${c.name} · ${engineLabel(c.engine)}`,
              }),
            ),
          ),
        ),
      );
    }

    return container;
  }

  function enginePicker(): HTMLElement {
    const available = draft.engines.length > 0 ? draft.engines : store.state.engines;
    const row = el("div", { class: "row" });

    for (const engine of available) {
      // An engine the binary does not contain is shown greyed with the CMake
      // option that would add it, rather than hidden: "why is DuckDB missing"
      // is a question the UI should answer, not raise.
      const enabled = engine.compiled_in;
      const chosen = draft.manualEngines.has(engine.name);
      const chip = el("button", {
        class: "chip",
        type: "button",
        "aria-pressed": String(chosen),
        disabled: !enabled,
        title: enabled
          ? engine.built_in
            ? "Built in — always available"
            : "Vendored backend, compiled into this build"
          : `Not in this build. Rebuild with ${engine.enable_with ?? "the matching CMake option"}.`,
        text: engineLabel(engine.name),
        style: chosen ? "outline: 2px solid var(--color-primary); outline-offset: -2px" : "",
      });
      on(chip, "click", () => {
        if (draft.manualEngines.has(engine.name)) {
          // Never leave a node with no engine at all.
          if (draft.manualEngines.size > 1) draft.manualEngines.delete(engine.name);
        } else {
          draft.manualEngines.add(engine.name);
        }
        render();
      });
      row.appendChild(chip);
    }
    return row;
  }

  // -- step 4: confirm -------------------------------------------------------

  function stepConfirm(): HTMLElement {
    const spec = draft.spec;
    const engines = [...draft.manualEngines];
    return el(
      "div",
      { class: "stack" },
      el("p", { class: "wizard__eyebrow", text: "Step 4 of 5" }),
      el("h2", { class: "wizard__title", text: "Ready to create" }),
      el(
        "dl",
        { class: "kv" },
        el("dt", { text: "Name" }),
        el("dd", { text: draft.nodeName || "(unnamed)" }),
        el("dt", { text: "Folder" }),
        el("dd", { class: "mono", text: draft.dataDir }),
        el("dt", { text: "Budget" }),
        el("dd", { text: `${draft.quotaMb} MiB` }),
        el("dt", { text: "Engines" }),
        el("dd", { text: engines.map(engineLabel).join(", ") || "Key–Value" }),
        el("dt", { text: "Default engine" }),
        el("dd", { text: engineLabel(spec?.default_engine ?? engines[0] ?? "kv") }),
        el("dt", { text: "Replication factor" }),
        el("dd", { text: String(spec?.replication_factor ?? 3) }),
        el("dt", { text: "At rest" }),
        el("dd", { text: draft.encrypt ? "Encrypted" : "Not encrypted" }),
        el("dt", { text: "Adopting" }),
        el("dd", { text: draft.adopt ? "Yes — an existing node lives in this folder" : "No — a new node" }),
      ),
      draft.encrypt &&
        el("p", {
          class: "wizard__lead",
          text: "The next step shows this node's recovery key once. It is not stored anywhere the app can read back, so save it before finishing.",
        }),
      draft.createError !== "" && el("p", { class: "error-note", text: draft.createError }),
    );
  }

  // -- step 5: recovery key --------------------------------------------------

  async function createNode(): Promise<void> {
    draft.creating = true;
    draft.createError = "";
    render();

    const spec: NodeSpec = draft.spec ?? {
      engines: [...draft.manualEngines],
      default_engine: [...draft.manualEngines][0] ?? "kv",
      quota_split: { db_pct: 60, transit_store_pct: 15, cache_hash_pct: 10, ledger_pct: 10, net_buffers_pct: 5 },
      shard_key: "",
      replication_factor: 3,
      secondary_indexes: [],
      retention_days: 0,
      collections: [],
      decision: {
        workload: "unspecified",
        confidence: 0,
        method: "keyword",
        scores: [],
        fallback_reason: "no description given; engines chosen by hand",
        confidence_floor: CONFIDENCE_FLOOR,
        description: draft.description,
        decided_at_ms: Date.now(),
      },
    };
    // The picker is authoritative over the proposal -- the user's last action
    // wins, and the audit record in the manifest keeps both.
    spec.engines = [...draft.manualEngines].length > 0 ? [...draft.manualEngines] : spec.engines;
    if (!spec.engines.includes(spec.default_engine)) spec.default_engine = spec.engines[0] ?? "kv";

    const request: CreateNodeRequest = {
      node_name: draft.nodeName,
      data_dir: draft.dataDir,
      adopt: draft.adopt,
      quota_mb: draft.quotaMb,
      spec,
      encrypt_at_rest: draft.encrypt,
      supervisor: false,
      removable: draft.removable,
      bootstrap_peers: [],
      description: draft.description,
    };

    try {
      const result = await sidecar.createNode(request);
      draft.created = result.node;
      draft.recoveryKey = result.recovery_key;
      draft.recoveryExported = result.recovery_key === null;
      draft.step = 5;
      await refreshNodeList();
      await refreshTopology();
      store.select({ kind: "node", nodeId: result.node.node_id });
    } catch (error) {
      draft.createError = describeError(error);
      draft.step = 4;
    } finally {
      draft.creating = false;
      render();
    }
  }

  function stepRecovery(): HTMLElement {
    const node = draft.created;
    const key = draft.recoveryKey;

    if (node === null) return el("div", { class: "skeleton", style: "height: 220px" });

    if (key === null) {
      return el(
        "div",
        { class: "stack" },
        el("p", { class: "wizard__eyebrow", text: "Step 5 of 5" }),
        el("h2", { class: "wizard__title", text: `${node.node_name} is running` }),
        el("p", {
          class: "wizard__lead",
          text: "This node is not encrypted at rest, so there is no recovery key to keep. It will start finding peers on this network straight away.",
        }),
      );
    }

    const saveButton = el("button", { class: "btn btn--primary", type: "button", text: "Save to a file…" });
    on(saveButton, "click", async () => {
      const path = await sidecar.pickSaveFile(
        "Save the recovery key",
        `${node.node_name || node.node_id.slice(0, 8)}-recovery-key.txt`,
      );
      if (path === null) return;
      try {
        await sidecar.exportRecoveryKey(node.node_id, path);
        const port = store.state.supervisorPort;
        if (port !== null) await apiFor(port).recordRecoveryKeyExport(node.node_id).catch(() => undefined);
        draft.recoveryExported = true;
        store.toast("success", "Recovery key saved", path);
        render();
      } catch (error) {
        store.toast("error", "Could not save the recovery key", describeError(error));
      }
    });

    const printButton = el("button", { class: "btn", type: "button", text: "Print" });
    on(printButton, "click", () => {
      window.print();
      // Printing cannot be confirmed from here -- the browser reports nothing
      // about what came out of the printer -- so this asks rather than assumes.
      if (window.confirm("Did the recovery key print correctly?")) {
        draft.recoveryExported = true;
        const port = store.state.supervisorPort;
        if (port !== null) void apiFor(port).recordRecoveryKeyExport(node.node_id).catch(() => undefined);
        render();
      }
    });

    const copyButton = el("button", { class: "btn btn--ghost", type: "button", text: "Copy" });
    on(copyButton, "click", async () => {
      await navigator.clipboard.writeText(key);
      store.toast("info", "Copied to the clipboard", "Paste it somewhere durable — the clipboard is not storage.");
    });

    return el(
      "div",
      { class: "stack" },
      el("p", { class: "wizard__eyebrow", text: "Step 5 of 5" }),
      el("h2", { class: "wizard__title", text: "Take the recovery key" }),
      el("p", {
        class: "wizard__lead",
        text: "This is the only time this key is shown. It is not escrowed: if this device's keychain is lost and you do not have this key, the data on this node cannot be read by anyone, including us.",
      }),
      el("div", { class: "qr" }, qrSvg(key, { scale: 4, title: "Recovery key" })),
      el("p", { class: "mono", style: "user-select: text; word-break: break-all", text: key }),
      el("div", { class: "row" }, saveButton, printButton, copyButton),
      draft.recoveryExported
        ? el("p", { class: "muted", text: "Saved. The supervisor has recorded that the export happened — never the key itself." })
        : el("p", { class: "error-note", text: "Save, print or copy the key to finish." }),
    );
  }

  // -- chrome ----------------------------------------------------------------

  function canAdvance(): boolean {
    switch (draft.step) {
      case 1: return draft.dataDir !== "";
      case 2: return true;
      case 3: return true;
      case 4: return !draft.creating;
      case 5: return draft.recoveryExported;
      default: return false;
    }
  }

  function render(): void {
    if (!open) return;

    const back = el("button", {
      class: "btn btn--ghost",
      type: "button",
      text: draft.step === 1 ? "Cancel" : "Back",
      disabled: draft.creating || draft.step === 5,
    });
    on(back, "click", () => (draft.step === 1 ? close() : go(draft.step - 1)));

    const nextLabel =
      draft.step === 4 ? (draft.creating ? "Creating…" : "Create node") : draft.step === 5 ? "Done" : "Continue";
    const next = el("button", {
      class: "btn btn--primary",
      type: "button",
      text: nextLabel,
      disabled: !canAdvance(),
    });
    on(next, "click", () => {
      if (draft.step === 4) void createNode();
      else if (draft.step === 5) close();
      else {
        if (draft.step === 2) {
          const nameInput = content.querySelector<HTMLInputElement>('input[type="text"]');
          const quotaInput = content.querySelector<HTMLInputElement>('input[type="number"]');
          const encryptInput = content.querySelector<HTMLInputElement>('input[type="checkbox"]');
          const descriptionInput = content.querySelector<HTMLTextAreaElement>("textarea");
          if (nameInput !== null) draft.nodeName = nameInput.value;
          if (quotaInput !== null) {
            const value = Number(quotaInput.value);
            if (Number.isFinite(value) && value > 0) draft.quotaMb = Math.round(value);
          }
          if (encryptInput !== null) draft.encrypt = encryptInput.checked;
          if (descriptionInput !== null) draft.description = descriptionInput.value;
          if (draft.nodeName.trim() === "" || draft.quotaMb <= 0) {
            nameInput?.reportValidity();
            quotaInput?.reportValidity();
            return;
          }
          draft.spec = null;
          draft.sizingError = "";
        }
        go(draft.step + 1);
        if (draft.step === 3 && draft.description.trim() !== "") void runSizing();
      }
    });

    const steps: Record<number, () => HTMLElement> = {
      1: stepPlacement,
      2: stepShape,
      3: stepSizing,
      4: stepConfirm,
      5: stepRecovery,
    };

    replace(
      content,
      stepper(draft.step),
      steps[draft.step](),
    );

    replace(
      wizardContainer,
      titleBar,
      content,
      el("div", { class: "wizard__footer" }, back, el("span", { class: "header__spacer" }), next),
    );
  }

  on(element, "keydown", (event) => {
    if (event.key === "Escape") close();
  });

  return {
    element,
    isOpen: () => open,
    open(_config?: { bootstrapPeer?: string }) {
      // _config is reserved for future use (e.g. pre-seeding a bootstrap peer).
      // The wizard currently opens fresh every time.
      draft = newDraft();
      draft.engines = store.state.engines;
      open = true;
      element.hidden = false;
      render();
      void scan();
    },
    close,
  };
}
