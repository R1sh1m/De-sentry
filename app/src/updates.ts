/** Read the public GitHub release instead of assuming every release has a
 * signed Tauri updater feed. macOS DMGs are unsigned and are distributed as
 * manual downloads, so they do not appear in Tauri's latest.json manifest. */

export interface LatestRelease {
  version: string;
  url: string;
}

interface GitHubReleaseResponse {
  tag_name?: unknown;
  html_url?: unknown;
  draft?: unknown;
  prerelease?: unknown;
}

const RELEASE_API = "https://api.github.com/repos/R1sh1m/De-sentry/releases/latest";
const RELEASE_PAGE_PREFIX = "https://github.com/R1sh1m/De-sentry/releases/";

export async function fetchLatestRelease(timeoutMs = 8_000): Promise<LatestRelease> {
  if (typeof navigator !== "undefined" && navigator.onLine === false) {
    throw new Error("Offline (local mesh mode)");
  }
  const controller = new AbortController();
  const timeout = window.setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(RELEASE_API, { signal: controller.signal, cache: "no-store" });
    if (!response.ok) throw new Error(`GitHub returned HTTP ${response.status}`);

    const release = (await response.json()) as GitHubReleaseResponse;
    if (
      typeof release.tag_name !== "string" ||
      typeof release.html_url !== "string" ||
      release.draft === true ||
      release.prerelease === true ||
      !release.html_url.startsWith(RELEASE_PAGE_PREFIX)
    ) {
      throw new Error("GitHub returned an invalid release record");
    }

    return { version: release.tag_name.replace(/^[vV]/, ""), url: release.html_url };
  } finally {
    window.clearTimeout(timeout);
  }
}

/** Returns positive when candidate is newer, zero when equal, and negative
 * when it is older. Supports standard major.minor.patch prerelease versions. */
export function compareVersions(candidate: string, current: string): number {
  const parse = (value: string) => {
    const cleaned = (value || "").trim().replace(/^[vV]/, "");
    const match = /^(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$/.exec(cleaned);
    if (match === null) {
      return { core: [0, 0, 0], prerelease: null };
    }
    return {
      core: [Number(match[1] || 0), Number(match[2] || 0), Number(match[3] || 0)],
      prerelease: match[4] ?? null,
    };
  };

  const a = parse(candidate);
  const b = parse(current);
  for (let index = 0; index < a.core.length; index += 1) {
    if (a.core[index] !== b.core[index]) return a.core[index] - b.core[index];
  }
  if (a.prerelease === b.prerelease) return 0;
  if (a.prerelease === null) return 1;
  if (b.prerelease === null) return -1;
  return a.prerelease.localeCompare(b.prerelease, undefined, { numeric: true });
}
