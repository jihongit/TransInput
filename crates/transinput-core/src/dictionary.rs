// SPDX-License-Identifier: AGPL-3.0-only
use std::{
    cmp::Reverse,
    collections::{BTreeMap, BinaryHeap, HashSet},
    sync::Arc,
};

pub const MAX_INPUT_BYTES: usize = 128;
pub const PAGE_SIZE: usize = 9;
pub const MAX_CANDIDATES: usize = PAGE_SIZE * 10;
const MAX_KEY_BYTES: usize = 128;
const MAX_TEXT_BYTES: usize = 512;
const MAX_ENTRIES: usize = 500_000;
// Bound duplicate pronunciation visits, rather than discarding lexically later keys.
const MAX_COMPLETION_VISITS: usize = 512;
const BEAM: usize = 9;

#[derive(Clone, Debug, PartialEq, Eq)]
struct Word {
    text: String,
    frequency: u32,
}

#[derive(Clone, Debug)]
pub struct Dictionary {
    words: BTreeMap<String, Vec<Arc<Word>>>,
    initials: BTreeMap<String, Vec<Arc<Word>>>,
    separated: BTreeMap<String, Vec<Arc<Word>>>,
    completions: CompletionIndex,
    mixed: crate::spelling::MixedIndex<Arc<Word>>,
    entries: usize,
}

#[derive(Clone, Debug)]
struct Completion {
    key: Arc<str>,
    word: Arc<Word>,
    rank: usize,
}

/// Sorted prefix ranges plus a range-minimum tree over frequency ranks. A short
/// prefix can reach high-frequency entries anywhere in its range in O(log N),
/// without walking all dictionary keys on each keystroke.
#[derive(Clone, Debug, Default)]
struct CompletionIndex {
    entries: Vec<Completion>,
    minima: Vec<usize>,
    leaves: usize,
}

impl CompletionIndex {
    fn new(words: &BTreeMap<String, Vec<Arc<Word>>>) -> Self {
        let mut entries = Vec::new();
        for (key, bucket) in words {
            let shared_key: Arc<str> = Arc::from(key.as_str());
            for word in bucket.iter().take(MAX_CANDIDATES) {
                entries.push(Completion {
                    key: shared_key.clone(),
                    word: word.clone(),
                    rank: 0,
                });
            }
        }
        let mut ranked: Vec<usize> = (0..entries.len()).collect();
        ranked.sort_unstable_by(|&a, &b| {
            compare_words(&entries[a].word, &entries[b].word)
                .then_with(|| entries[a].key.cmp(&entries[b].key))
        });
        for (rank, index) in ranked.into_iter().enumerate() {
            entries[index].rank = rank;
        }
        let leaves = entries.len().next_power_of_two();
        let mut result = Self {
            entries,
            minima: vec![usize::MAX; leaves * 2],
            leaves,
        };
        for i in 0..result.entries.len() {
            result.minima[leaves + i] = i;
        }
        for node in (1..leaves).rev() {
            result.minima[node] =
                result.better(result.minima[node * 2], result.minima[node * 2 + 1]);
        }
        result
    }

    fn better(&self, a: usize, b: usize) -> usize {
        if a == usize::MAX {
            b
        } else if b == usize::MAX || self.entries[a].rank < self.entries[b].rank {
            a
        } else {
            b
        }
    }

    fn best(&self, begin: usize, end: usize) -> usize {
        let (mut left, mut right) = (begin + self.leaves, end + self.leaves);
        let mut best = usize::MAX;
        while left < right {
            if left % 2 == 1 {
                best = self.better(best, self.minima[left]);
                left += 1;
            }
            if right % 2 == 1 {
                right -= 1;
                best = self.better(best, self.minima[right]);
            }
            left /= 2;
            right /= 2;
        }
        best
    }

    fn has_prefix(&self, prefix: &str) -> bool {
        let index = self
            .entries
            .partition_point(|entry| entry.key.as_ref() < prefix);
        self.entries
            .get(index)
            .is_some_and(|entry| entry.key.starts_with(prefix))
    }

