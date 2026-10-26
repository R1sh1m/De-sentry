import { el, on, clear } from '../util/dom.js';
import { FileViewer, FileData } from './types';

/** Fallback handler for unknown binary files. */
export class GenericHandler implements FileViewer {
  private container: HTMLElement | null = null;

  async render(container: HTMLElement, file: FileData): Promise<void> {
    this.container = container;
    clear(container);

    const card = el('div', { class: 'generic-info-card' });
    card.appendChild(el('h3', { text: file.filename }));
    card.appendChild(el('div', { class: 'mono', text: file.mime }));
    card.appendChild(el('div', { text: `Size: ${this.formatSize(file.size_bytes)}` }));
    card.appendChild(
      el('p', { text: 'No preview available for this file type.' }),
    );

    const downloadBtn = el('button', { class: 'btn btn--primary', text: 'Download' });
    on(downloadBtn, 'click', () => {
      const a = document.createElement('a');
      a.href = `data:${file.mime};base64,${file.base64 ?? ''}`;
      a.download = file.filename;
      a.click();
    });
    card.appendChild(downloadBtn);

    container.appendChild(card);
  }

  private formatSize(bytes: number): string {
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
    return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  }

  destroy(): void {
    if (this.container) {
      clear(this.container);
      this.container = null;
    }
  }
}
