/**
 * Tiny DOM helpers.
 *
 * There is no UI framework here on purpose: the engine's whole discipline is
 * zero fetched dependencies, and a control room whose UI layer is 200 lines of
 * `createElement` is a control room whose UI layer nobody has to audit. These
 * helpers exist so that building an element tree reads like the tree it
 * builds.
 */

export type Attrs = Record<string, string | number | boolean | null | undefined>;
export type Child = Node | string | number | null | undefined | false;

/** Creates an element with attributes and children in one call. */
export function el<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  attrs: Attrs = {},
  ...children: Child[]
): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  applyAttrs(node, attrs);
  append(node, children);
  return node;
}

/** Same, for SVG -- which needs createElementNS or nothing renders. */
export function svg(tag: string, attrs: Attrs = {}, ...children: Child[]): SVGElement {
  const node = document.createElementNS("http://www.w3.org/2000/svg", tag);
  for (const [key, value] of Object.entries(attrs)) {
    if (value === null || value === undefined || value === false) continue;
    node.setAttribute(key, String(value));
  }
  append(node as unknown as HTMLElement, children);
  return node;
}

function applyAttrs(node: HTMLElement, attrs: Attrs): void {
  for (const [key, value] of Object.entries(attrs)) {
    if (value === null || value === undefined || value === false) continue;
    if (key === "class") {
      node.className = String(value);
    } else if (key === "text") {
      node.textContent = String(value);
    } else if (value === true) {
      node.setAttribute(key, "");
    } else {
      node.setAttribute(key, String(value));
    }
  }
}

function append(node: HTMLElement, children: Child[]): void {
  for (const child of children) {
    if (child === null || child === undefined || child === false) continue;
    node.append(child instanceof Node ? child : document.createTextNode(String(child)));
  }
}

export function clear(node: Element): void {
  while (node.firstChild) node.removeChild(node.firstChild);
}

export function replace(node: Element, ...children: Child[]): void {
  clear(node);
  append(node as HTMLElement, children);
}

export function on<K extends keyof HTMLElementEventMap>(
  node: Element,
  type: K,
  handler: (event: HTMLElementEventMap[K]) => void,
): () => void {
  node.addEventListener(type, handler as EventListener);
  return () => node.removeEventListener(type, handler as EventListener);
}

/** 16px inline icon. No icon font, no sprite sheet -- one path each. */
export function icon(path: string, size = 16): SVGElement {
  return svg(
    "svg",
    {
      width: size,
      height: size,
      viewBox: "0 0 16 16",
      fill: "none",
      stroke: "currentColor",
      "stroke-width": 1.5,
      "stroke-linecap": "round",
      "stroke-linejoin": "round",
      "aria-hidden": "true",
    },
    svg("path", { d: path }),
  );
}