    fn append(&self, prefix: &str, output: &mut Vec<String>) {
        let begin = self
            .entries
            .partition_point(|entry| entry.key.as_ref() < prefix);
        // Keys are lowercase ASCII only, so '{' is the exclusive prefix bound.
        let upper = format!("{prefix}{{");
        let end = self
            .entries
            .partition_point(|entry| entry.key.as_ref() < upper.as_str());
        let mut ranges = BinaryHeap::new();
        let add = |ranges: &mut BinaryHeap<_>, left, right| {
            if left < right {
                let index = self.best(left, right);
                ranges.push((Reverse(self.entries[index].rank), left, right, index));
            }
        };
        add(&mut ranges, begin, end);
        for _ in 0..MAX_COMPLETION_VISITS {
            if output.len() == MAX_CANDIDATES {
                break;
            }
            let Some((_, left, right, index)) = ranges.pop() else {
                break;
            };
            let entry = &self.entries[index];
            if !output.contains(&entry.word.text) {
                output.push(entry.word.text.clone());
            }
            add(&mut ranges, left, index);
            add(&mut ranges, index + 1, right);
        }
    }
}

fn compare_words(a: &Word, b: &Word) -> std::cmp::Ordering {
    b.frequency
        .cmp(&a.frequency)
        .then_with(|| a.text.cmp(&b.text))
}

fn resolve_keys(
    keys: BTreeMap<String, Vec<(String, String)>>,
    words: &BTreeMap<String, Vec<Arc<Word>>>,
) -> BTreeMap<String, Vec<Arc<Word>>> {
    let mut result = BTreeMap::new();
    for (key, spellings) in keys {
        let mut bucket = Vec::new();
        for (pinyin, text) in spellings {
            if let Some(word) = words[&pinyin].iter().find(|word| word.text == text) {
                bucket.push(word.clone());
            }
        }
        bucket.sort_by(|a, b| compare_words(a, b));
        let mut seen = HashSet::new();
        bucket.retain(|word| seen.insert(word.text.clone()));
        bucket.truncate(MAX_CANDIDATES);
        result.insert(key, bucket);
    }
    result
}

impl Dictionary {
    /// Reads plain `text<TAB>space-separated-pinyin<TAB>frequency` data.
    /// Input data needs its own redistribution/source review before packaging.
    pub fn from_tsv(source: &str) -> Result<Self, String> {
        if source.len() > 64 * 1024 * 1024 {
            return Err("dictionary exceeds 64 MiB".into());
        }
        let mut result = Self {
            words: BTreeMap::new(),
            initials: BTreeMap::new(),
            separated: BTreeMap::new(),
            completions: CompletionIndex::default(),
            mixed: crate::spelling::MixedIndex::default(),
            entries: 0,
        };
        let mut initial_keys: BTreeMap<String, Vec<(String, String)>> = BTreeMap::new();
        let mut separated_keys: BTreeMap<String, Vec<(String, String)>> = BTreeMap::new();
        for (index, raw) in source.lines().enumerate() {
            let line = raw.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let fields: Vec<&str> = line.split('\t').collect();
            if !(2..=3).contains(&fields.len()) {
                return Err(format!("invalid field count at line {}", index + 1));
            }
            let text = fields[0];
            let key: String = fields[1].split_ascii_whitespace().collect();
            if text.is_empty()
                || text.len() > MAX_TEXT_BYTES
                || text.chars().any(char::is_control)
                || key.is_empty()
                || key.len() > MAX_KEY_BYTES
                || !key.bytes().all(|c| c.is_ascii_lowercase())
            {
                return Err(format!("invalid text or pinyin at line {}", index + 1));
            }
            let frequency = if fields.len() == 3 {
                fields[2]
                    .parse::<u32>()
                    .map_err(|_| format!("invalid weight at line {}", index + 1))?
            } else {
                1
            };
            let bucket = result.words.entry(key.clone()).or_default();
            if let Some(existing) = bucket.iter_mut().find(|word| word.text == text) {
                Arc::make_mut(existing).frequency = existing.frequency.max(frequency);
            } else {
                let word = Arc::new(Word {
                    text: text.to_owned(),
                    frequency,
                });
                bucket.push(word);
                result.entries += 1;
            }
            let initials: String = fields[1]
                .split_ascii_whitespace()
                .filter_map(|syllable| syllable.chars().next())
                .collect();
            if initials.len() >= 2 {
                initial_keys
                    .entry(initials)
                    .or_default()
                    .push((key.clone(), text.to_owned()));
                let separated = fields[1]
                    .split_ascii_whitespace()
                    .collect::<Vec<_>>()
                    .join("'");
                separated_keys
                    .entry(separated)
                    .or_default()
                    .push((key, text.to_owned()));
            }
            if result.entries > MAX_ENTRIES {
                return Err("dictionary exceeds entry limit".into());
            }
        }
        if result.entries == 0 {
            return Err("dictionary has no entries".into());
        }
        for bucket in result.words.values_mut() {
            bucket.sort_by(|a, b| compare_words(a, b));
        }
        // Share canonical words only after all duplicate weights are resolved.
        result.initials = resolve_keys(initial_keys, &result.words);
        result.separated = resolve_keys(separated_keys, &result.words);
        for (key, words) in &result.separated {
            let spelling = key.replace('\'', " ");
            for word in words {
                result.mixed.insert(&spelling, word.clone(), word.frequency);
            }
        }
        result.completions = CompletionIndex::new(&result.words);
        Ok(result)
    }

