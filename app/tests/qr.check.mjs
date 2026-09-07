/**
 * Conformance checks for src/util/qr.ts.
 *
 * A QR code that renders but does not scan fails silently -- the user prints a
 * recovery key, throws away the only other copy, and finds out months later.
 * So the encoder is checked against values from the standard itself rather
 * than against its own output: the eight published level-M format strings
 * (ISO/IEC 18004 Annex C) and the published 18-bit version-information words.
 *
 * Run with:  node tests/qr.check.mjs   (Node 22.18+ strips types natively)
 */

import { encodeQr } from "../src/util/qr.ts";

// Known-good level-M format strings from ISO/IEC 18004 Annex C, masks 0..7.
const expectFormat = [
  "101010000010010","101000100100101","101111001111100","101101101001011",
  "100010111111001","100000011001110","100111110010111","100101010100000",
];
// Known-good version information for versions 7..10.
const expectVersion = {7:"000111110010010100",8:"001000010110111100",9:"001001101010011001",10:"001010010011010011"};

function readFormatCopy1(code) {
  const m = code.modules, n = code.size;
  // bit i placement, per writeFormat copy 1
  const bits = [];
  for (let i = 0; i < 15; i++) {
    let v;
    if (i < 6) v = m[8][i];
    else if (i < 8) v = m[8][i + 1];
    else if (i === 8) v = m[7][8];
    else v = m[14 - i][8];
    bits.push(v ? 1 : 0);
  }
  // bits[0] is LSB; the published string is MSB-first
  return bits.slice().reverse().join("");
}
function readFormatCopy2(code) {
  const m = code.modules, n = code.size;
  const bits = [];
  for (let i = 0; i < 15; i++) {
    const v = i < 7 ? m[n - 1 - i][8] : m[8][n - 15 + i];
    bits.push(v ? 1 : 0);
  }
  return bits.slice().reverse().join("");
}
function readVersion(code) {
  const m = code.modules, n = code.size;
  const bits = [];
  for (let i = 0; i < 18; i++) {
    const a = Math.floor(i / 3), b = (i % 3) + n - 11;
    bits.push(m[a][b] ? 1 : 0);
  }
  return bits.slice().reverse().join("");
}

let failures = 0;
function check(label, ok, extra="") { if (!ok) { failures++; console.log("FAIL", label, extra); } }

// 1. Format information must match one of the published strings, and both
//    copies must agree.
for (const text of ["hi", "x".repeat(300)]) {
  const code = encodeQr(text);
  const f1 = readFormatCopy1(code), f2 = readFormatCopy2(code);
  check(`format copies agree (${text.length}B)`, f1 === f2, `${f1} vs ${f2}`);
  check(`format is a valid level-M string (${text.length}B)`, expectFormat.includes(f1), f1);
}

// 2. Version information, versions 7..10.
for (const [v, want] of Object.entries(expectVersion)) {
  const n = Number(v);
  // pick a payload length that lands on exactly this version
  const cap = { 7:118, 8:145, 9:172, 10:203 }[n];
  const code = encodeQr("y".repeat(cap));
  check(`chose version ${v}`, code.version === n, `got ${code.version}`);
  if (code.version === n) check(`version bits v${v}`, readVersion(code) === want, readVersion(code));
}

// 3. Structural invariants.
const code = encodeQr(JSON.stringify({version:2,node_id:"a".repeat(64),bootstrap_peers:["192.168.1.20:7801","192.168.1.21:7801"]}));
const m = code.modules, n = code.size;
check("size formula", n === code.version * 4 + 17, `${n}`);
// finder patterns dark corners
for (const [r0,c0] of [[0,0],[0,n-7],[n-7,0]]) {
  check(`finder at ${r0},${c0}`, m[r0][c0] && m[r0+6][c0+6] && m[r0+3][c0+3] && !m[r0+1][c0+1]);
}
// timing patterns alternate
let timingOk = true;
for (let i = 8; i < n - 8; i++) { if (m[6][i] !== (i % 2 === 0) || m[i][6] !== (i % 2 === 0)) timingOk = false; }
check("timing patterns", timingOk);
// dark module
check("dark module", m[n - 8][8] === true);
// every module assigned
let unassigned = 0;
for (const row of m) for (const v of row) if (typeof v !== "boolean") unassigned++;
check("all modules assigned", unassigned === 0, `${unassigned} unassigned`);

// 4. Capacity boundary: 666 bytes fits at v20, 667 throws.
check("666 bytes fits", encodeQr("z".repeat(666)).version === 20);
let threw = false;
try { encodeQr("z".repeat(667)); } catch { threw = true; }
check("667 bytes rejected", threw);

console.log(failures === 0 ? "qr.ts: ALL CHECKS PASSED" : `qr.ts: ${failures} FAILURES`);
process.exit(failures === 0 ? 0 : 1);
