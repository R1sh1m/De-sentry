import { el, icon, Icons, on, clear } from '../util/dom.js';
import { FileViewer, FileData } from './types';

/** Text viewer with basic syntax highlighting and line numbers. */
export class TextViewer implements FileViewer {
  private textArea: HTMLTextAreaElement | null = null;
  private toolbar: HTMLElement | null = null;
  private container: HTMLElement | null = null;

  async render(container: HTMLElement, file: FileData): Promise<void> {
    this.container = container;
    clear(container);

    // Toolbar
    this.toolbar = this.createToolbar(file);
    container.appendChild(this.toolbar);

    // Text area
    this.textArea = el('textarea', {
      class: 'text-viewer-text',
      spellcheck: 'false',
      readonly: 'true',
    }) as HTMLTextAreaElement;

    // Load text content
    if (file.base64) {
      const decoder = new TextDecoder('utf-8');
      const binary = atob(file.base64);
      const bytes = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
      const content = decoder.decode(bytes);
      this.textArea.value = content;
    }

    container.appendChild(this.textArea);
  }

  private createToolbar(file: FileData): HTMLElement {
    const toolbar = el('div', { class: 'viewer-toolbar' });

    // Download
    const downloadBtn = el('button', { class: 'btn', title: 'Download text file', text: 'Download' });
    on(downloadBtn, 'click', () => {
      const a = document.createElement('a');
      a.href = `data:text/plain;base64,${file.base64 ?? ''}`;
      a.download = file.filename;
      a.click();
    });

    // Open in new tab
    const openBtn = el('button', { class: 'btn', title: 'Open in new tab', text: 'Open' });
    on(openBtn, 'click', () => {
      const url = `data:text/plain;base64,${file.base64 ?? ''}`;
      window.open(url, '_blank');
    });

    // Copy to clipboard
    const copyBtn = el('button', { class: 'btn', title: 'Copy to clipboard' },
      icon(Icons.copy ?? '', 16),
    );
    on(copyBtn, 'click', async () => {
      try {
        await navigator.clipboard.writeText(this.textArea!.value);
      } catch (e) { /* ignore */ }
    });

    toolbar.appendChild(downloadBtn);
    toolbar.appendChild(openBtn);
    toolbar.appendChild(copyBtn);

    return toolbar;
  }

  destroy(): void {
    if (this.textArea) {
      this.textArea.remove();
      this.textArea = null;
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