export const Icons = {
  chevronRight: "M6 3.5 10.5 8 6 12.5",
  chevronLeft: "M10.5 3.5 6 8 10.5 12.5",
  folder: "M1.75 4.25A1.25 1.25 0 0 1 3 3h3l1.5 1.75h5.5A1.25 1.25 0 0 1 14.25 6v6A1.25 1.25 0 0 1 13 13.25H3A1.25 1.25 0 0 1 1.75 12z",
  drive: "M2.5 9.5h11M3.5 9.5 5 3.5h6l1.5 6v3h-9zM5 11.5h.01",
  node: "M8 1.75 13.75 5v6L8 14.25 2.25 11V5zM8 14.25V8M13.75 5L8 8 2.25 5",
  network: "M8 1.75a1.75 1.75 0 1 0 0 3.5 1.75 1.75 0 1 0 0-3.5M3.25 10.25a1.75 1.75 0 1 0 0 3.5 1.75 1.75 0 1 0 0-3.5m9.5 0a1.75 1.75 0 1 0 0 3.5 1.75 1.75 0 1 0 0-3.5M6.8 6.5L4.4 9.6M9.2 6.5l2.4 3.1M5 12h6",
  shield: "M8 1.75 13 3.5v4c0 3.2-2.1 5.6-5 6.75-2.9-1.15-5-3.55-5-6.75v-4z",
  refresh: "M13.5 8a5.5 5.5 0 1 1-1.6-3.9M13.5 2v3h-3",
  plus: "M8 3.5v9M3.5 8h9",
  close: "M4 4l8 8M12 4l-8 8",
  copy: "M5.5 5.5V3.25A1.25 1.25 0 0 1 6.75 2h6A1.25 1.25 0 0 1 14 3.25v6A1.25 1.25 0 0 1 12.75 10.5H10.5M2 6.75A1.25 1.25 0 0 1 3.25 5.5h6A1.25 1.25 0 0 1 10.5 6.75v6A1.25 1.25 0 0 1 9.25 14h-6A1.25 1.25 0 0 1 2 12.75z",
  search: "M10.2 10.2l3.3 3.3M7 11.5a4.5 4.5 0 1 0 0-9 4.5 4.5 0 0 0 0 9z",
  inspector: "M2.5 3.5h11a1 1 0 0 1 1 1v7a1 1 0 0 1-1 1h-11a1 1 0 0 1-1-1v-7a1 1 0 0 1 1-1zM10.5 3.5v9",
  sun: "M8 5.5a2.5 2.5 0 1 0 0 5 2.5 2.5 0 0 0 0-5zM8 1.5v1.5M8 13v1.5M1.5 8h1.5M13 8h1.5M3.4 3.4l1.1 1.1M11.5 11.5l1.1 1.1M3.4 12.6l1.1-1.1M11.5 4.5l1.1-1.1",
  moon: "M13.5 9.8A5.5 5.5 0 0 1 6.2 2.5 5.5 5.5 0 1 0 13.5 9.8z",
  zoomIn: "M7 11a4 4 0 1 0 0-8 4 4 0 0 0 0 8zm3 3-2.5-2.5M7 5v4M5 7h4",
  zoomOut: "M7 11a4 4 0 1 0 0-8 4 4 0 0 0 0 8zm3 3-2.5-2.5M5 7h4",
  zoomReset: "M2.5 8a5.5 5.5 0 0 1 9.4-3.9M12 2v3.5H8.5M13.5 8a5.5 5.5 0 0 1-9.4 3.9M4 14v-3.5h3.5",
  mesh: "M8 1.5a1.75 1.75 0 1 0 0 3.5 1.75 1.75 0 1 0 0-3.5M3.5 10.75a1.75 1.75 0 1 0 0 3.5 1.75 1.75 0 1 0 0-3.5M12.5 10.75a1.75 1.75 0 1 0 0 3.5 1.75 1.75 0 1 0 0-3.5M5.25 12.5h5.5M7.2 5.1L4.3 10.9M8.8 5.1L11.7 10.9",
  tree: "M5.5 1.5h5v3.5h-5zM8 5v3M3.5 11V8h9v3M1 11h5v3.5h-5zM10 11h5v3.5h-5z",
  chart: "M2.5 13.5h11M3.5 10.5l3-4 3 2.5 3.5-5",
  check: "M3.5 8.5l3 3 6-7",
  trash: "M2.5 4.5h11M5.5 4.5V2.75a.75.75 0 0 1 .75-.75h3.5a.75.75 0 0 1 .75.75V4.5M6 7.5v4M10 7.5v4M3.5 4.5l.8 9a1.25 1.25 0 0 0 1.25 1.15h4.9a1.25 1.25 0 0 0 1.25-1.15l.8-9",
  warning: "M8 2.5 14.5 13.5H1.5zM8 7v3.5M8 12.5h.01",
  plug: "M6 2.5v3M10 2.5v3M5 5.5h6v3A3 3 0 0 1 8 11.5 3 3 0 0 1 5 8.5zm3 6v2",
  terminal: "M2.5 4.5l4 3.5-4 3.5M7.5 11.5h6",
  inbox: "M2.5 9.5 4 3.5h8l1.5 6v3a1 1 0 0 1-1 1h-9a1 1 0 0 1-1-1zm0 0h3.5a1.5 1.5 0 0 0 3 0H13.5",
  ledger: "M3.5 2.5h9a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1h-9a1 1 0 0 1-1-1v-9a1 1 0 0 1 1-1zm2 3.5h5m-5 3h5m-5 3h3",
  /** Closed padlock — node is encrypted and locked */
  lock: "M5 7.5V5.5a3 3 0 0 1 6 0v2M3.5 7.5h9a1 1 0 0 1 1 1v5a1 1 0 0 1-1 1h-9a1 1 0 0 1-1-1v-5a1 1 0 0 1 1-1zM8 10v2",
  /** Open padlock — node is encrypted and unlocked/running */
  lockOpen: "M5 7.5V5.5a3 3 0 0 1 6 0M3.5 7.5h9a1 1 0 0 1 1 1v5a1 1 0 0 1-1 1h-9a1 1 0 0 1-1-1v-5a1 1 0 0 1 1-1zM8 10v2",
  /** Pairing: QR code — scan to link another device. Replaces the generic shield. */
  qr: "M2 2h4.5v4.5H2zM9.5 2h4.5v4.5H9.5zM2 9.5h4.5v4.5H2zM4.25 4.25h.01M11.75 4.25h.01M4.25 11.75h.01M9.5 9.5h2M14 9.5v2.5M9.5 12.5v1.5M12 14h2M11.75 11.75h.01",
  /** Window chrome (custom titlebar when decorations are off) */
  minus: "M3.5 8h9",
  square: "M3.5 3.5h9v9h-9z",
};
