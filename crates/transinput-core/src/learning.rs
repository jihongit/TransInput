// SPDX-License-Identifier: AGPL-3.0-only
//! Bounded local selection preferences. No text is learned by observing input.
use std::{cmp::Reverse, collections::BTreeMap};

pub const MAX_LEARNED: usize = 512;
pub const MAX_LEARNING_BYTES: usize = 512 * 1024;

#[derive(Clone, Debug)]
struct Selection {
    uses: u32,
    last: u64,
}

#[derive(Debug, Default)]
pub struct Learning {
    rows: BTreeMap<String, BTreeMap<String, Selection>>,
    count: usize,
    clock: u64,
}

fn valid(input: &str, text: &str) -> bool {
    !input.is_empty()
        && input.len() <= crate::dictionary::MAX_INPUT_BYTES
        && input.bytes().all(|c| c.is_ascii_lowercase() || c == b'\'')
        && !input.starts_with('\'')
        && !input.ends_with('\'')
        && !input.contains("''")
        && !text.is_empty()
        && text.len() <= 512
        && !text.chars().any(char::is_control)
        && text
            .chars()
            .any(|c| matches!(c, '\u{3400}'..='\u{9fff}' | '\u{20000}'..='\u{323af}'))
}

impl Learning {
    pub fn len(&self) -> usize {
        self.count
    }
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// Called only after an explicit, successful local Chinese selection. The
    /// caller must additionally check that `text` was an available candidate.
    pub fn record(&mut self, input: &str, text: &str) -> bool {
        if !valid(input, text) {
            return false;
        }
        if self.clock == u64::MAX {
            let mut order: Vec<_> = self
                .rows
                .iter()
                .flat_map(|(key, words)| {
                    words
                        .iter()
                        .map(move |(text, row)| (row.last, key.clone(), text.clone()))
                })
                .collect();
            order.sort_unstable();
            for (index, (_, key, text)) in order.into_iter().enumerate() {
                self.rows
                    .get_mut(&key)
                    .unwrap()
                    .get_mut(&text)
                    .unwrap()
                    .last = index as u64 + 1;
            }
            self.clock = self.count as u64;
        }
        self.clock += 1;
        if let Some(row) = self
            .rows
            .get_mut(input)
            .and_then(|words| words.get_mut(text))
        {
            row.uses = row.uses.saturating_add(1);
            row.last = self.clock;
            return true;
        }
        if self.count == MAX_LEARNED {
            let oldest = self
                .rows
                .iter()
                .flat_map(|(key, words)| {
                    words
                        .iter()
                        .map(move |(text, row)| (row.last, key.clone(), text.clone()))
                })
                .min()
                .unwrap();
            let words = self.rows.get_mut(&oldest.1).unwrap();
            words.remove(&oldest.2);
            if words.is_empty() {
                self.rows.remove(&oldest.1);
            }
            self.count -= 1;
        }
        self.rows.entry(input.to_owned()).or_default().insert(
            text.to_owned(),
            Selection {
                uses: 1,
                last: self.clock,
            },
        );
        self.count += 1;
        true
    }

    /// Reorder only the current dictionary's candidates. Imported or stale data
    /// cannot inject text, execute actions, or affect another pinyin context.
    pub fn rank(&self, input: &str, candidates: &mut [String]) {
        if let Some(words) = self.rows.get(input) {
            candidates.sort_by_key(|text| Reverse(words.get(text).map_or(0, |row| row.last)));
        }
    }

    pub fn export(&self) -> Vec<u8> {
        use std::fmt::Write;
        let mut output = String::from("TIL1\n");
        for (input, words) in &self.rows {
            for (text, row) in words {
                writeln!(output, "{input}\t{text}\t{}\t{}", row.uses, row.last).unwrap();
            }
        }
        output.into_bytes()
    }

