// SPDX-License-Identifier: AGPL-3.0-only
//! Explicit personal vocabulary. No observation, network, or platform storage.
use std::collections::BTreeMap;

pub const MAX_WORDS: usize = 1024;
pub const MAX_WORD_BYTES: usize = 128;
pub const MAX_PINYIN_BYTES: usize = 128;
pub const MAX_WORD_FILE_BYTES: usize = 320 * 1024;
const MAX_HOMOPHONES: usize = 32;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PersonalWord {
    pub pinyin: String,
    pub text: String,
}

#[derive(Clone, Debug, Default)]
pub struct UserWords {
    rows: BTreeMap<(String, String), PersonalWord>,
    exact: BTreeMap<String, Vec<String>>,
    initials: BTreeMap<String, Vec<String>>,
    completions: BTreeMap<String, Vec<String>>,
    mixed: crate::spelling::MixedIndex<String>,
    revision: u64,
}

fn canonical(source: &str) -> Option<String> {
    if source.is_empty() || source.len() > MAX_PINYIN_BYTES {
        return None;
    }
    let replaced = source.replace('\'', " ");
    if !replaced
        .bytes()
        .all(|c| c.is_ascii_alphabetic() || c == b' ')
    {
        return None;
    }
    let result = replaced
        .split_ascii_whitespace()
        .collect::<Vec<_>>()
        .join(" ")
        .to_ascii_lowercase();
    (!result.is_empty()).then_some(result)
}

fn valid_text(text: &str) -> bool {
    !text.is_empty()
        && text.len() <= MAX_WORD_BYTES
        && text.chars().count() <= 32
        && !text.chars().any(|c| c.is_control() || c.is_whitespace())
        && text
            .chars()
            .any(|c| matches!(c, '\u{3400}'..='\u{9fff}' | '\u{20000}'..='\u{323af}'))
}

fn joined(pinyin: &str) -> String {
    pinyin.replace(' ', "")
}

impl UserWords {
    pub fn len(&self) -> usize {
        self.rows.len()
    }
    pub fn is_empty(&self) -> bool {
        self.rows.is_empty()
    }
    pub fn revision(&self) -> u64 {
        self.revision
    }
    pub fn row(&self, index: usize) -> Option<&PersonalWord> {
        self.rows.values().nth(index)
    }

    fn reindex(&mut self) {
        self.exact.clear();
        self.initials.clear();
        self.completions.clear();
        self.mixed = crate::spelling::MixedIndex::default();
        for word in self.rows.values() {
            self.mixed
                .insert(&word.pinyin, word.text.clone(), 1_000_000);
            let spelling = joined(&word.pinyin);
            let bucket = self.completions.entry(spelling).or_default();
            if !bucket.contains(&word.text) {
                bucket.push(word.text.clone());
            }
            let initials: String = word
                .pinyin
                .split(' ')
                .filter_map(|s| s.chars().next())
                .collect();
            if initials.len() >= 2 {
                let bucket = self.initials.entry(initials).or_default();
                if bucket.len() < crate::dictionary::MAX_CANDIDATES && !bucket.contains(&word.text)
                {
                    bucket.push(word.text.clone());
                }
            }
            for key in [joined(&word.pinyin), word.pinyin.replace(' ', "'")] {
                let bucket = self.exact.entry(key).or_default();
                if !bucket.contains(&word.text) {
                    bucket.push(word.text.clone());
                }
            }
        }
    }

    /// Explicit entries take priority only at a complete spelling. Partial
    /// prefixes are merged separately. Explicit selection learning runs afterward.
    pub fn apply(&self, input: &str, candidates: &mut Vec<String>) {
        if let Some(personal) = self.exact.get(input) {
            let mut result = personal.clone();
            for text in candidates.drain(..) {
                if !result.contains(&text) {
                    result.push(text);
                }
                if result.len() == crate::dictionary::MAX_CANDIDATES {
                    break;
                }
            }
            result.truncate(crate::dictionary::MAX_CANDIDATES);
            *candidates = result;
        }
    }
    pub(crate) fn exact_texts(&self, input: &str) -> &[String] {
        self.exact.get(input).map_or(&[], Vec::as_slice)
    }

