/**
 * File type utilities: icons, categories, MIME detection, formatting.
 *
 * Used by the file browser to classify and display stored documents.
 * Zero dependencies — pure functions, no side effects.
 */

// -- file type icons (emoji fallback, rendered as text) -----------------------

/** Map of MIME type prefixes to emoji icons. */
const MIME_ICONS: [string, string][] = [
  ["application/pdf", "📄"],
  ["application/msword", "📝"],
  ["application/vnd.openxmlformats-officedocument.wordprocessingml.document", "📝"],
  ["application/vnd.ms-excel", "📊"],
  ["application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "📊"],
  ["application/vnd.ms-powerpoint", "📽️"],
  ["application/vnd.openxmlformats-officedocument.presentationml.presentation", "📽️"],
  ["image/", "🖼️"],
  ["video/", "🎬"],
  ["audio/", "🎵"],
  ["text/csv", "📊"],
  ["text/plain", "📃"],
  ["text/html", "🌐"],
  ["application/json", "📋"],
  ["application/xml", "📋"],
  ["application/zip", "📦"],
  ["application/gzip", "📦"],
  ["application/x-tar", "📦"],
  ["application/octet-stream", "📁"],
];

/** Map of file extensions to emoji icons. */
const EXT_ICONS: [string, string][] = [
  [".pdf", "📄"],
  [".doc", "📝"],
  [".docx", "📝"],
  [".xls", "📊"],
  [".xlsx", "📊"],
  [".ppt", "📽️"],
  [".pptx", "📽️"],
  [".jpg", "🖼️"],
  [".jpeg", "🖼️"],
  [".png", "🖼️"],
  [".gif", "🖼️"],
  [".svg", "🖼️"],
  [".webp", "🖼️"],
  [".bmp", "🖼️"],
  [".ico", "🖼️"],
  [".mp4", "🎬"],
  [".webm", "🎬"],
  [".avi", "🎬"],
  [".mp3", "🎵"],
  [".wav", "🎵"],
  [".csv", "📊"],
  [".txt", "📃"],
  [".html", "🌐"],
  [".htm", "🌐"],
  [".json", "📋"],
  [".xml", "📋"],
  [".zip", "📦"],
  [".gz", "📦"],
  [".tar", "📦"],
  [".7z", "📦"],
  [".rar", "📦"],
];

// -- file categories ----------------------------------------------------------

export type FileCategory = "pdf" | "document" | "spreadsheet" | "presentation" | "image" | "video" | "audio" | "archive" | "text" | "code" | "other";

/** Map of MIME type prefixes to categories. */
const MIME_CATEGORIES: [string, FileCategory][] = [
  ["application/pdf", "pdf"],
  ["application/msword", "document"],
  ["application/vnd.openxmlformats-officedocument.wordprocessingml.document", "document"],
  ["application/vnd.ms-excel", "spreadsheet"],
  ["application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "spreadsheet"],
  ["application/vnd.ms-powerpoint", "presentation"],
  ["application/vnd.openxmlformats-officedocument.presentationml.presentation", "presentation"],
  ["image/", "image"],
  ["video/", "video"],
  ["audio/", "audio"],
  ["application/zip", "archive"],
  ["application/gzip", "archive"],
  ["application/x-tar", "archive"],
  ["application/x-7z-compressed", "archive"],
  ["application/x-rar-compressed", "archive"],
  ["text/csv", "spreadsheet"],
  ["text/plain", "text"],
  ["text/html", "code"],
  ["text/xml", "code"],
  ["application/json", "code"],
  ["application/xml", "code"],
];

/** Map of file extensions to categories. */
const EXT_CATEGORIES: [string, FileCategory][] = [
  [".pdf", "pdf"],
  [".doc", "document"],
  [".docx", "document"],
  [".xls", "spreadsheet"],
  [".xlsx", "spreadsheet"],
  [".ppt", "presentation"],
  [".pptx", "presentation"],
  [".jpg", "image"],
  [".jpeg", "image"],
  [".png", "image"],
  [".gif", "image"],
  [".svg", "image"],
  [".webp", "image"],
  [".bmp", "image"],
  [".ico", "image"],
  [".mp4", "video"],
  [".webm", "video"],
  [".avi", "video"],
  [".mp3", "audio"],
  [".wav", "audio"],
  [".csv", "spreadsheet"],
  [".txt", "text"],
  [".html", "code"],
  [".htm", "code"],
  [".json", "code"],
  [".xml", "code"],
  [".zip", "archive"],
  [".gz", "archive"],
  [".tar", "archive"],
  [".7z", "archive"],
  [".rar", "archive"],
];

// -- public API ---------------------------------------------------------------

/**
 * Returns the emoji icon for a file based on its MIME type or filename.
 * Falls back to a generic folder icon.
 */
export function getFileIcon(mime: string | undefined | null, filename: string = ""): string {
  // Try MIME first
  if (mime) {
    for (const [prefix, icon] of MIME_ICONS) {
      if (mime.startsWith(prefix)) return icon;
    }
  }
  // Try extension
  if (filename) {
    const ext = filename.slice(filename.lastIndexOf(".")).toLowerCase();
    for (const [extKey, icon] of EXT_ICONS) {
      if (ext === extKey) return icon;
    }
  }
  return "📁";
}

/**
 * Returns the file category based on MIME type or filename.
 */
export function getFileCategory(mime: string | undefined | null, filename: string = ""): FileCategory {
  // Try MIME first
  if (mime) {
    for (const [prefix, cat] of MIME_CATEGORIES) {
      if (mime.startsWith(prefix)) return cat;
    }
  }
  // Try extension
  if (filename) {
    const ext = filename.slice(filename.lastIndexOf(".")).toLowerCase();
    for (const [extKey, cat] of EXT_CATEGORIES) {
      if (ext === extKey) return cat;
    }
  }
  return "other";
}

/**
 * Returns a human-readable label for a file category.
 */
export function categoryLabel(cat: FileCategory): string {
  const labels: Record<FileCategory, string> = {
    pdf: "PDF",
    document: "Document",
    spreadsheet: "Spreadsheet",
    presentation: "Presentation",
    image: "Image",
    video: "Video",
    audio: "Audio",
    archive: "Archive",
    text: "Text",
    code: "Code",
    other: "Other",
  };
  return labels[cat];
}

/**
 * Returns the file extension from a filename.
 */
export function getExtension(filename: string): string {
  const dot = filename.lastIndexOf(".");
  return dot >= 0 ? filename.slice(dot).toLowerCase() : "";
}

/**
 * Returns the filename without extension.
 */
export function getBaseName(filename: string): string {
  const dot = filename.lastIndexOf(".");
  return dot >= 0 ? filename.slice(0, dot) : filename;
}

/**
 * Returns true if the file can be previewed inline (PDF, images).
 */
export function isPreviewable(mime: string | undefined | null): boolean {
  if (!mime) return false;
  return mime === "application/pdf" || mime.startsWith("image/");
}

/**
 * Returns true if the file is an Office document that needs a system app.
 */
export function isOfficeFile(mime: string | undefined | null, filename: string = ""): boolean {
  if (mime) {
    return (
      mime.includes("wordprocessingml") ||
      mime.includes("spreadsheetml") ||
      mime.includes("presentationml") ||
      mime.includes("msword") ||
      mime.includes("ms-excel") ||
      mime.includes("ms-powerpoint")
    );
  }
  const ext = getExtension(filename);
  return [".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx"].includes(ext);
}

/**
 * Returns true if the file is a text-based type that can be displayed as text.
 */
export function isTextFile(mime: string | undefined | null, filename: string = ""): boolean {
  if (mime) {
    return mime.startsWith("text/") || mime === "application/json" || mime === "application/xml";
  }
  const ext = getExtension(filename);
  return [".txt", ".json", ".xml", ".csv", ".html", ".htm", ".md", ".log"].includes(ext);
}

/**
 * Returns the MIME type for a file extension.
 */
export function mimeForExtension(ext: string): string {
  const map: Record<string, string> = {
    ".pdf": "application/pdf",
    ".doc": "application/msword",
    ".docx": "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    ".xls": "application/vnd.ms-excel",
    ".xlsx": "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    ".ppt": "application/vnd.ms-powerpoint",
    ".pptx": "application/vnd.openxmlformats-officedocument.presentationml.presentation",
    ".jpg": "image/jpeg",
    ".jpeg": "image/jpeg",
    ".png": "image/png",
    ".gif": "image/gif",
    ".svg": "image/svg+xml",
    ".webp": "image/webp",
    ".bmp": "image/bmp",
    ".ico": "image/x-icon",
    ".mp4": "video/mp4",
    ".webm": "video/webm",
    ".mp3": "audio/mpeg",
    ".wav": "audio/wav",
    ".csv": "text/csv",
    ".txt": "text/plain",
    ".html": "text/html",
    ".htm": "text/html",
    ".json": "application/json",
    ".xml": "application/xml",
    ".zip": "application/zip",
    ".gz": "application/gzip",
    ".tar": "application/x-tar",
  };
  return map[ext.toLowerCase()] ?? "application/octet-stream";
}
