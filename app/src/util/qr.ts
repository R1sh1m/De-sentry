/**
 * A QR encoder, written from scratch.
 *
 * Two screens need one: pairing (a peer scans a payload of addresses and a
 * public key to join the mesh) and recovery-key export (the user photographs
 * or prints a key that exists nowhere else, because there is no escrow). Both
 * must work with no network and no npm install at run time, which rules out
 * pulling in a QR library -- so this is ISO/IEC 18004 byte mode, error
 * correction level M, versions 1 through 20, implemented directly.
 *
 * Scope is deliberately narrow. Byte mode encodes anything we produce (JSON,
 * base32 keys) without a mode-selection heuristic that could pick wrong. Level
 * M gives ~15% recovery, which is the right trade for a code read off a
 * screen or a clean print. Version 20 at level M holds 666 bytes -- far more
 * than either payload -- and stopping there keeps the alignment-pattern and
 * block tables small enough to read.
 */

export interface QrCode {
  /** Module count per side, including no quiet zone. */
  size: number;
  /** Row-major booleans; true means a dark module. */
  modules: boolean[][];
  version: number;
}

/** Thrown when the payload does not fit in a version-20 level-M symbol. */
export class QrTooLongError extends Error {
  constructor(byteLength: number) {
    super(`payload of ${byteLength} bytes exceeds the 666-byte capacity of a version-20 QR code`);
    this.name = "QrTooLongError";
  }
}

// -- static tables ----------------------------------------------------------
//
// Indexed by version - 1. Level M only.

/** Total codewords (data + error correction) in the symbol. */
const TOTAL_CODEWORDS = [
  26, 44, 70, 100, 134, 172, 196, 242, 292, 346, 404, 466, 532, 581, 655, 733, 815, 901, 991, 1085,
];

/** [ecCodewordsPerBlock, group1Blocks, group1DataCodewords, group2Blocks, group2DataCodewords] */
const EC_BLOCKS_M: ReadonlyArray<readonly [number, number, number, number, number]> = [
  [10, 1, 16, 0, 0],
  [16, 1, 28, 0, 0],
  [26, 1, 44, 0, 0],
  [18, 2, 32, 0, 0],
  [24, 2, 43, 0, 0],
  [16, 4, 27, 0, 0],
  [18, 4, 31, 0, 0],
  [22, 2, 38, 2, 39],
  [22, 3, 36, 2, 37],
  [26, 4, 43, 1, 44],
  [30, 1, 50, 4, 51],
  [22, 6, 36, 2, 37],
  [22, 8, 37, 1, 38],
  [24, 4, 40, 5, 41],
  [24, 5, 41, 5, 42],
  [28, 7, 45, 3, 46],
  [28, 10, 46, 1, 47],
  [26, 9, 43, 4, 44],
  [26, 3, 44, 11, 45],
  [26, 3, 41, 13, 42],
];

/** Row/column centres of the alignment patterns, per version. */
const ALIGNMENT_CENTERS: ReadonlyArray<readonly number[]> = [
  [],
  [6, 18],
  [6, 22],
  [6, 26],
  [6, 30],
  [6, 34],
  [6, 22, 38],
  [6, 24, 42],
  [6, 26, 46],
  [6, 28, 50],
  [6, 30, 54],
  [6, 32, 58],
  [6, 34, 62],
  [6, 26, 46, 66],
  [6, 26, 48, 70],
  [6, 26, 50, 74],
  [6, 30, 54, 78],
  [6, 30, 56, 82],
  [6, 30, 58, 86],
  [6, 34, 62, 90],
];

const MAX_VERSION = 20;

function dataCodewordCount(version: number): number {
  const [ec, g1, g1d, g2, g2d] = EC_BLOCKS_M[version - 1];
  void ec;
  return g1 * g1d + g2 * g2d;
}

// -- GF(256) arithmetic ------------------------------------------------------
//
// The Reed-Solomon field QR uses: primitive polynomial x^8+x^4+x^3+x^2+1
// (0x11d). Log/antilog tables are built once, so encoding is table lookups
// rather than repeated polynomial multiplication.

const GF_EXP = new Uint8Array(512);
const GF_LOG = new Uint8Array(256);
(() => {
  let x = 1;
  for (let i = 0; i < 255; i++) {
    GF_EXP[i] = x;
    GF_LOG[x] = i;
    x <<= 1;
    if (x & 0x100) x ^= 0x11d;
  }
  for (let i = 255; i < 512; i++) GF_EXP[i] = GF_EXP[i - 255];
})();