    pub fn len(&self) -> usize {
        self.entries
    }
    pub fn is_empty(&self) -> bool {
        self.entries == 0
    }

    /// Bounded beam search over exact keys, indexed completions and syllable initials.
    /// It is an independent baseline; corpus-quality ranking is a later milestone.
    pub fn candidates(&self, input: &str) -> Vec<String> {
        self.candidates_with_personal(input, None)
    }
    pub(crate) fn candidates_with_personal(
        &self,
        input: &str,
        personal: Option<&crate::user_words::UserWords>,
    ) -> Vec<String> {
        let personal = personal.filter(|words| !words.is_empty());
        if input.is_empty()
            || input.len() > MAX_INPUT_BYTES
            || !input.bytes().all(|c| c.is_ascii_lowercase() || c == b'\'')
            || input.starts_with('\'')
            || input.ends_with('\'')
            || input.contains("''")
        {
            return Vec::new();
        }
        let mut beams: Vec<Vec<(f64, String)>> = vec![Vec::new(); input.len() + 1];
        beams[0].push((0.0, String::new()));
        for start in 0..input.len() {
            if beams[start].is_empty() {
                continue;
            }
            let paths = beams[start].clone();
            if input.as_bytes()[start] == b'\'' {
                if start > 0 && start + 1 < input.len() {
                    beams[start + 1].extend(paths);
                    trim_beam(&mut beams[start + 1]);
                }
                continue;
            }
            let mut separated = false;
            for end in start + 1..=input.len().min(start + MAX_KEY_BYTES) {
                separated |= input.as_bytes()[end - 1] == b'\'';
                let index = if separated {
                    &self.separated
                } else {
                    &self.words
                };
                let spelling = &input[start..end];
                let words = index.get(spelling);
                let extra = personal.map_or(&[][..], |model| model.exact_texts(spelling));
                if words.is_some() || !extra.is_empty() {
                    for (score, prefix) in &paths {
                        let local = extra
                            .iter()
                            .take(BEAM)
                            .map(|text| (text.as_str(), 1_000_000u32));
                        let base = words
                            .into_iter()
                            .flatten()
                            .take(BEAM)
                            .map(|word| (word.text.as_str(), word.frequency));
                        for (word, frequency) in local.chain(base) {
                            if prefix.len() + word.len() > MAX_TEXT_BYTES {
                                continue;
                            }
                            let mut text = prefix.clone();
                            text.push_str(word);
                            let next_score = score + (f64::from(frequency) + 1.0).ln() - 16.0;
                            beams[end].push((next_score, text));
                        }
                    }
                    trim_beam(&mut beams[end]);
                }
            }
        }
        let mut output = Vec::new();
        // "nhm" can otherwise become rare interjection fragments (n + hm).
        // Prefer whole-phrase initials when this is not a full-pinyin prefix.
        // Partial syllables such as "sh" retain ordinary pinyin completions.
        if !input.contains('\'') && !self.completions.has_prefix(input) {
            if let Some(words) = self.initials.get(input) {
                output.extend(words.iter().map(|word| word.text.clone()));
            }
        }
        for (_, text) in beams[input.len()].drain(..) {
            if output.len() == MAX_CANDIDATES {
                break;
            }
            if !output.contains(&text) {
                output.push(text);
            }
        }
        if input.contains('\'') {
            if let Some(words) = self.separated.get(input) {
                for word in words {
                    if output.len() == MAX_CANDIDATES {
                        break;
                    }
                    if !output.contains(&word.text) {
                        output.push(word.text.clone());
                    }
                }
            }
        }
        if !input.contains('\'') && output.len() < MAX_CANDIDATES {
            self.completions.append(input, &mut output);
            if let Some(words) = self.initials.get(input) {
                for word in words {
                    if output.len() == MAX_CANDIDATES {
                        break;
                    }
                    if !output.contains(&word.text) {
                        output.push(word.text.clone());
                    }
                }
            }
        }
        let mut mixed = self.mixed.matches(input);
        mixed.sort_by(|a, b| compare_words(a, b));
        let mut mixed_texts: Vec<_> = mixed.into_iter().map(|word| word.text.clone()).collect();
        if let Some(personal) = personal {
            let mut local = personal.mixed_texts(input);
            local.extend(mixed_texts);
            mixed_texts = local;
        }
        // Direct full spellings and established whole-phrase initials retain
        // priority. A valid mixed phrase outranks accidental fragment joins:
        // "diann" should find 电脑 before dian + the rare syllable n.
        let position = if self.words.contains_key(input)
            || self.separated.contains_key(input)
            || self.initials.contains_key(input)
        {
            PAGE_SIZE.min(output.len())
        } else {
            0
        };
        let mut tail = output.split_off(position);
        for text in mixed_texts {
            if output.len() == MAX_CANDIDATES {
                break;
            }
            if !output.contains(&text) {
                tail.retain(|word| word != &text);
                output.push(text);
            }
        }
        output.extend(tail);
        output.truncate(MAX_CANDIDATES);
        if let Some(personal) = personal {
            let extra = personal.supplements(input);
            // Exact base pinyin/ordinary prefixes keep their first page. True
            // initials precede accidental one-character fragment combinations.
            let position = if personal.is_initials(input) && !self.completions.has_prefix(input) {
                0
            } else {
                PAGE_SIZE.min(output.len())
            };
            let mut tail = output.split_off(position);
            for text in extra {
                if !output.contains(&text) {
                    tail.retain(|base| base != &text);
                    output.push(text);
                }
            }
            output.extend(tail);
            personal.apply(input, &mut output);
        }
        output.truncate(MAX_CANDIDATES);
        output
    }
}

