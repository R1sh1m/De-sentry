//! Unit tests for the Unicode‑aware `contains_word` helper.

use de_sentry_app::ai::contains_word;

#[test]
fn test_contains_word_unicode_boundary() {
    // Chinese characters – should match whole word boundaries.
    let haystack = "这是一个测试例子"; // "this is a test example"
    let needle = "测试"; // "test"
    assert!(contains_word(haystack, needle));

    // Should NOT match when the needle is only a substring of a larger word.
    let haystack2 = "示例测试用例"; // "example test case"
    let needle2 = "测试用"; // partial inside a larger token
    assert!(!contains_word(haystack2, needle2));
}

#[test]
fn test_contains_word_ascii_boundary() {
    let haystack = "The quick-brown_fox jumps over the lazy dog.";
    assert!(contains_word(haystack, "fox"));
    // Substring inside another word should not match.
    assert!(!contains_word(haystack, "ox"));
    // Word with punctuation before/after should match.
    assert!(contains_word(haystack, "quick"));
    assert!(contains_word(haystack, "dog"));
}
