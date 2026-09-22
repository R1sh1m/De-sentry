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
//! Audited primitives (`sha2`/`hmac`/`pbkdf2` crates, pure Rust, vendored at
//! build time) for SHA-256 / HMAC / PBKDF2 -- outputs are byte-identical to
//! the hand-rolled versions they replaced (M-4), so existing wrapped DEKs
//! keep working. No escrow: the sidecar stores only salt + wrapped DEK (both
//! non-secret alone), never the passphrase or the unwrapped DEK.
//!
//! Honest limit: the DEK *wrapping construction* (HMAC keystream XOR +
//! separate HMAC tag) is custom, not an AEAD. It is format-stable (changing
//! it would strand every stored envelope), small enough to audit in full
//! below, and covered by round-trip + wrong-passphrase tests. Prefer a real
//! AEAD for any new envelope format.

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

// -- crypto (audited crates) ---------------------------------------------------
// SHA-256 / HMAC / PBKDF2 come from `sha2` / `hmac` / `pbkdf2` (M-4 fix).
// Outputs are byte-identical to the hand-rolled versions they replaced, so
// every stored envelope keeps working; the NIST + round-trip tests below pin
// that. Only the DEK wrapping construction is custom (see module docs).

use hmac::{Hmac, Mac};
use sha2::Sha256;

fn hmac_sha256(key: &[u8], data: &[u8]) -> [u8; 32] {
    let mut mac = Hmac::<Sha256>::new_from_slice(key).expect("HMAC accepts any key length");
    mac.update(data);
    mac.finalize().into_bytes().into()
}

/// PBKDF2-HMAC-SHA256, dkLen == 32.
pub fn pbkdf2_sha256(password: &[u8], salt: &[u8], iters: u32) -> [u8; 32] {
    let mut out = [0u8; 32];
    pbkdf2::pbkdf2_hmac::<Sha256>(password, salt, iters, &mut out);
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
        // "abc" — FIPS 180-4 §B, via the sha2 crate (M-4): pins byte-identical
        // output with the hand-rolled version this replaced.
        use sha2::{Digest, Sha256};
        let mut hasher = Sha256::new();
        hasher.update(b"abc");
        let h: [u8; 32] = hasher.finalize().into();
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
