// SPDX-License-Identifier: AGPL-3.0-only
//! Syllable trie: each explicit syllable accepts its full spelling or initial.
//! No exponential enumeration of abbreviated keys and no per-query corpus scan.
use std::{
    cmp::Reverse,
    collections::{BTreeMap, BinaryHeap, HashSet},
};

const MAX_EDGE_VISITS: usize = 8192;
const MAX_MATCH_VISITS: usize = 512;

#[derive(Clone, Debug)]
struct Node<T> {
    children: BTreeMap<String, usize>,
    terms: Vec<(u32, T)>,
    best: u32,
}
impl<T> Default for Node<T> {
    fn default() -> Self {
        Self {
            children: BTreeMap::new(),
            terms: Vec::new(),
            best: 0,
        }
    }
}

#[derive(Clone, Debug)]
pub(crate) struct MixedIndex<T> {
    nodes: Vec<Node<T>>,
}
impl<T> Default for MixedIndex<T> {
    fn default() -> Self {
        Self {
            nodes: vec![Node::default()],
        }
    }
}
impl<T: Clone + Eq> MixedIndex<T> {
    pub(crate) fn insert(&mut self, pinyin: &str, text: T, weight: u32) {
        let syllables: Vec<_> = pinyin.split_ascii_whitespace().collect();
        if syllables.len() < 2 {
            return;
        }
        let mut node = 0;
        self.nodes[node].best = self.nodes[node].best.max(weight);
        for syllable in syllables {
            let next = if let Some(next) = self.nodes[node].children.get(syllable) {
                *next
            } else {
                let next = self.nodes.len();
                self.nodes.push(Node::default());
                self.nodes[node].children.insert(syllable.to_owned(), next);
                next
            };
            node = next;
            self.nodes[node].best = self.nodes[node].best.max(weight);
        }
        if !self.nodes[node].terms.iter().any(|(_, term)| term == &text) {
            self.nodes[node].terms.push((weight, text));
        }
    }
    pub(crate) fn matches(&self, input: &str) -> Vec<T> {
        self.search(input).0
    }

    fn search(&self, input: &str) -> (Vec<T>, usize, bool) {
        if input.len() < 2
            || input.len() > 128
            || !input.bytes().all(|c| c.is_ascii_lowercase() || c == b'\'')
            || input.starts_with('\'')
            || input.ends_with('\'')
            || input.contains("''")
        {
            return (Vec::new(), 0, false);
        }
        let mut pending = BinaryHeap::from([(self.nodes[0].best, Reverse((0usize, 0usize, 0u8)))]);
        let mut seen = HashSet::from([(0usize, 0usize, 0u8)]);
        let mut matches = Vec::new();
        let (mut visits, mut terms) = (0, 0);
        let mut exhausted = false;
        'search: while let Some((_, Reverse((node, position, flags)))) = pending.pop() {
            if position == input.len() {
                // Require both a full multi-letter syllable and an abbreviation;
                // existing full-spelling and pure-initial paths retain their order.
                if flags == 3 {
                    for (weight, text) in &self.nodes[node].terms {
                        if terms == MAX_MATCH_VISITS {
                            exhausted = true;
                            break 'search;
                        }
                        terms += 1;
                        if !matches.iter().any(|(_, value)| value == text) {
                            matches.push((*weight, text.clone()));
                        }
                    }
                }
                continue;
            }
            let first = input.as_bytes()[position];
            let lower = char::from(first).to_string();
            let upper = char::from(first + 1).to_string();
            for (syllable, next) in self.nodes[node].children.range(lower..upper) {
                if visits == MAX_EDGE_VISITS {
                    exhausted = true;
                    break 'search;
                }
                visits += 1;
                let mut push = |end: usize, flags| {
                    let end = if input.as_bytes().get(end) == Some(&b'\'') {
                        end + 1
                    } else {
                        end
                    };
                    let state = (*next, end, flags);
                    if seen.insert(state) {
                        pending.push((self.nodes[*next].best, Reverse(state)));
                    }
                };
                if input[position..].starts_with(syllable) {
                    push(
                        position + syllable.len(),
                        flags | u8::from(syllable.len() > 1),
                    );
                }
                if syllable.len() > 1 {
                    push(position + 1, flags | 2);
                }
                if syllable.len() > 2
                    && ["zh", "ch", "sh"]
                        .iter()
                        .any(|initial| syllable.starts_with(initial))
                    && input[position..].starts_with(&syllable[..2])
                {
                    push(position + 2, flags | 2);
                }
            }
        }
        matches.sort_by_key(|(weight, _)| Reverse(*weight));
        (
            matches.into_iter().map(|(_, value)| value).collect(),
            visits,
            exhausted,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn exact_syllables_or_initials_with_explicit_boundaries() {
        let mut index = MixedIndex::default();
        index.insert("shu ru fa", "输入法", 10);
        for input in [
            "shuruf", "shurfa", "srufa", "shurf", "srfa", "shu'r'f", "s'ru'fa",
        ] {
            assert_eq!(index.matches(input), ["输入法"], "{input}");
        }
        for input in [
            "srf", "shurufa", "shurufax", "shur", "srrfa", "s''ru'fa", "shu'r'", "你",
        ] {
            assert!(index.matches(input).is_empty(), "{input}");
        }
    }
    #[test]
    fn frequency_priority_and_query_work_are_bounded() {
        let mut index = MixedIndex::default();
        for n in 0..2000 {
            let suffix = format!(
                "n{}{}{}",
                char::from(b'a' + (n / 676) as u8),
                char::from(b'a' + (n / 26 % 26) as u8),
                char::from(b'a' + (n % 26) as u8)
            );
            index.insert(&format!("ni {suffix}"), n, n as u32);
        }
        let (output, visits, _) = index.search("nin");
        assert_eq!(output[0], 1999);
        assert!(output.len() <= MAX_MATCH_VISITS && visits <= MAX_EDGE_VISITS);
        let mut branching = MixedIndex::default();
        for n in 0..600 {
            branching.insert(
                &format!(
                    "na n{}{} {}",
                    char::from(b'a' + (n / 26) as u8),
                    char::from(b'a' + (n % 26) as u8),
                    ["na"; 28].join(" ")
                ),
                n,
                1,
            );
        }
        let (_, visits, exhausted) = branching.search(&"n".repeat(30));
        assert_eq!(visits, MAX_EDGE_VISITS);
        assert!(exhausted);
    }
}