function gfMul(a: number, b: number): number {
  if (a === 0 || b === 0) return 0;
  return GF_EXP[GF_LOG[a] + GF_LOG[b]];
}

/** Generator polynomial of degree `degree`, coefficients high-order first. */
function generatorPolynomial(degree: number): Uint8Array {
  let poly = new Uint8Array([1]);
  for (let i = 0; i < degree; i++) {
    const next = new Uint8Array(poly.length + 1);
    for (let j = 0; j < poly.length; j++) {
      next[j] ^= poly[j];
      next[j + 1] ^= gfMul(poly[j], GF_EXP[i]);
    }
    poly = next;
  }
  return poly;
}

/** Reed-Solomon remainder of `data` for the given number of EC codewords. */
function errorCorrection(data: Uint8Array, ecCount: number): Uint8Array {
  const generator = generatorPolynomial(ecCount);
  const remainder = new Uint8Array(ecCount);
  for (const byte of data) {
    const factor = byte ^ remainder[0];
    remainder.copyWithin(0, 1);
    remainder[ecCount - 1] = 0;
    if (factor !== 0) {
      for (let i = 0; i < ecCount; i++) {
        remainder[i] ^= gfMul(generator[i + 1], factor);
      }
    }
  }
  return remainder;
}

// -- bit buffer --------------------------------------------------------------

class BitBuffer {
  private bits: number[] = [];

  push(value: number, length: number): void {
    for (let i = length - 1; i >= 0; i--) this.bits.push((value >>> i) & 1);
  }

  get length(): number {
    return this.bits.length;
  }

  padToBytes(): void {
    while (this.bits.length % 8 !== 0) this.bits.push(0);
  }

  toBytes(): Uint8Array {
    const out = new Uint8Array(this.bits.length / 8);
    for (let i = 0; i < this.bits.length; i++) {
      if (this.bits[i]) out[i >> 3] |= 0x80 >>> (i & 7);
    }
    return out;
  }
}

// -- encoding ----------------------------------------------------------------

function utf8(text: string): Uint8Array {
  return new TextEncoder().encode(text);
}

function chooseVersion(byteLength: number): number {
  // The character-count field is 8 bits below version 10 and 16 bits at or
  // above it, so capacity is not simply data codewords minus a constant.
  for (let version = 1; version <= MAX_VERSION; version++) {
    const countBits = version < 10 ? 8 : 16;
    const capacityBits = dataCodewordCount(version) * 8 - 4 - countBits;
    if (byteLength * 8 <= capacityBits) return version;
  }
  throw new QrTooLongError(byteLength);
}

function buildDataCodewords(payload: Uint8Array, version: number): Uint8Array {
  const buffer = new BitBuffer();
  buffer.push(0b0100, 4); // byte mode
  buffer.push(payload.length, version < 10 ? 8 : 16);
  for (const byte of payload) buffer.push(byte, 8);

  const capacityBits = dataCodewordCount(version) * 8;
  // Terminator: up to four zero bits, truncated if the symbol is nearly full.
  buffer.push(0, Math.min(4, capacityBits - buffer.length));
  buffer.padToBytes();

  const bytes = buffer.toBytes();
  const out = new Uint8Array(dataCodewordCount(version));
  out.set(bytes);
  // Alternating pad codewords, per the standard.
  for (let i = bytes.length; i < out.length; i++) {
    out[i] = (i - bytes.length) % 2 === 0 ? 0xec : 0x11;
  }
  return out;
}

/**
 * Splits data into blocks, computes each block's EC codewords, and interleaves
 * both -- the interleaving is what makes a burst of damage spread across
 * blocks rather than destroying one block entirely.
 */
function interleave(data: Uint8Array, version: number): Uint8Array {
  const [ecCount, g1, g1d, g2, g2d] = EC_BLOCKS_M[version - 1];
  const dataBlocks: Uint8Array[] = [];
  const ecBlocks: Uint8Array[] = [];

  let offset = 0;
  for (let i = 0; i < g1; i++) {
    const block = data.subarray(offset, offset + g1d);
    offset += g1d;
    dataBlocks.push(block);
    ecBlocks.push(errorCorrection(block, ecCount));
  }
  for (let i = 0; i < g2; i++) {
    const block = data.subarray(offset, offset + g2d);
    offset += g2d;
    dataBlocks.push(block);
    ecBlocks.push(errorCorrection(block, ecCount));
  }

  const out = new Uint8Array(TOTAL_CODEWORDS[version - 1]);
  let out_i = 0;
  const maxData = Math.max(g1d, g2d);
  for (let i = 0; i < maxData; i++) {
    for (const block of dataBlocks) {
      if (i < block.length) out[out_i++] = block[i];
    }
  }
  for (let i = 0; i < ecCount; i++) {
    for (const block of ecBlocks) out[out_i++] = block[i];
  }
  return out;
}