    /// Transactional decoding. Platform code encrypts this in memory format
    /// before storing it; the shared core never chooses or accesses a path.
    pub fn from_bytes(bytes: &[u8]) -> Result<Self, &'static str> {
        if bytes.len() > MAX_LEARNING_BYTES {
            return Err("learning size");
        }
        let source = std::str::from_utf8(bytes).map_err(|_| "learning encoding")?;
        let body = source.strip_prefix("TIL1\n").ok_or("learning version")?;
        let mut result = Self::default();
        for line in body.lines() {
            let fields: Vec<_> = line.split('\t').collect();
            if fields.len() != 4 || !valid(fields[0], fields[1]) || result.count == MAX_LEARNED {
                return Err("learning record");
            }
            let uses: u32 = fields[2].parse().map_err(|_| "learning count")?;
            let last: u64 = fields[3].parse().map_err(|_| "learning sequence")?;
            if uses == 0 || last == 0 {
                return Err("learning value");
            }
            let words = result.rows.entry(fields[0].to_owned()).or_default();
            if words
                .insert(fields[1].to_owned(), Selection { uses, last })
                .is_some()
            {
                return Err("learning duplicate");
            }
            result.count += 1;
            result.clock = result.clock.max(last);
        }
        Ok(result)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn selection_promotes_recent_choice_without_learning_a_preview() {
        let mut learning = Learning::default();
        let original = vec!["中国".into(), "这个".into(), "整个".into()];
        let mut candidates = original.clone();
        learning.rank("zg", &mut candidates);
        assert_eq!(candidates, original);
        assert!(learning.record("zg", "整个"));
        learning.rank("zg", &mut candidates);
        assert_eq!(candidates[0], "整个");
        assert!(learning.record("zg", "这个"));
        learning.rank("zg", &mut candidates);
        assert_eq!(candidates[0], "这个");
        let mut other = original.clone();
        learning.rank("zhongguo", &mut other);
        assert_eq!(other, original);
        assert!(learning.record("zg", "不存在的候选"));
        learning.rank("zg", &mut candidates);
        assert!(!candidates.iter().any(|text| text == "不存在的候选"));
    }
    #[test]
    fn bounded_history_roundtrip_eviction_and_sequence_overflow() {
        let mut learning = Learning::default();
        for i in 0..MAX_LEARNED + 20 {
            assert!(learning.record("zg", &format!("中文{i}")));
        }
        assert_eq!(learning.len(), MAX_LEARNED);
        assert!(!learning.rows["zg"].contains_key("中文0"));
        let encoded = learning.export();
        let mut restored = Learning::from_bytes(&encoded).unwrap();
        assert_eq!(restored.export(), encoded);
        restored.clock = u64::MAX;
        assert!(restored.record("nh", "你好"));
        assert_eq!(restored.len(), MAX_LEARNED);
        assert!(restored.clock < u64::MAX);
    }
    #[test]
    fn maximum_sized_records_fit_the_owned_export_budget() {
        let mut learning = Learning::default();
        let input = "n".repeat(128);
        for index in 0..MAX_LEARNED {
            let text = format!("中{}{:03}", "a".repeat(506), index);
            assert_eq!(text.len(), 512);
            assert!(learning.record(&input, &text));
        }
        let bytes = learning.export();
        assert!(bytes.len() <= MAX_LEARNING_BYTES);
        assert_eq!(Learning::from_bytes(&bytes).unwrap().export(), bytes);
        assert!(!learning.record(&"n".repeat(129), "你好"));
        assert!(!learning.record("nh", &format!("中{}", "a".repeat(510))));
        assert!(Learning::from_bytes(&vec![0; MAX_LEARNING_BYTES + 1]).is_err());
    }
    #[test]
    fn malformed_or_untrusted_records_cannot_replace_valid_state() {
        for bytes in [
            b"".as_slice(),
            b"TIL2\n",
            b"TIL1\nnh\tEnglish\t1\t1\n",
            b"TIL1\nnh\t\xff\t1\t1\n",
        ] {
            assert!(Learning::from_bytes(bytes).is_err());
        }
        for text in [
            "TIL1\nnh\t你好\t0\t1\n",
            "TIL1\nnh\t你好\t1\t0\n",
            "TIL1\nnh\t你好\t1\t1\nnh\t你好\t2\t2\n",
            "TIL1\n'nh\t你好\t1\t1\n",
        ] {
            assert!(Learning::from_bytes(text.as_bytes()).is_err());
        }
        let mut learning = Learning::default();
        assert!(learning.record("nh", "你好"));
        for (input, text) in [
            ("NH", "你好"),
            ("n\0h", "你好"),
            ("nh", "Hello"),
            ("nh", "你好\n"),
        ] {
            assert!(!learning.record(input, text));
        }
        assert_eq!(learning.len(), 1);
    }
}
