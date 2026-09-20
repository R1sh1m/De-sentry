//! Custom passphrases as an alternative to generated recovery keys.
//!
//! A generated recovery key is 256 bits from the OS RNG — maximal entropy but
//! awkward to remember. A custom passphrase is user-chosen text stretched with
//! PBKDF2-HMAC-SHA256 (210k iterations, 16-byte salt) into the same 32-byte
//! node key shape, so the rest of the plumbing (`unlock_secret` over stdin,
//! OS keychain entry, Crockford encoding) is unchanged.
//!
//! Envelope-ready rotation: for passphrase nodes a random 32-byte DEK is
//! generated once. The passphrase derives a KEK which wraps the DEK
//! (XOR stream from SHA-256(KEK || salt || counter) + HMAC integrity tag).
//! Rotation re-wraps the same DEK under a new salt/KEK, so the old passphrase
//! stops working without re-encrypting any data. When `encrypt_at_rest` is
//! enforced, the DEK becomes the actual data key with no format change.
//!
//! No new dependencies: SHA-256 / HMAC / PBKDF2 are implemented here in pure
//! Rust so `cargo build` keeps working offline. No escrow: the sidecar stores
//! only salt + wrapped DEK (both non-secret alone), never the passphrase or
//! the unwrapped DEK.

use serde::{Deserialize, Serialize};

/// Minimum passphrase length (chars). Lenient + warning: below this is
/// rejected; above it a weak score warns but still proceeds with explicit ack.
pub const MIN_LEN: usize = 12;
/// PBKDF2 iteration count — ~100ms on a desktop, per locked decision.
pub const ITERS: u32 = 210_000;
/// Random salt bytes per node.
pub const SALT_BYTES: usize = 16;
/// Node key bytes (matches AES-256-GCM / recovery KEY_BYTES).
pub const KEY_BYTES: usize = 32;

#[derive(Debug, thiserror::Error)]
pub enum PassphraseError {
    #[error("choose a passphrase with at least {0} characters")]
    TooShort(usize),
    #[error("the two passphrases do not match")]
    Mismatch,
    #[error("could not read the operating system's random source: {0}")]
    Entropy(String),
    #[error("that passphrase did not unlock this node")]
    AuthFailed,
    #[error("this node's key envelope is corrupt: {0}")]
    Corrupt(&'static str),
}

/// Non-secret KDF parameters stored in node.json alongside the wrapped DEK.
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct KdfParams {
    pub kdf: String,
    pub iters: u32,
    pub salt_hex: String,
}

impl KdfParams {
    pub fn new(salt: &[u8]) -> Self {
        Self {
            kdf: "pbkdf2-sha256-210k".to_owned(),
            iters: ITERS,
            salt_hex: hex_encode(salt),
        }
    }

