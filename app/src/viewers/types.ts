/**
 * Base interface for all file viewers.
 * Each viewer is responsible for rendering a specific file type into a container.
 */
export interface FileViewer {
  /** Render the viewer into the container */
  render(container: HTMLElement, file: FileData): Promise<void>;
  /** Clean up resources */
  destroy(): void;
  /** Optional: handle resize */
  resize?(width: number, height: number): void;
}

/**
 * File metadata and content passed to viewers.
 * The base64 field may be omitted for streaming scenarios.
 */
export interface FileData {
  filename: string;
  mime: string;
  size_bytes: number;
  base64?: string;  // May be omitted for streaming
  sha256: string;
  created_ms: number;
  collection: string;
  key: string;
}

/**
 * Viewer component factory signature.
 * Returns a viewer instance for the given MIME type.
 */
export interface ViewerComponent {
  render(container: HTMLElement, file: FileData): Promise<void>;
  destroy(): void;
  resize?(width: number, height: number): void;
}