// -- matrix ------------------------------------------------------------------

type Matrix = { modules: (boolean | null)[][]; reserved: boolean[][]; size: number };

function newMatrix(size: number): Matrix {
  const modules: (boolean | null)[][] = [];
  const reserved: boolean[][] = [];
  for (let r = 0; r < size; r++) {
    modules.push(new Array<boolean | null>(size).fill(null));
    reserved.push(new Array<boolean>(size).fill(false));
  }
  return { modules, reserved, size };
}

function setFunction(m: Matrix, row: number, col: number, dark: boolean): void {
  if (row < 0 || col < 0 || row >= m.size || col >= m.size) return;
  m.modules[row][col] = dark;
  m.reserved[row][col] = true;
}

function placeFinder(m: Matrix, row: number, col: number): void {
  // The 7x7 finder plus its one-module separator. Drawing the separator here
  // rather than as a second pass is what keeps the reserved mask correct at
  // the symbol edges.
  for (let r = -1; r <= 7; r++) {
    for (let c = -1; c <= 7; c++) {
      const inFinder =
        r >= 0 && r <= 6 && c >= 0 && c <= 6 &&
        (r === 0 || r === 6 || c === 0 || c === 6 || (r >= 2 && r <= 4 && c >= 2 && c <= 4));
      setFunction(m, row + r, col + c, inFinder);
    }
  }
}

function placeAlignment(m: Matrix, version: number): void {
  const centers = ALIGNMENT_CENTERS[version - 1];
  for (const r of centers) {
    for (const c of centers) {
      // Skip the three corners occupied by finder patterns.
      const nearFinder =
        (r <= 8 && c <= 8) || (r <= 8 && c >= m.size - 9) || (r >= m.size - 9 && c <= 8);
      if (nearFinder) continue;
      for (let dr = -2; dr <= 2; dr++) {
        for (let dc = -2; dc <= 2; dc++) {
          const dark = Math.max(Math.abs(dr), Math.abs(dc)) !== 1;
          setFunction(m, r + dr, c + dc, dark);
        }
      }
    }
  }
}

function placeTiming(m: Matrix): void {
  for (let i = 8; i < m.size - 8; i++) {
    const dark = i % 2 === 0;
    setFunction(m, 6, i, dark);
    setFunction(m, i, 6, dark);
  }
}

/** BCH(15,5) format information, level M, XORed with the standard mask. */
function formatBits(mask: number): number {
  const data = (0b00 << 3) | mask; // 00 == level M
  let value = data << 10;
  for (let i = 14; i >= 10; i--) {
    if ((value >>> i) & 1) value ^= 0x537 << (i - 10);
  }
  return ((data << 10) | value) ^ 0x5412;
}

/** BCH(18,6) version information, present only for version >= 7. */
function versionBits(version: number): number {
  let value = version << 12;
  for (let i = 17; i >= 12; i--) {
    if ((value >>> i) & 1) value ^= 0x1f25 << (i - 12);
  }
  return (version << 12) | value;
}

function reserveFormatAreas(m: Matrix): void {
  for (let i = 0; i < 9; i++) {
    if (i !== 6) {
      setFunction(m, 8, i, false);
      setFunction(m, i, 8, false);
    }
  }
  setFunction(m, 8, 8, false);
  for (let i = 0; i < 8; i++) {
    setFunction(m, 8, m.size - 1 - i, false);
    setFunction(m, m.size - 1 - i, 8, false);
  }
  // The always-dark module below the top-left finder.
  setFunction(m, m.size - 8, 8, true);
}

function writeFormat(m: Matrix, mask: number): void {
  const bits = formatBits(mask);
  for (let i = 0; i < 15; i++) {
    const dark = ((bits >>> i) & 1) === 1;
    // Copy 1: around the top-left finder.
    if (i < 6) setFunction(m, 8, i, dark);
    else if (i < 8) setFunction(m, 8, i + 1, dark);
    else if (i === 8) setFunction(m, 7, 8, dark);
    else setFunction(m, 14 - i, 8, dark);
    // Copy 2: seven modules up the left of the bottom-left finder, then eight
    // along the top-right. The split is at seven, not eight -- module
    // (size-8, 8) is the always-dark module and carries no format bit.
    if (i < 7) setFunction(m, m.size - 1 - i, 8, dark);
    else setFunction(m, 8, m.size - 15 + i, dark);
  }
  setFunction(m, m.size - 8, 8, true);
}