    pub fn salt_bytes(&self) -> Result<Vec<u8>, PassphraseError> {
        hex_decode(&self.salt_hex).map_err(|_| PassphraseError::Corrupt("bad salt hex"))
    }
}

// -- validation + strength -----------------------------------------------------

/// Small blocklist of catastrophically common secrets. Checked case-insensitively
/// as a substring so `P@ssw0rd!1234` still warns.
const BLOCKLIST: &[&str] = &[
    "password", "passw0rd", "123456", "qwerty", "letmein", "welcome", "admin",
    "desentry", "changeme", "iloveyou",
];

/// 0-4 strength score: length + character classes + blocklist penalty.
pub fn strength(passphrase: &str) -> (u8, &'static str) {
    let len = passphrase.chars().count();
    let mut classes = 0;
    if passphrase.chars().any(|c| c.is_lowercase()) {
        classes += 1;
    }
    if passphrase.chars().any(|c| c.is_uppercase()) {
        classes += 1;
    }
    if passphrase.chars().any(|c| c.is_ascii_digit()) {
        classes += 1;
    }
    if passphrase.chars().any(|c| !c.is_alphanumeric()) {
        classes += 1;
    }
    let lowered = passphrase.to_lowercase();
    let blocklisted = BLOCKLIST.iter().any(|b| lowered.contains(b));

    let mut score: i32 = 0;
    if len >= 12 {
        score += 1;
    }
    if len >= 16 {
        score += 1;
    }
    if len >= 20 {
        score += 1;
    }
    if classes >= 3 {
        score += 1;
    }
    if blocklisted {
        score -= 2;
    }
    let score = score.clamp(0, 4) as u8;
    let label = match score {
        0 | 1 => "weak",
        2 => "fair",
        3 => "strong",
        _ => "excellent",
    };
    (score, label)
}

/// Validates length + confirmation match. Weak-but-long passes here; the UI
/// shows the warning and requires an explicit ack checkbox instead.
pub fn validate(passphrase: &str, confirm: &str) -> Result<(), PassphraseError> {
    if passphrase.chars().count() < MIN_LEN {
        return Err(PassphraseError::TooShort(MIN_LEN));
    }
    if passphrase != confirm {
        return Err(PassphraseError::Mismatch);
    }
    Ok(())
}

// -- crypto (pure Rust, no new deps) -------------------------------------------

fn sha256(message: &[u8]) -> [u8; 32] {
    // FIPS 180-4, single-block-friendly reference implementation.
    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    ];
    let mut h: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    ];

    let bit_len = (message.len() as u64).wrapping_mul(8);
    let mut padded = message.to_vec();
    padded.push(0x80);
    while padded.len() % 64 != 56 {
        padded.push(0);
    }
    padded.extend_from_slice(&bit_len.to_be_bytes());

    for chunk in padded.chunks_exact(64) {
        let mut w = [0u32; 64];
        for (i, b) in chunk.chunks_exact(4).enumerate().take(16) {
            w[i] = u32::from_be_bytes([b[0], b[1], b[2], b[3]]);
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16].wrapping_add(s0).wrapping_add(w[i - 7]).wrapping_add(s1);
        }
        let (mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut hh) =
            (h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ ((!e) & g);
            let t1 = hh.wrapping_add(s1).wrapping_add(ch).wrapping_add(K[i]).wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(maj);
            hh = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
        h[5] = h[5].wrapping_add(f);
        h[6] = h[6].wrapping_add(g);
        h[7] = h[7].wrapping_add(hh);
    }

    let mut out = [0u8; 32];
    for (i, word) in h.iter().enumerate() {
        out[i * 4..i * 4 + 4].copy_from_slice(&word.to_be_bytes());
    }
    out
}

fn hmac_sha256(key: &[u8], data: &[u8]) -> [u8; 32] {
    let mut block = [0u8; 64];
    if key.len() > 64 {
        let h = sha256(key);
        block[..32].copy_from_slice(&h);
    } else {
        block[..key.len()].copy_from_slice(key);
    }
    let mut ipad = [0x36u8; 64];
    let mut opad = [0x5cu8; 64];
    for i in 0..64 {
        ipad[i] ^= block[i];
        opad[i] ^= block[i];
    }
    let mut inner = ipad.to_vec();
    inner.extend_from_slice(data);
    let inner_hash = sha256(&inner);
    let mut outer = opad.to_vec();
    outer.extend_from_slice(&inner_hash);
    sha256(&outer)
}

/// PBKDF2-HMAC-SHA256, dkLen == 32 (one extra block avoided by construction).
pub fn pbkdf2_sha256(password: &[u8], salt: &[u8], iters: u32) -> [u8; 32] {
    let mut u = hmac_sha256(password, &[salt, &1u32.to_be_bytes()].concat());
    let mut out = u;
    for _ in 1..iters {
        u = hmac_sha256(password, &u);
        for i in 0..32 {
            out[i] ^= u[i];
        }
    }
    out
}

/// Derives the 32-byte KEK from a passphrase + salt.
pub fn derive_kek(passphrase: &str, salt: &[u8], iters: u32) -> [u8; KEY_BYTES] {
    pbkdf2_sha256(passphrase.as_bytes(), salt, iters)
}

pub fn generate_salt() -> Result<[u8; SALT_BYTES], PassphraseError> {
    let mut salt = [0u8; SALT_BYTES];
    getrandom::getrandom(&mut salt).map_err(|e| PassphraseError::Entropy(e.to_string()))?;
    Ok(salt)
}

pub fn generate_dek() -> Result<[u8; KEY_BYTES], PassphraseError> {
    let mut dek = [0u8; KEY_BYTES];
    getrandom::getrandom(&mut dek).map_err(|e| PassphraseError::Entropy(e.to_string()))?;
    Ok(dek)
}

fn keystream(kek: &[u8; 32], salt: &[u8], len: usize) -> Vec<u8> {
    let mut stream = Vec::with_capacity(len);
    let mut counter: u32 = 1;
    while stream.len() < len {
        let mut msg = Vec::with_capacity(salt.len() + 4);
        msg.extend_from_slice(salt);
        msg.extend_from_slice(&counter.to_be_bytes());
        let block = hmac_sha256(kek, &msg);
        stream.extend_from_slice(&block);
        counter = counter.wrapping_add(1);
    }
    stream.truncate(len);
    stream
}

