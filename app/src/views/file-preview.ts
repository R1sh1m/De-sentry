/**
 * File preview modal component.
 *
 * Shows a preview of a file based on its MIME type:
 * - PDF: embedded viewer
 * - Images: full-size with zoom
 * - Office files: metadata + "Open in System App" button
 * - Other binaries: info + "Open in System App" or "Save As..."
 */

import { apiToken } from "../api.js";
import { sidecar } from "../bridge.js";
import { store } from "../state.js";
import { bytes, timestamp } from "../util/format.js";
import { el, icon, Icons, on } from "../util/dom.js";
import { getFileIcon, getFileCategory, isPreviewable, isOfficeFile, isTextFile, categoryLabel } from "../file-types.js";

export interface FileMeta {
  collection: string;
  key: string;
  filename: string;
  mime: string;
  size_bytes: number;
  sha256: string;
  created_ms: number;
  nodePort: number;
}

let activeModal: HTMLElement | null = null;
let activeObjectUrls: string[] = [];

function fileStreamUrl(port: number, collection: string, key: string): string {
  return `http://127.0.0.1:${port}/_files/${encodeURIComponent(collection)}/${encodeURIComponent(key)}/stream`;
}

async function fetchFile(meta: FileMeta): Promise<Response> {
  const token = await apiToken();
  const headers: Record<string, string> = token ? { Authorization: "Bearer " + token } : {};
  const ports = [
    meta.nodePort,
    ...store.dataNodes()
      .filter((node) => node.reachable && node.process.process === "running")
      .map((node) => node.process.api_port),
  ].filter((port, index, all) => port > 0 && all.indexOf(port) === index);
  let lastError = "No reachable node has this file.";
  for (const port of ports) {
    try {
      const response = await fetch(fileStreamUrl(port, meta.collection, meta.key), { headers });
      if (response.ok) return response;
      const detail = (await response.text()).trim();
      lastError = `Node ${port} returned ${response.status}${detail ? `: ${detail.slice(0, 240)}` : ""}`;
    } catch (error) {
      lastError = error instanceof Error ? error.message : String(error);
    }
  }
  throw new Error(lastError);
}

function blobAsBase64(blob: Blob): Promise<string> {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => {
      if (typeof reader.result !== "string") {
        reject(new Error("File encoding produced no data."));
        return;
      }
      const separator = reader.result.indexOf(",");
      if (separator < 0) {
        reject(new Error("File encoding produced an invalid data URL."));
        return;
      }
      resolve(reader.result.slice(separator + 1));
    };
    reader.onerror = () => reject(reader.error ?? new Error("File read failed."));
    reader.readAsDataURL(blob);
  });
}

async function saveFetchedFile(meta: FileMeta): Promise<string | null> {
  const response = await fetchFile(meta);
  const base64 = await blobAsBase64(await response.blob());
  return sidecar.saveAssetFile(meta.filename, base64, meta.size_bytes, meta.sha256);
}

function closeModal(): void {
  if (activeModal) {
    activeModal.remove();
    activeModal = null;
  }
  for (const url of activeObjectUrls) URL.revokeObjectURL(url);
  activeObjectUrls = [];
}