function writeVersion(m: Matrix, version: number): void {
  if (version < 7) return;
  const bits = versionBits(version);
  for (let i = 0; i < 18; i++) {
    const dark = ((bits >>> i) & 1) === 1;
    const a = Math.floor(i / 3);
    const b = (i % 3) + m.size - 11;
    setFunction(m, a, b, dark);
    setFunction(m, b, a, dark);
  }
}

function maskBit(mask: number, row: number, col: number): boolean {
  switch (mask) {
    case 0: return (row + col) % 2 === 0;
    case 1: return row % 2 === 0;
    case 2: return col % 3 === 0;
    case 3: return (row + col) % 3 === 0;
    case 4: return (Math.floor(row / 2) + Math.floor(col / 3)) % 2 === 0;
    case 5: return ((row * col) % 2) + ((row * col) % 3) === 0;
    case 6: return (((row * col) % 2) + ((row * col) % 3)) % 2 === 0;
    default: return (((row + col) % 2) + ((row * col) % 3)) % 2 === 0;
  }
}

/** Zig-zag placement of the interleaved codewords, bottom-right upward. */
function placeData(m: Matrix, codewords: Uint8Array): void {
  let bit = 0;
  let upward = true;
  for (let right = m.size - 1; right >= 1; right -= 2) {
    if (right === 6) right = 5; // the vertical timing column is skipped entirely
    for (let step = 0; step < m.size; step++) {
      const row = upward ? m.size - 1 - step : step;
      for (const col of [right, right - 1]) {
        if (m.reserved[row][col]) continue;
        const byte = bit >> 3;
        const dark = byte < codewords.length && ((codewords[byte] >>> (7 - (bit & 7))) & 1) === 1;
        m.modules[row][col] = dark;
        bit += 1;
      }
    }
    upward = !upward;
  }
}

// -- mask selection ----------------------------------------------------------

function applyMask(m: Matrix, mask: number): boolean[][] {
  const out: boolean[][] = [];
  for (let r = 0; r < m.size; r++) {
    const row: boolean[] = [];
    for (let c = 0; c < m.size; c++) {
      const value = m.modules[r][c] === true;
      row.push(m.reserved[r][c] ? value : value !== maskBit(mask, r, c));
    }
    out.push(row);
  }
  return out;
}

/** The four penalty rules from the standard, summed. Lower is better. */
function penalty(grid: boolean[][]): number {
  const n = grid.length;
  let score = 0;

  // Rule 1: runs of five or more same-coloured modules in a row or column.
  const runScore = (line: boolean[]): number => {
    let total = 0;
    let run = 1;
    for (let i = 1; i < line.length; i++) {
      if (line[i] === line[i - 1]) {
        run += 1;
      } else {
        if (run >= 5) total += run - 2;
        run = 1;
      }
    }
    if (run >= 5) total += run - 2;
    return total;
  };
  for (let i = 0; i < n; i++) {
    score += runScore(grid[i]);
    score += runScore(grid.map((row) => row[i]));
  }

  // Rule 2: 2x2 blocks of a single colour.
  for (let r = 0; r < n - 1; r++) {
    for (let c = 0; c < n - 1; c++) {
      const v = grid[r][c];
      if (grid[r][c + 1] === v && grid[r + 1][c] === v && grid[r + 1][c + 1] === v) score += 3;
    }
  }

  // Rule 3: the finder-like 1:1:3:1:1 pattern with four light modules either
  // side -- the pattern a scanner could mistake for a finder.
  const target = [true, false, true, true, true, false, true];
  const light4 = [false, false, false, false];
  const matchesAt = (line: boolean[], at: number, pattern: boolean[]): boolean => {
    if (at < 0 || at + pattern.length > line.length) return false;
    for (let i = 0; i < pattern.length; i++) if (line[at + i] !== pattern[i]) return false;
    return true;
  };
  const patternScore = (line: boolean[]): number => {
    let total = 0;
    for (let i = 0; i + 7 <= line.length; i++) {
      if (!matchesAt(line, i, target)) continue;
      if (matchesAt(line, i - 4, light4) || matchesAt(line, i + 7, light4)) total += 40;
    }
    return total;
  };
  for (let i = 0; i < n; i++) {
    score += patternScore(grid[i]);
    score += patternScore(grid.map((row) => row[i]));
  }

  // Rule 4: deviation from a 50/50 dark ratio.
  let dark = 0;
  for (const row of grid) for (const v of row) if (v) dark += 1;
  const ratio = (dark * 100) / (n * n);
  score += Math.floor(Math.abs(ratio - 50) / 5) * 10;
  return score;
}

