//! Recovery keys and at-rest key custody.
//!
//! There is no escrow. A node's at-rest key exists in exactly two places: the
//! OS keychain on the machine that created it, and wherever the user put the
//! recovery key when the wizard showed it. Nothing in this app, in the
//! supervisor's registry, or on the node's disk can reproduce it. The
//! supervisor records only *that* an export happened and when
//! (`ManagedNode::recovery_key_exported`), never the key.
//!
//! That is a deliberate and costly choice: a user who loses both copies loses
//! the data. The alternative -- a copy the app can reach -- is a copy an
//! attacker who reaches the app can reach, which would make the encryption
//! decorative.

use std::fmt::Write as _;
use std::fs;
use std::path::Path;

/// Crockford base32: no I, L, O or U, so a key read off a printout or a phone
/// screen has no character pairs that are routinely confused.
const ALPHABET: &[u8] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/// 32 bytes -- 256 bits of entropy, matching the AES-256-GCM key it protects.
const KEY_BYTES: usize = 32;
/// Groups of five, for reading aloud and for typing back in.
const GROUP: usize = 5;

#[derive(Debug, thiserror::Error)]
pub enum RecoveryError {
    #[error("could not read the operating system's random source: {0}")]
    Entropy(String),
    #[error("could not write the recovery key to {0}: {1}")]
    Write(String, String),
    #[error("that is not a valid recovery key: {0}")]
    Malformed(&'static str),
}

/// Generates a fresh recovery key.
///
/// Entropy comes straight from the OS (`getrandom`), not from a seeded PRNG:
/// there is no state to get wrong, and no risk of two nodes created in the
/// same millisecond sharing a seed.
pub fn generate() -> Result<String, RecoveryError> {
    let mut bytes = [0u8; KEY_BYTES];
    getrandom::getrandom(&mut bytes).map_err(|error| RecoveryError::Entropy(error.to_string()))?;
    Ok(encode(&bytes))
}

/// Base32-encodes raw key bytes into the grouped, human-transcribable form.
pub fn encode(bytes: &[u8]) -> String {
    let mut bits = 0u32;
    let mut bit_count = 0u32;
    let mut symbols = String::new();

    for byte in bytes {
        bits = (bits << 8) | u32::from(*byte);
        bit_count += 8;
        while bit_count >= 5 {
            bit_count -= 5;
            let index = ((bits >> bit_count) & 0x1f) as usize;
            symbols.push(ALPHABET[index] as char);
        }
    }
    if bit_count > 0 {
        let index = ((bits << (5 - bit_count)) & 0x1f) as usize;
        symbols.push(ALPHABET[index] as char);
    }

    let mut grouped = String::with_capacity(symbols.len() + symbols.len() / GROUP);
    for (i, symbol) in symbols.chars().enumerate() {
        if i > 0 && i % GROUP == 0 {
            grouped.push('-');
        }
        grouped.push(symbol);
    }
    grouped
}

/// Parses a recovery key back to bytes, tolerating how people actually type.
///
/// Case is ignored, separators are ignored, and the four Crockford
/// substitutions are applied (O->0, I/L->1) -- a key transcribed from paper
/// that fails to parse over a capital O would be a cruel way to lose data.
pub fn decode(text: &str) -> Result<Vec<u8>, RecoveryError> {
    let mut bits = 0u32;
    let mut bit_count = 0u32;
    let mut out = Vec::new();

    for raw in text.chars() {
        let symbol = match raw.to_ascii_uppercase() {
            '-' | ' ' | '\t' | '\n' | '\r' | '_' => continue,
            'O' => '0',
            'I' | 'L' => '1',
            other => other,
        };
        let Some(index) = ALPHABET.iter().position(|c| *c as char == symbol) else {
            return Err(RecoveryError::Malformed("it contains a character that is not part of a key"));
        };
        bits = (bits << 5) | index as u32;
        bit_count += 5;
        if bit_count >= 8 {
            bit_count -= 8;
            out.push(((bits >> bit_count) & 0xff) as u8);
        }
    }

    if out.len() != KEY_BYTES {
        return Err(RecoveryError::Malformed("it is the wrong length"));
    }
    Ok(out)
}

/// Writes the key to a file the user chose.
///
/// The file is plain text with a header explaining what it is, because a
/// stray file of 56 base32 characters found in two years' time is otherwise
/// unidentifiable -- and a recovery key nobody recognises is a recovery key
/// nobody keeps.
pub fn export(path: &Path, node_name: &str, node_id: &str, key: &str) -> Result<(), RecoveryError> {
    let mut body = String::new();
    let _ = writeln!(body, "De-Sentry recovery key");
    let _ = writeln!(body, "======================");
    let _ = writeln!(body);
    let _ = writeln!(body, "Node:    {node_name}");
    let _ = writeln!(body, "Node ID: {node_id}");
    let _ = writeln!(body);
    let _ = writeln!(body, "{key}");
    let _ = writeln!(body);
    let _ = writeln!(
        body,
        "This key decrypts the data stored by this node. It is not held anywhere else:"
    );
    let _ = writeln!(
        body,
        "not by the application, not by the node itself, and not by anyone who wrote it."
    );
    let _ = writeln!(
        body,
        "If this file and this machine's keychain entry are both lost, the data is gone."
    );
    let _ = writeln!(body);
    let _ = writeln!(body, "Keep it somewhere a backup would survive a lost laptop.");

    fs::write(path, body)
        .map_err(|error| RecoveryError::Write(path.display().to_string(), error.to_string()))?;

    // Owner-only, matching the treatment of identity.key. On Windows the file
    // inherits the user profile's ACL, which is already owner-scoped.
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = fs::set_permissions(path, fs::Permissions::from_mode(0o600));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_generated_key_round_trips() {
        let key = generate().expect("the OS has a random source");
        let bytes = decode(&key).expect("a generated key parses");
        assert_eq!(bytes.len(), KEY_BYTES);
        assert_eq!(encode(&bytes), key);
    }

    #[test]
    fn keys_are_grouped_and_use_no_confusable_letters() {
        let key = generate().unwrap();
        assert!(key.contains('-'));
        for c in key.chars().filter(|c| *c != '-') {
            assert!(!"ILOU".contains(c), "{c} is easily misread");
        }
    }

    #[test]
    fn transcription_slips_are_tolerated() {
        let bytes = vec![0xab; KEY_BYTES];
        let key = encode(&bytes);
        // Lower case, spaces instead of dashes, and the classic O-for-zero.
        let typed = key.to_lowercase().replace('-', " ").replace('0', "O");
        assert_eq!(decode(&typed).unwrap(), bytes);
    }

    #[test]
    fn a_truncated_key_is_rejected() {
        let key = generate().unwrap();
        assert!(decode(&key[..key.len() - 6]).is_err());
    }

    #[test]
    fn two_keys_differ() {
        assert_ne!(generate().unwrap(), generate().unwrap());
    }
}