export function openFilePreview(meta: FileMeta): void {
  closeModal();

  const scrim = el("div", { class: "modal-scrim", role: "dialog", "aria-modal": "true" });
  const box = el("div", { class: "modal-box", style: "max-width: 90vw; max-height: 90vh; width: 800px;" });

  const header = el(
    "div",
    { class: "row row--between", style: "margin-bottom: var(--space-sm);" },
    el("div", { class: "row", style: "gap: var(--space-xs);" },
      el("span", { style: "font-size: 24px;", text: getFileIcon(meta.mime, meta.filename) }),
      el("strong", { style: "font: var(--text-body-strong);", text: meta.filename }),
    ),
    el("button", { class: "btn btn--sm btn--ghost", type: "button", "aria-label": "Close" }, icon(Icons.close, 14)),
  );

  const closeBtn = header.querySelector("button") as HTMLButtonElement;
  on(closeBtn, "click", closeModal);

  const metaSection = el(
    "div",
    { class: "stack", style: "gap: var(--space-xxs); margin-bottom: var(--space-md);" },
    el("div", { class: "row", style: "gap: var(--space-md); flex-wrap: wrap;" },
      el("span", { class: "muted", style: "font: var(--text-fine);" }, `Type: ${meta.mime}`),
      el("span", { class: "muted", style: "font: var(--text-fine);" }, `Size: ${bytes(meta.size_bytes)}`),
      el("span", { class: "muted", style: "font: var(--text-fine);" }, `Category: ${categoryLabel(getFileCategory(meta.mime, meta.filename))}`),
    ),
    el("div", { class: "row", style: "gap: var(--space-md); flex-wrap: wrap;" },
      el("span", { class: "muted", style: "font: var(--text-fine);" }, `SHA256: ${meta.sha256.slice(0, 16)}…`),
      el("span", { class: "muted", style: "font: var(--text-fine);" }, `Created: ${timestamp(meta.created_ms)}`),
      el("span", { class: "muted", style: "font: var(--text-fine);" }, `Collection: ${meta.collection}`),
    ),
  );

  const previewArea = el("div", { class: "stack", style: "min-height: 200px; max-height: 60vh; overflow: auto;" });

  // Build preview content based on file type
  if (isPreviewable(meta.mime)) {
    if (meta.mime === "application/pdf") {
      const loading = el("p", { class: "muted", text: "Loading PDF…" });
      previewArea.appendChild(loading);
      const embed = el("embed", {
        type: "application/pdf",
        style: "width: 100%; height: 60vh; border: none;",
      });
      void fetchFile(meta).then(async (res) => {
        const url = URL.createObjectURL(await res.blob());
        activeObjectUrls.push(url);
        embed.setAttribute("src", url);
        loading.remove();
        previewArea.appendChild(embed);
      }).catch((error: unknown) => {
        loading.textContent = `Could not load PDF: ${error instanceof Error ? error.message : String(error)}`;
      });
    } else if (meta.mime.startsWith("image/")) {
      const img = el("img", {
        alt: meta.filename,
        style: "max-width: 100%; max-height: 60vh; object-fit: contain;",
      });
      previewArea.appendChild(img);
      void fetchFile(meta).then(async (res) => {
        const url = URL.createObjectURL(await res.blob());
        activeObjectUrls.push(url);
        img.src = url;
      }).catch((error: unknown) => {
        img.remove();
        previewArea.appendChild(el("p", { class: "muted", text: `Could not load image: ${error instanceof Error ? error.message : String(error)}` }));
      });
    }
  } else if (isOfficeFile(meta.mime, meta.filename)) {
    previewArea.appendChild(
      el("div", { class: "stack", style: "align-items: center; padding: var(--space-xl);" },
        el("span", { style: "font-size: 48px;", text: getFileIcon(meta.mime, meta.filename) }),
        el("p", { class: "muted", text: "Office documents cannot be previewed in-app." }),
        el("p", { class: "muted", style: "font: var(--text-fine);", text: "Use \"Open in System App\" to view with your default application." }),
      ),
    );
  } else if (isTextFile(meta.mime, meta.filename)) {
    const textArea = el("pre", { class: "explorer__doc", style: "white-space: pre-wrap; max-height: 60vh; overflow: auto;" });
    previewArea.appendChild(textArea);
    // Fetch text content
    void (async () => {
      try {
        const res = await fetchFile(meta);
        const text = await res.text();
        textArea.textContent = text;
      } catch {
        textArea.textContent = "Could not load file content.";
      }
    })();
  } else {
    previewArea.appendChild(
      el("div", { class: "stack", style: "align-items: center; padding: var(--space-xl);" },
        el("span", { style: "font-size: 48px;", text: getFileIcon(meta.mime, meta.filename) }),
        el("p", { class: "muted", text: "Binary file — no in-app preview available." }),
        el("p", { class: "muted", style: "font: var(--text-fine);", text: "Use \"Open in System App\" or \"Save As...\" to access this file." }),
      ),
    );
  }

  const actions = el("div", { class: "row", style: "gap: var(--space-xs); margin-top: var(--space-md);" });

  // Open in System App button
  const openBtn = el("button", { class: "btn btn--sm btn--primary", type: "button" }, icon(Icons.plug, 12), " Open in System App");
  on(openBtn, "click", async () => {
    try {
      const path = await saveFetchedFile(meta);
      if (!path) return;
      await sidecar.openSavedAsset(path, false);
      store.toast("success", "File opened", `Opened ${meta.filename} with your default app.`);
    } catch (err) {
      store.toast("error", "Could not open file", String(err));
    }
  });
  actions.appendChild(openBtn);

  // Save As button
  const saveBtn = el("button", { class: "btn btn--sm btn--ghost", type: "button" }, icon(Icons.copy, 12), " Save As...");
  on(saveBtn, "click", async () => {
    try {
      const path = await saveFetchedFile(meta);
      if (path) store.toast("success", "File saved", `Saved ${meta.filename} to ${path}.`);
    } catch (err) {
      store.toast("error", "Could not save file", String(err));
    }
  });
  actions.appendChild(saveBtn);

  box.appendChild(header);
  box.appendChild(metaSection);
  box.appendChild(previewArea);
  box.appendChild(actions);

  scrim.appendChild(box);

  on(scrim, "click", (e) => {
    if (e.target === scrim) closeModal();
  });

  const onKey = (e: KeyboardEvent) => {
    if (e.key === "Escape") {
      e.preventDefault();
      closeModal();
    }
  };
  window.addEventListener("keydown", onKey);

  document.body.appendChild(scrim);
  activeModal = scrim;

  // Clean up key listener when modal closes
  const observer = new MutationObserver(() => {
    if (!document.body.contains(scrim)) {
      window.removeEventListener("keydown", onKey);
      observer.disconnect();
    }
  });
  observer.observe(document.body, { childList: true });
}

export function closeFilePreview(): void {
  closeModal();
}
