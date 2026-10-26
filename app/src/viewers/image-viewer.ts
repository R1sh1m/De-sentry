import { el, icon, Icons, on, clear } from '../util/dom.js';
import { FileViewer, FileData } from './types';

/** Image viewer with zoom and pan controls. */
export class ImageViewer implements FileViewer {
  private img: HTMLImageElement | null = null;
  private toolbar: HTMLElement | null = null;
  private container: HTMLElement | null = null;
  private viewport: HTMLElement | null = null;
  private zoom = 1;
  private panX = 0;
  private panY = 0;
  private isPanning = false;
  private lastX = 0;
  private lastY = 0;

  async render(container: HTMLElement, file: FileData): Promise<void> {
    this.container = container;
    clear(container);

    // Toolbar
    this.toolbar = this.createToolbar(file);
    container.appendChild(this.toolbar);

    // Viewport for panning/zooming
    this.viewport = el('div', { class: 'image-viewport' });
    container.appendChild(this.viewport);

    // Build data URL
    const dataUrl = file.base64 ? `data:${file.mime};base64,${file.base64}` : '';

    this.img = el('img', {
      src: dataUrl,
      alt: file.filename,
      class: 'image-viewer-img',
      style: `transform: translate(${this.panX}px, ${this.panY}px) scale(${this.zoom});`,
    }) as HTMLImageElement;

    // Load / error handling
    this.img.addEventListener('load', () => {
      // Center image initially
      this.fitToScreen();
    });
    this.img.addEventListener('error', () => {
      clear(this.viewport!);
      const err = el('div', { class: 'viewer-error', text: 'Failed to load image.' });
      this.viewport!.appendChild(err);
    });

    // Pan handling
    this.img.addEventListener('mousedown', (e) => {
      if (this.zoom <= 1) return; // only pan when zoomed
      this.isPanning = true;
      this.lastX = e.clientX;
      this.lastY = e.clientY;
      this.img!.style.cursor = 'grabbing';
    });
    window.addEventListener('mousemove', this.onMouseMove);
    window.addEventListener('mouseup', this.onMouseUp);

    this.viewport.appendChild(this.img);
  }

  private onMouseMove = (e: MouseEvent) => {
    if (!this.isPanning || !this.img) return;
    const dx = e.clientX - this.lastX;
    const dy = e.clientY - this.lastY;
    this.panX += dx;
    this.panY += dy;
    this.lastX = e.clientX;
    this.lastY = e.clientY;
    this.updateTransform();
  };

  private onMouseUp = () => {
    this.isPanning = false;
    if (this.img) this.img.style.cursor = 'grab';
  };

  private updateTransform() {
    if (this.img) {
      this.img.style.transform = `translate(${this.panX}px, ${this.panY}px) scale(${this.zoom})`;
    }
  }

  private createToolbar(file: FileData): HTMLElement {
    const toolbar = el('div', { class: 'viewer-toolbar' });

    // Download
    const downloadBtn = el('button', { class: 'btn', title: 'Download image', text: 'Download' });
    on(downloadBtn, 'click', () => {
      const a = document.createElement('a');
      a.href = `data:${file.mime};base64,${file.base64 ?? ''}`;
      a.download = file.filename;
      a.click();
    });

    // Open in new tab
    const openBtn = el('button', { class: 'btn', title: 'Open in new tab', text: 'Open' });
    on(openBtn, 'click', () => {
      const url = `data:${file.mime};base64,${file.base64 ?? ''}`;
      window.open(url, '_blank');
    });

    // Copy data URL
    const copyBtn = el('button', { class: 'btn', title: 'Copy data URL' },
      icon(Icons.copy ?? '', 16),
    );
    on(copyBtn, 'click', async () => {
      try {
        await navigator.clipboard.writeText(`data:${file.mime};base64,${file.base64 ?? ''}`);
      } catch (e) { /* ignore */ }
    });

    // Zoom controls
    const zoomIn = el('button', { class: 'btn', title: 'Zoom In' }, icon(Icons.zoomIn ?? '', 16));
    const zoomOut = el('button', { class: 'btn', title: 'Zoom Out' }, icon(Icons.zoomOut ?? '', 16));
    const zoomReset = el('button', { class: 'btn', title: 'Reset Zoom' }, icon(Icons.zoomReset ?? '', 16));
    const fitBtn = el('button', { class: 'btn', title: 'Fit to screen' }, icon(Icons.zoomReset ?? '', 16));
    const actualBtn = el('button', { class: 'btn', title: 'Actual size' }, icon(Icons.search ?? '', 16));

    on(zoomIn, 'click', () => this.setZoom(this.zoom + 0.25));
    on(zoomOut, 'click', () => this.setZoom(Math.max(0.25, this.zoom - 0.25)));
    on(zoomReset, 'click', () => this.setZoom(1));
    on(fitBtn, 'click', () => this.fitToScreen());
    on(actualBtn, 'click', () => { this.zoom = 1; this.panX = 0; this.panY = 0; this.updateTransform(); });

    toolbar.appendChild(downloadBtn);
    toolbar.appendChild(openBtn);
    toolbar.appendChild(copyBtn);
    toolbar.appendChild(zoomOut);
    toolbar.appendChild(zoomIn);
    toolbar.appendChild(zoomReset);
    toolbar.appendChild(fitBtn);
    toolbar.appendChild(actualBtn);

    return toolbar;
  }

  private setZoom(factor: number) {
    this.zoom = factor;
    this.updateTransform();
  }

  private fitToScreen() {
    if (!this.img || !this.viewport) return;
    const vw = this.viewport.clientWidth;
    const vh = this.viewport.clientHeight;
    const iw = this.img.naturalWidth || this.img.width;
    const ih = this.img.naturalHeight || this.img.height;
    const scaleX = vw / iw;
    const scaleY = vh / ih;
    this.zoom = Math.min(scaleX, scaleY, 1); // don't upscale
    this.panX = (vw - iw * this.zoom) / 2;
    this.panY = (vh - ih * this.zoom) / 2;
    this.updateTransform();
  }

  destroy(): void {
    window.removeEventListener('mousemove', this.onMouseMove);
    window.removeEventListener('mouseup', this.onMouseUp);
    if (this.img) {
      this.img.remove();
      this.img = null;
    }
    if (this.toolbar) {
      this.toolbar.remove();
      this.toolbar = null;
    }
    if (this.viewport) {
      this.viewport.remove();
      this.viewport = null;
    }
    if (this.container) {
      clear(this.container);
      this.container = null;
    }
  }
}
