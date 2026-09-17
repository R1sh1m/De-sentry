/**
 * Shared empty state: watermark + title + body + optional actions.
 *
 * The canvas, sidebar and intake views each grew their own copy; they now
 * share one rhythm (and one glow treatment) so an empty screen reads as the
 * same app everywhere. Copy stays per-call-site -- the words differ, the
 * shape does not.
 */

import { el } from "./dom.js";
import { sentryWatermarkSvg } from "./logo.js";

export interface EmptyStateOptions {
  title: string;
  body: string;
  actions?: HTMLElement[];
  watermarkSize?: number;
}

export function emptyState(options: EmptyStateOptions): HTMLElement {
  return el(
    "div",
    { class: "empty" },
    el(
      "div",
      { class: "empty__watermark empty__watermark--glow" },
      sentryWatermarkSvg(options.watermarkSize ?? 110),
    ),
    el("p", { class: "empty__title", text: options.title }),
    el("p", { class: "empty__body", text: options.body }),
    ...(options.actions ?? []),
  );
}
