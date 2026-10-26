import { el, icon, Icons, on, clear } from '../util/dom.js';
import { FileViewer, FileData } from './types';

/** Simple PDF viewer using an <embed> element with a data URL. */
export class PdfViewer implements FileViewer {
  private embed: HTMLEmbedElement | null = null;
  private toolbar: HTMLElement | null = null;
  private container: HTMLElement | null = null;
  private zoom = 1;

  async render(container: HTMLElement, file: FileData): Promise<void> {
    this.container = container;
    clear(container);

    // Toolbar
    this.toolbar = this.createToolbar(file);
    container.appendChild(this.toolbar);

    // Loading placeholder
    const loading = el('div', { class: 'viewer-loading', text: 'Loading PDF…' });
    container.appendChild(loading);

    // Build data URL – expect base64 content
    const dataUrl = file.base64
      ? `data:application/pdf;base64,${file.base64}`
      : '';

    this.embed = el('embed', {
      src: dataUrl,
      type: 'application/pdf',
      width: '100%',
      height: '100%',
      style: `transform: scale(${this.zoom}); transform-origin: 0 0;`,
    }) as HTMLEmbedElement;

    // Load / error handling
    const onLoad = () => {
      clear(loading);
    };
    const onError = () => {
      clear(loading);
      const err = el('div', { class: 'viewer-error', text: 'Failed to load PDF (corrupt or unsupported).' });
      container.appendChild(err);
    };
    this.embed.addEventListener('load', onLoad);
    this.embed.addEventListener('error', onError);

    container.appendChild(this.embed);
  }

  /** Build toolbar with common actions. */
  private createToolbar(file: FileData): HTMLElement {
    const toolbar = el('div', { class: 'viewer-toolbar' });

    // Download button
    const downloadBtn = el('button', { class: 'btn', title: 'Download PDF', text: 'Download' });
    on(downloadBtn, 'click', () => {
      const a = document.createElement('a');
      a.href = `data:application/pdf;base64,${file.base64 ?? ''}`;
      a.download = file.filename;
      a.click();
    });

    // Open in system / new tab
    const openBtn = el('button', { class: 'btn', title: 'Open in new tab', text: 'Open' });
    on(openBtn, 'click', () => {
      const url = `data:application/pdf;base64,${file.base64 ?? ''}`;
      window.open(url, '_blank');
    });

    // Copy link (data URL) to clipboard
    const copyBtn = el('button', { class: 'btn', title: 'Copy data URL' },
      icon(Icons.copy ?? '', 16),
    );
    on(copyBtn, 'click', async () => {
      try {
        await navigator.clipboard.writeText(`data:application/pdf;base64,${file.base64 ?? ''}`);
      } catch (e) {
        // ignore – clipboard may be unavailable
      }
    });

    // Zoom controls
    const zoomIn = el('button', { class: 'btn', title: 'Zoom In' }, icon(Icons.zoomIn ?? '', 16));
    const zoomOut = el('button', { class: 'btn', title: 'Zoom Out' }, icon(Icons.zoomOut ?? '', 16));
    const zoomReset = el('button', { class: 'btn', title: 'Reset Zoom' }, icon(Icons.zoomReset ?? '', 16));
    on(zoomIn, 'click', () => this.setZoom(this.zoom + 0.25));
    on(zoomOut, 'click', () => this.setZoom(Math.max(0.25, this.zoom - 0.25)));
    on(zoomReset, 'click', () => this.setZoom(1));

    toolbar.appendChild(downloadBtn);
    toolbar.appendChild(openBtn);
    toolbar.appendChild(copyBtn);
    toolbar.appendChild(zoomOut);
    toolbar.appendChild(zoomIn);
    toolbar.appendChild(zoomReset);

    return toolbar;
  }

  private setZoom(factor: number) {
    this.zoom = factor;
    if (this.embed) {
      this.embed.style.transform = `scale(${this.zoom})`;
    }
  }

  destroy(): void {
    if (this.embed) {
      this.embed.remove();
      this.embed = null;
    }
    if (this.toolbar) {
      this.toolbar.remove();
      this.toolbar = null;
    }
    if (this.container) {
      clear(this.container);
      this.container = null;
    }
  }
}