    /// Derived indexes are rebuilt only on edits. Prefix range visits are bounded
    /// by the profile's 1024 rows; a query never scans unrelated spellings.
    pub(crate) fn supplements(&self, input: &str) -> Vec<String> {
        let mut output = self.initials.get(input).cloned().unwrap_or_default();
        if input.len() < 2 || !input.bytes().all(|b| b.is_ascii_lowercase()) {
            return Vec::new();
        }
        for (key, words) in self.completions.range(input.to_owned()..) {
            if !key.starts_with(input) || output.len() == crate::dictionary::MAX_CANDIDATES {
                break;
            }
            for text in words {
                if output.len() == crate::dictionary::MAX_CANDIDATES {
                    break;
                }
                if !output.contains(text) {
                    output.push(text.clone());
                }
            }
        }
        output
    }
    pub(crate) fn is_initials(&self, input: &str) -> bool {
        self.initials.contains_key(input)
    }
    pub(crate) fn mixed_texts(&self, input: &str) -> Vec<String> {
        let mut output = self.mixed.matches(input);
        output.sort();
        output.truncate(crate::dictionary::MAX_CANDIDATES);
        output
    }

    /// -1 invalid, -5 capacity. Exact duplicates are inert and idempotent.
    pub fn add(&mut self, pinyin: &str, text: &str) -> Result<(), i32> {
        let pinyin = canonical(pinyin).ok_or(-1)?;
        if !valid_text(text) {
            return Err(-1);
        }
        let key = (pinyin.clone(), text.to_owned());
        if self.rows.contains_key(&key) {
            return Ok(());
        }
        let spelling = joined(&pinyin);
        if self.len() == MAX_WORDS
            || self
                .rows
                .values()
                .filter(|word| {
                    word.pinyin
                        .bytes()
                        .filter(|byte| *byte != b' ')
                        .eq(spelling.bytes())
                })
                .count()
                >= MAX_HOMOPHONES
        {
            return Err(-5);
        }
        let revision = self.revision.checked_add(1).ok_or(-1)?;
        self.rows.insert(
            key,
            PersonalWord {
                pinyin,
                text: text.to_owned(),
            },
        );
        self.revision = revision;
        self.reindex();
        Ok(())
    }

    pub fn remove(&mut self, pinyin: &str, text: &str) -> Result<(), i32> {
        let pinyin = canonical(pinyin).ok_or(-1)?;
        if !valid_text(text) {
            return Err(-1);
        }
        let key = (pinyin, text.to_owned());
        if self.rows.contains_key(&key) {
            let revision = self.revision.checked_add(1).ok_or(-1)?;
            self.rows.remove(&key);
            self.revision = revision;
            self.reindex();
        }
        Ok(())
    }

    pub fn export(&self) -> Vec<u8> {
        let mut bytes = b"TIW1".to_vec();
        bytes.extend_from_slice(&self.revision.to_le_bytes());
        bytes.extend_from_slice(&(self.len() as u32).to_le_bytes());
        for word in self.rows.values() {
            bytes.extend_from_slice(&(word.pinyin.len() as u16).to_le_bytes());
            bytes.extend_from_slice(&(word.text.len() as u16).to_le_bytes());
            bytes.extend_from_slice(word.pinyin.as_bytes());
            bytes.extend_from_slice(word.text.as_bytes());
        }
        bytes
    }