/// Wraps a DEK under a KEK. Returns (wrapped_hex, tag_hex).
pub fn wrap_dek(dek: &[u8; 32], kek: &[u8; 32], salt: &[u8]) -> (String, String) {
    let ks = keystream(kek, salt, 32);
    let mut wrapped = [0u8; 32];
    for i in 0..32 {
        wrapped[i] = dek[i] ^ ks[i];
    }
    let tag = hmac_sha256(kek, &wrapped);
    (hex_encode(&wrapped), hex_encode(&tag))
}

/// Unwraps, verifying the HMAC tag in constant-approximate time.
pub fn unwrap_dek(
    wrapped_hex: &str,
    tag_hex: &str,
    kek: &[u8; 32],
    salt: &[u8],
) -> Result<[u8; 32], PassphraseError> {
    let wrapped = hex_decode(wrapped_hex).map_err(|_| PassphraseError::Corrupt("bad wrapped hex"))?;
    let tag = hex_decode(tag_hex).map_err(|_| PassphraseError::Corrupt("bad tag hex"))?;
    if wrapped.len() != 32 || tag.len() != 32 {
        return Err(PassphraseError::Corrupt("wrong envelope length"));
    }
    let expect = hmac_sha256(kek, &wrapped);
    let mut diff = 0u8;
    for i in 0..32 {
        diff |= expect[i] ^ tag[i];
    }
    if diff != 0 {
        return Err(PassphraseError::AuthFailed);
    }
    let ks = keystream(kek, salt, 32);
    let mut dek = [0u8; 32];
    for i in 0..32 {
        dek[i] = wrapped[i] ^ ks[i];
    }
    Ok(dek)
}

// -- hex -----------------------------------------------------------------------

fn hex_encode(bytes: &[u8]) -> String {
    const H: &[u8] = b"0123456789abcdef";
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push(H[(b >> 4) as usize] as char);
        s.push(H[(b & 0xf) as usize] as char);
    }
    s
}

fn hex_decode(s: &str) -> Result<Vec<u8>, ()> {
    if s.len() % 2 != 0 {
        return Err(());
    }
    let mut out = Vec::with_capacity(s.len() / 2);
    let bytes = s.as_bytes();
    let val = |c: u8| match c {
        b'0'..=b'9' => Ok(c - b'0'),
        b'a'..=b'f' => Ok(c - b'a' + 10),
        b'A'..=b'F' => Ok(c - b'A' + 10),
        _ => Err(()),
    };
    for pair in bytes.chunks_exact(2) {
        out.push((val(pair[0])? << 4) | val(pair[1])?);
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sha256_matches_nist_vector() {
        // "abc" — FIPS 180-4 §B.
        let h = sha256(b"abc");
        assert_eq!(
            hex_encode(&h),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }

    #[test]
    fn pbkdf2_is_deterministic_and_salted() {
        let salt = [7u8; SALT_BYTES];
        let a = derive_kek("correct horse battery staple", &salt, 1000);
        let b = derive_kek("correct horse battery staple", &salt, 1000);
        assert_eq!(a, b);
        let other = derive_kek("correct horse battery staple", &[8u8; SALT_BYTES], 1000);
        assert_ne!(a, other);
    }

    #[test]
    fn wrap_unwrap_round_trips_and_rejects_wrong_passphrase() {
        let salt = [9u8; SALT_BYTES];
        let dek = [0xabu8; KEY_BYTES];
        let kek = derive_kek("my secret phrase here!", &salt, 1000);
        let (w, t) = wrap_dek(&dek, &kek, &salt);
        let back = unwrap_dek(&w, &t, &kek, &salt).unwrap();
        assert_eq!(back, dek);
        let wrong = derive_kek("a different phrase entirely", &salt, 1000);
        assert!(unwrap_dek(&w, &t, &wrong, &salt).is_err());
    }

    #[test]
    fn short_passphrase_rejected_and_mismatch_detected() {
        assert!(validate("short", "short").is_err());
        assert!(validate("long enough phrase", "different phrase!").is_err());
        assert!(validate("long enough phrase", "long enough phrase").is_ok());
    }

    #[test]
    fn blocklisted_phrase_scores_weak() {
        let (score, _) = strength("Password1234!");
        assert!(score <= 1, "blocklisted base must score weak, got {score}");
        let (good, _) = strength("turquoise falcon over dusty mesa 42!");
        assert!(good >= 3, "long varied phrase must score strong, got {good}");
    }

    #[test]
    fn two_salts_differ() {
        assert_ne!(generate_salt().unwrap(), generate_salt().unwrap());
    }
}