fn trim_beam(paths: &mut Vec<(f64, String)>) {
    paths.sort_by(|a, b| b.0.total_cmp(&a.0).then_with(|| a.1.cmp(&b.1)));
    let mut seen = HashSet::new();
    paths.retain(|(_, text)| seen.insert(text.clone()));
    paths.truncate(BEAM);
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn phrase_and_independent_segmentation() {
        let dictionary = crate::prototype_dictionary();
        assert_eq!(dictionary.candidates("nihao")[0], "你好");
        assert_eq!(dictionary.candidates("nihaoshijie")[0], "你好世界");
        assert_eq!(dictionary.candidates("woshizhongguo")[0], "我是中国");
    }
    #[test]
    fn apostrophe_boundaries_and_unknown_input() {
        let dictionary = crate::prototype_dictionary();
        assert_eq!(dictionary.candidates("ni'hao")[0], "你好");
        assert!(dictionary.candidates("'ni").is_empty());
        assert!(dictionary.candidates("ni'").is_empty());
        assert!(dictionary.candidates("ni''hao").is_empty());
        assert!(dictionary.candidates("zzzz").is_empty());
        assert!(dictionary.candidates("你").is_empty());
        assert!(dictionary.candidates(&"n".repeat(129)).is_empty());
    }
    #[test]
    fn completions_are_unique_and_bounded() {
        let output = crate::prototype_dictionary().candidates("n");
        assert!(!output.is_empty());
        assert!(output.len() <= MAX_CANDIDATES);
        assert_eq!(output.iter().collect::<HashSet<_>>().len(), output.len());
    }
    #[test]
    fn rejects_malformed_or_empty_dictionary() {
        for source in [
            "",
            "x",
            "你\tNi\t1",
            "你\tni\t-1",
            "你\tni\tNaN",
            "你\t\t1",
            "你\tni\t1\textra",
        ] {
            assert!(Dictionary::from_tsv(source).is_err(), "{source}");
        }
    }
    #[test]
    fn duplicate_uses_maximum_weight() {
        let dictionary = Dictionary::from_tsv("你\tni\t10\n你\tni\t100\n泥\tni\t20").unwrap();
        assert_eq!(dictionary.len(), 2);
        assert_eq!(dictionary.candidates("ni")[0], "你");
    }

    #[test]
    fn prefix_completion_reaches_frequent_words_after_the_old_scan_limit() {
        let mut source = String::new();
        for i in 0..700 {
            let suffix: String = [i / (26 * 26), (i / 26) % 26, i % 26]
                .into_iter()
                .map(|part| char::from(b'a' + part as u8))
                .collect();
            source.push_str(&format!("候选{i}\tn{suffix}\t1\n"));
        }
        source.push_str("最后的高频词\tnzzzz\t1000000\n");
        let dictionary = Dictionary::from_tsv(&source).unwrap();
        let candidates = dictionary.candidates("n");
        assert_eq!(candidates[0], "最后的高频词");
        assert_eq!(candidates.len(), MAX_CANDIDATES);
        assert!(dictionary.candidates("z").is_empty());
    }

    #[test]
    fn initials_use_syllable_boundaries_and_resolved_duplicate_weights() {
        let dictionary = Dictionary::from_tsv(
            "你好\tni hao\t1\n你还\tni hai\t20\n你好\tni hao\t100\n输入法\tshu ru fa\t50\n西安\txi an\t10\n先\txian\t100",
        )
        .unwrap();
        assert_eq!(dictionary.candidates("nh")[0], "你好");
        assert_eq!(dictionary.candidates("srf"), ["输入法"]);
        assert_eq!(dictionary.candidates("xa"), ["西安"]);
        assert_eq!(dictionary.candidates("xi'an"), ["西安"]);
        assert!(dictionary.candidates("n'h").is_empty());
        assert_eq!(dictionary.candidates("srfa"), ["输入法"]);
    }

    #[test]
    fn direct_full_spelling_precedes_high_weight_mixed_competitors() {
        let dictionary =
            Dictionary::from_tsv("年\tnian\t100\n泥岸\tni an\t20\n你啊能\tni a neng\t1000000")
                .unwrap();
        assert_eq!(dictionary.candidates("nian")[0], "年");
        assert_eq!(dictionary.candidates("ni'an")[0], "泥岸");
        assert!(
            dictionary
                .candidates("nian")
                .iter()
                .any(|text| text == "你啊能")
        );
    }
    #[test]
    fn exact_pinyin_precedes_initials_and_polyphones_remain_unique() {
        let dictionary = Dictionary::from_tsv(
            "翻\tfan\t10\n方案呢\tfa an ne\t100000\n长大\tchang da\t100\n长大\tzhang da\t200\n张大\tzhang da\t50",
        )
        .unwrap();
        assert_eq!(dictionary.candidates("fan")[0], "翻");
        assert_eq!(dictionary.candidates("zd"), ["长大", "张大"]);
        let output = dictionary.candidates("z");
        assert_eq!(output.iter().filter(|word| *word == "长大").count(), 1);
    }
}