// -- public API --------------------------------------------------------------

/** Encodes `text` as a level-M QR code, choosing the smallest version that fits. */
export function encodeQr(text: string): QrCode {
  const payload = utf8(text);
  const version = chooseVersion(payload.length);
  const size = version * 4 + 17;

  const m = newMatrix(size);
  placeFinder(m, 0, 0);
  placeFinder(m, 0, size - 7);
  placeFinder(m, size - 7, 0);
  placeAlignment(m, version);
  placeTiming(m);
  reserveFormatAreas(m);
  writeVersion(m, version);

  placeData(m, interleave(buildDataCodewords(payload, version), version));

  let best: boolean[][] | null = null;
  let bestScore = Infinity;
  let bestMask = 0;
  for (let mask = 0; mask < 8; mask++) {
    const candidate = applyMask(m, mask);
    // The format bits differ per mask and are themselves scored, so they are
    // written into the candidate before it is judged.
    const judged = withFormat(candidate, m, mask);
    const score = penalty(judged);
    if (score < bestScore) {
      bestScore = score;
      best = judged;
      bestMask = mask;
    }
  }
  void bestMask;
  return { size, modules: best as boolean[][], version };
}

function withFormat(grid: boolean[][], m: Matrix, mask: number): boolean[][] {
  const copy = grid.map((row) => row.slice());
  const scratch: Matrix = { modules: copy as (boolean | null)[][], reserved: m.reserved, size: m.size };
  writeFormat(scratch, mask);
  return copy;
}

export interface QrSvgOptions {
  /** Module size in px. */
  scale?: number;
  /** Quiet-zone width in modules. Four is the standard minimum. */
  quietZone?: number;
  /** Dark module colour. Defaults to currentColor so the code follows the theme. */
  color?: string;
  background?: string;
  title?: string;
}

/**
 * Renders a code as an SVG element.
 *
 * One `<path>` of rectangles rather than a `<rect>` per module: a version-20
 * symbol has 9,409 modules, and 9,409 DOM nodes in a dialog is a visible
 * hitch on a slow machine.
 */
export function qrToSvg(code: QrCode, options: QrSvgOptions = {}): SVGElement {
  const scale = options.scale ?? 4;
  const quiet = options.quietZone ?? 4;
  const dimension = (code.size + quiet * 2) * scale;

  let d = "";
  for (let r = 0; r < code.size; r++) {
    for (let c = 0; c < code.size; c++) {
      if (!code.modules[r][c]) continue;
      const x = (c + quiet) * scale;
      const y = (r + quiet) * scale;
      d += `M${x} ${y}h${scale}v${scale}h-${scale}z`;
    }
  }

  const ns = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(ns, "svg");
  svg.setAttribute("width", String(dimension));
  svg.setAttribute("height", String(dimension));
  svg.setAttribute("viewBox", `0 0 ${dimension} ${dimension}`);
  svg.setAttribute("role", "img");
  svg.setAttribute("shape-rendering", "crispEdges");
  if (options.title) {
    const title = document.createElementNS(ns, "title");
    title.textContent = options.title;
    svg.appendChild(title);
    svg.setAttribute("aria-label", options.title);
  } else {
    svg.setAttribute("aria-hidden", "true");
  }

  // The quiet zone must be light, not transparent: a code on a dark control
  // room background with no light margin does not scan.
  const bg = document.createElementNS(ns, "rect");
  bg.setAttribute("width", String(dimension));
  bg.setAttribute("height", String(dimension));
  bg.setAttribute("fill", options.background ?? "#ffffff");
  svg.appendChild(bg);

  const path = document.createElementNS(ns, "path");
  path.setAttribute("d", d);
  path.setAttribute("fill", options.color ?? "#000000");
  svg.appendChild(path);
  return svg;
}

/** Convenience: text straight to an SVG element. */
export function qrSvg(text: string, options?: QrSvgOptions): SVGElement {
  return qrToSvg(encodeQr(text), options);
}
