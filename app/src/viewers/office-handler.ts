import { el, on, clear } from '../util/dom.js';
import { FileViewer, FileData } from './types';

/**
 * Handler for Office files (.docx, .xlsx, .pptx).
 * No in-app preview – shows file info and suggests opening in system app.
 */
export class OfficeHandler implements FileViewer {
  private container: HTMLElement | null = null;

  async render(container: HTMLElement, file: FileData): Promise<void> {
    this.container = container;
    clear(container);

    const infoCard = el('div', { class: 'office-info-card' });

    // Large file type icon
    const iconEl = this.getFileIcon(file.mime);
    infoCard.appendChild(iconEl);

    // Filename
    infoCard.appendChild(el('h3', { class: 'office-filename', text: file.filename }));

    // MIME type
    infoCard.appendChild(el('div', { class: 'office-mime mono', text: file.mime }));

    // Size
    const sizeLabel = this.formatSize(file.size_bytes);
    infoCard.appendChild(el('div', { class: 'office-size', text: `Size: ${sizeLabel}` }));

    // Message
    infoCard.appendChild(
      el('p', { class: 'office-message', text: 'This file type requires a system application to view.' }),
    );

    // Suggested app
    const suggested = this.getSuggestedApp(file.mime);
    infoCard.appendChild(
      el('p', { class: 'office-suggestion', text: `Suggested: ${suggested}` }),
    );

    // Buttons
    const actions = el('div', { class: 'office-actions' });

    // Open in system app
    const openBtn = el('button', { class: 'btn btn--primary', text: 'Open in System App' });
    on(openBtn, 'click', () => {
      // For Office files, we can't preview – just trigger download which OS may open
      const a = document.createElement('a');
      a.href = this.getDataUrl(file);
      a.download = file.filename;
      a.click();
    });
    actions.appendChild(openBtn);

    // Save As...
    const saveBtn = el('button', { class: 'btn', text: 'Save As...' });
    on(saveBtn, 'click', () => {
      const a = document.createElement('a');
      a.href = this.getDataUrl(file);
      a.download = file.filename;
      a.click();
    });
    actions.appendChild(saveBtn);

    infoCard.appendChild(actions);
    container.appendChild(infoCard);
  }

  private getFileIcon(mime: string): HTMLElement {
    // Simple large icon using SVG
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('width', '64');
    svg.setAttribute('height', '64');
    svg.setAttribute('viewBox', '0 0 64 64');
    svg.setAttribute('fill', 'none');
    svg.setAttribute('stroke', 'currentColor');
    svg.setAttribute('stroke-width', '2');
    svg.setAttribute('stroke-linecap', 'round');
    svg.setAttribute('stroke-linejoin', 'round');
    svg.setAttribute('class', 'office-file-icon');

    // Document shape
    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    path.setAttribute('d', 'M14 8h24l12 12v36H14z');
    svg.appendChild(path);

    // Fold corner
    const fold = document.createElementNS('http://www.w3.org/2000/svg', 'path');
    fold.setAttribute('d', 'M38 8v12h12');
    svg.appendChild(fold);

    // Type-specific mark
    const mark = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    mark.setAttribute('x', '32');
    mark.setAttribute('y', '44');
    mark.setAttribute('text-anchor', 'middle');
    mark.setAttribute('fill', 'currentColor');
    mark.setAttribute('font-size', '16');
    mark.setAttribute('font-family', 'monospace');
    mark.textContent = this.getTypeLabel(mime);
    svg.appendChild(mark);

    return svg as unknown as HTMLElement;
  }

  private getTypeLabel(mime: string): string {
    if (mime.includes('word')) return 'DOC';
    if (mime.includes('spreadsheet')) return 'XLS';
    if (mime.includes('presentation')) return 'PPT';
    return 'OFF';
  }

  private getSuggestedApp(mime: string): string {
    if (mime.includes('word')) return 'Microsoft Word / LibreOffice Writer';
    if (mime.includes('spreadsheet')) return 'Microsoft Excel / LibreOffice Calc';
    if (mime.includes('presentation')) return 'Microsoft PowerPoint / LibreOffice Impress';
    return 'System default application';
  }

  private formatSize(bytes: number): string {
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
    return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  }

  private getDataUrl(file: FileData): string {
    // Infer MIME for data URL
    return `data:${file.mime};base64,${file.base64 ?? ''}`;
  }

  destroy(): void {
    if (this.container) {
      clear(this.container);
      this.container = null;
    }
  }
}