    pub fn import(bytes: &[u8]) -> Result<Self, i32> {
        if bytes.len() < 16 || bytes.len() > MAX_WORD_FILE_BYTES || &bytes[..4] != b"TIW1" {
            return Err(-1);
        }
        let revision = u64::from_le_bytes(bytes[4..12].try_into().map_err(|_| -1)?);
        let count = u32::from_le_bytes(bytes[12..16].try_into().map_err(|_| -1)?) as usize;
        if count > MAX_WORDS || (count > 0 && revision == 0) {
            return Err(-1);
        }
        let mut result = Self::default();
        let mut offset = 16;
        for _ in 0..count {
            let lengths = bytes.get(offset..offset + 4).ok_or(-1)?;
            let first = u16::from_le_bytes([lengths[0], lengths[1]]) as usize;
            let second = u16::from_le_bytes([lengths[2], lengths[3]]) as usize;
            if first == 0 || first > MAX_PINYIN_BYTES || second == 0 || second > MAX_WORD_BYTES {
                return Err(-1);
            }
            offset += 4;
            let pinyin = std::str::from_utf8(bytes.get(offset..offset + first).ok_or(-1)?)
                .map_err(|_| -1)?;
            offset += first;
            let text = std::str::from_utf8(bytes.get(offset..offset + second).ok_or(-1)?)
                .map_err(|_| -1)?;
            offset += second;
            if canonical(pinyin).as_deref() != Some(pinyin) {
                return Err(-1);
            }
            let before = result.len();
            result.add(pinyin, text)?;
            if result.len() == before {
                return Err(-1);
            }
        }
        if offset != bytes.len() {
            return Err(-1);
        }
        result.revision = revision;
        Ok(result)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn explicit_spelling_priority_dedup_and_removal() {
        let mut words = UserWords::default();
        words.add("XING  HE'yan jiu suo", "星河研究所").unwrap();
        let revision = words.revision();
        words.add("xing he yan jiu suo", "星河研究所").unwrap();
        assert_eq!((words.len(), words.revision()), (1, revision));
        for input in ["xinghey anjiusuo", "xh", "xinghe"] {
            let mut base = vec!["星河".to_owned()];
            words.apply(input, &mut base);
            assert_eq!(base, ["星河"]);
        }
        for input in ["xingheyanjiusuo", "xing'he'yan'jiu'suo"] {
            let mut base = vec!["星河".to_owned(), "星河研究所".to_owned()];
            words.apply(input, &mut base);
            assert_eq!(base, ["星河研究所", "星河"]);
        }
        words.remove("xing he yan jiu suo", "星河研究所").unwrap();
        assert!(words.is_empty());
    }
    #[test]
    fn bounds_corruption_and_transactional_decode() {
        let mut words = UserWords::default();
        for (pinyin, text) in [
            ("", "测试"),
            ("ni3 hao", "你好"),
            ("ni\nhao", "你好"),
            ("ni", "ASCII"),
            ("ni", "你 好"),
            ("ni", "\u{0000}你"),
        ] {
            assert_eq!(words.add(pinyin, text), Err(-1));
        }
        words.add("xing he", "星河").unwrap();
        let bytes = words.export();
        assert_eq!(UserWords::import(&bytes).unwrap().export(), bytes);
        for end in 0..bytes.len() {
            assert!(UserWords::import(&bytes[..end]).is_err());
        }
        let mut extra = bytes.clone();
        extra.push(0);
        assert!(UserWords::import(&extra).is_err());
        for n in 0..32 {
            words.add("ce shi", &format!("测试{n}")).unwrap();
        }
        assert_eq!(words.add("ce shi", "测试越界"), Err(-5));
        assert_eq!(words.len(), 33);
    }
    #[test]
    fn full_capacity_never_evicts_or_hides_failure() {
        let mut words = UserWords::default();
        for index in 0..MAX_WORDS {
            let pinyin = format!(
                "ce{}{}{}",
                (b'a' + (index / 676) as u8) as char,
                (b'a' + (index / 26 % 26) as u8) as char,
                (b'a' + (index % 26) as u8) as char
            );
            words.add(&pinyin, &format!("虚构词{index}")).unwrap();
        }
        let saved = words.export();
        assert_eq!(words.add("zui hou", "最后词条"), Err(-5));
        assert_eq!(words.export(), saved);
        assert_eq!(UserWords::import(&saved).unwrap().len(), MAX_WORDS);
        let restored = UserWords::import(&saved).unwrap();
        let matches = restored.supplements("ce");
        assert_eq!(matches.len(), crate::dictionary::MAX_CANDIDATES);
        assert_eq!(
            matches
                .iter()
                .collect::<std::collections::BTreeSet<_>>()
                .len(),
            matches.len()
        );
        assert_eq!(restored.supplements("ce"), words.supplements("ce"));
        assert!(restored.supplements("c").is_empty());
        assert!(restored.supplements("zzzz").is_empty());
    }
}
