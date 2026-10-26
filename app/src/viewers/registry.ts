import { ViewerComponent } from './types';
import { PdfViewer } from './pdf-viewer';
import { ImageViewer } from './image-viewer';
import { TextViewer } from './text-viewer';
import { OfficeHandler } from './office-handler';
import { GenericHandler } from './generic-handler';

/**
 * Registry mapping MIME types (or prefixes) to viewer constructors.
 * For simplicity we store exact MIME strings; for prefix matches (e.g., image/*)
 * we fall back to a generic handler.
 */
const registry: { [mime: string]: new () => ViewerComponent } = {
  // PDF
  'application/pdf': PdfViewer,
  // Images – common types
  'image/png': ImageViewer,
  'image/jpeg': ImageViewer,
  'image/jpg': ImageViewer,
  'image/gif': ImageViewer,
  'image/webp': ImageViewer,
  'image/svg+xml': ImageViewer,
  // Text / code
  'text/plain': TextViewer,
  'text/markdown': TextViewer,
  'text/csv': TextViewer,
  'application/json': TextViewer,
  'application/xml': TextViewer,
  // Office types – no preview, just handler
  'application/vnd.openxmlformats-officedocument.wordprocessingml.document': OfficeHandler,
  'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet': OfficeHandler,
  'application/vnd.openxmlformats-officedocument.presentationml.presentation': OfficeHandler,
  // Fallback for unknown binary types
  'application/octet-stream': GenericHandler,
};

/**
 * Return a viewer instance for the given MIME type.
 * If no exact match is found, we try prefix matching (e.g., image/*).
 * Finally we fall back to the generic handler.
 */
export function createViewer(mime: string): ViewerComponent {
  const ctor = registry[mime] ||
    // Prefix handling – e.g., image/png matches image/*
    Object.entries(registry).find(([key]) => {
      if (key.endsWith('/*')) {
        const prefix = key.slice(0, -2);
        return mime.startsWith(prefix);
      }
      return false;
    })?.[1] ||
    // Fallback generic handler
    GenericHandler;
  return new ctor();
}

/**
 * Optional helper to retrieve the constructor without instantiating.
 */
export function getViewerForMime(mime: string): ViewerComponent | null {
  const ctor = registry[mime] ?? null;
  return ctor ? new ctor() : null;
}
