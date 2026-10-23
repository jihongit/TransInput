// SPDX-License-Identifier: AGPL-3.0-only
use crate::dictionary::{Dictionary, MAX_INPUT_BYTES, PAGE_SIZE};
use std::sync::{Arc, Mutex};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Event {
    Letter(char),
    Backspace,
    Delete,
    Left,
    Right,
    Up,
    Down,
    PageUp,
    PageDown,
    Enter,
    Space,
    Escape,
    Select(usize),
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Outcome {
    PassThrough,
    Updated,
    Commit(String),
    Cancelled,
    InputLimit,
}

#[derive(Debug)]
pub struct Engine {
    dictionary: Arc<Dictionary>,
    learning: Option<Arc<Mutex<crate::learning::Learning>>>,
    user_words: Option<crate::user_words_ffi::SharedWords>,
    preedit: String,
    cursor: usize,
    candidates: Vec<String>,
    selected: usize,
    page: usize,
    revision: u64,
}

impl Engine {
    pub fn new(dictionary: Arc<Dictionary>) -> Self {
        Self {
            dictionary,
            learning: None,
            user_words: None,
            preedit: String::new(),
            cursor: 0,
            candidates: Vec::new(),
            selected: 0,
            page: 0,
            revision: 0,
        }
    }
    pub fn preedit(&self) -> &str {
        &self.preedit
    }
    pub fn set_learning(&mut self, learning: Option<Arc<Mutex<crate::learning::Learning>>>) {
        self.learning = learning;
    }
    pub(crate) fn set_user_words(&mut self, words: Option<crate::user_words_ffi::SharedWords>) {
        self.user_words = words;
    }
    pub fn cursor(&self) -> usize {
        self.cursor
    }
    pub fn candidates(&self) -> &[String] {
        let begin = self.page * PAGE_SIZE;
        &self.candidates[begin..(begin + PAGE_SIZE).min(self.candidates.len())]
    }
    pub fn candidate_total(&self) -> usize {
        self.candidates.len()
    }
    pub fn page(&self) -> usize {
        self.page
    }
    pub fn set_page(&mut self, page: usize) {
        let page = page.min(self.candidates.len().saturating_sub(1) / PAGE_SIZE);
        if page != self.page {
            self.page = page;
            self.selected = 0;
            self.bump();
        }
    }
    pub fn selected(&self) -> usize {
        self.selected
    }
    pub fn revision(&self) -> u64 {
        self.revision
    }
    pub fn composing(&self) -> bool {
        !self.preedit.is_empty()
    }

    pub fn reset(&mut self) {
        self.preedit.clear();
        self.cursor = 0;
        self.candidates.clear();
        self.selected = 0;
        self.page = 0;
        self.revision = self.revision.wrapping_add(1);
    }

    /// Replace a complete ordered platform snapshot; recompute once rather than
    /// replaying every prefix across the process boundary. Invalid input is inert.
    pub fn replace_composition(&mut self, source: &str, cursor: usize) -> bool {
        if source.len() > MAX_INPUT_BYTES
            || cursor > source.len()
            || !source
                .bytes()
                .all(|c| c.is_ascii_alphabetic() || c == b'\'')
        {
            return false;
        }
        self.preedit = source.to_ascii_lowercase();
        self.cursor = cursor;
        self.recompute();
        true
    }

    pub fn apply(&mut self, event: Event) -> Outcome {
        match event {
            Event::Letter(c) if c.is_ascii_alphabetic() || c == '\'' => {
                if self.preedit.len() == MAX_INPUT_BYTES {
                    return Outcome::InputLimit;
                }
                if c == '\'' && !self.composing() {
                    return Outcome::PassThrough;
                }
                self.preedit.insert(self.cursor, c.to_ascii_lowercase());
                self.cursor += 1;
                self.recompute();
                Outcome::Updated
            }
            Event::Letter(_) => Outcome::PassThrough,
            _ if !self.composing() => Outcome::PassThrough,
            Event::Backspace => {
                if self.cursor > 0 {
                    self.cursor -= 1;
                    self.preedit.remove(self.cursor);
                    self.recompute();
                }
                Outcome::Updated
            }
            Event::Delete => {
                if self.cursor < self.preedit.len() {
                    self.preedit.remove(self.cursor);
                    self.recompute();
                }
                Outcome::Updated
            }
            Event::Left => {
                self.cursor = self.cursor.saturating_sub(1);
                Outcome::Updated
            }
            Event::Right => {
                self.cursor = (self.cursor + 1).min(self.preedit.len());
                Outcome::Updated
            }
            Event::Up => {
                self.selected = self.selected.saturating_sub(1);
                self.bump();
                Outcome::Updated
            }
            Event::Down => {
                if !self.candidates().is_empty() {
                    self.selected = (self.selected + 1).min(self.candidates().len() - 1);
                }
                self.bump();
                Outcome::Updated
            }
            Event::PageUp => {
                self.set_page(self.page.saturating_sub(1));
                Outcome::Updated
            }
            Event::PageDown => {
                self.set_page(self.page.saturating_add(1));
                Outcome::Updated
            }
            Event::Enter => {
                let text = self.preedit.clone();
                self.reset();
                Outcome::Commit(text)
            }
            Event::Space => self.commit(self.selected),
            Event::Select(index) => self.commit(index),
            Event::Escape => {
                self.reset();
                Outcome::Cancelled
            }
        }
    }

    fn commit(&mut self, index: usize) -> Outcome {
        if let Some(text) = self.candidates().get(index).cloned() {
            self.reset();
            Outcome::Commit(text)
        } else {
            Outcome::Updated
        }
    }
    fn bump(&mut self) {
        self.revision = self.revision.wrapping_add(1);
    }
    fn recompute(&mut self) {
        let personal = self
            .user_words
            .as_ref()
            .and_then(|words| words.lock().ok().map(|current| current.clone()));
        self.candidates = self
            .dictionary
            .candidates_with_personal(&self.preedit, personal.as_deref());
        if let Some(learning) = &self.learning {
            if let Ok(model) = learning.lock() {
                model.rank(&self.preedit, &mut self.candidates);
            }
        }
        self.selected = 0;
        self.page = 0;
        self.bump();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn engine() -> Engine {
        Engine::new(Arc::new(crate::prototype_dictionary()))
    }
    #[test]
    fn personal_initials_completion_learning_and_removal() {
        let mut words = crate::user_words::UserWords::default();
        words.add("xing he yan jiu suo", "星河研究所").unwrap();
        words.add("xing he yan jiu suo", "星河研修所").unwrap();
        words.add("ni hao peng you", "你好朋友").unwrap();
        // Without syllable spaces we can complete, but must not guess initials.
        words.add("yunduogongzuoshi", "云朵工作室").unwrap();
        let shared = Arc::new(Mutex::new(Arc::new(words)));
        let learning = Arc::new(Mutex::new(crate::learning::Learning::default()));
        let mut typed = engine();
        typed.set_user_words(Some(shared.clone()));
        typed.set_learning(Some(learning.clone()));
        for input in [
            "xhyjs",
            "xinghe",
            "xingheyanjiusuo",
            "xing'he'yan'jiu'suo",
            "xinghyjs",
            "xhyanjs",
            "xing'h'yan'j's",
        ] {
            typed.replace_composition(input, input.len());
            assert!(
                typed.candidates.iter().any(|word| word == "星河研修所"),
                "{input}"
            );
            assert!(learning.lock().unwrap().record(input, "星河研修所"));
            typed.replace_composition(input, input.len());
            assert_eq!(typed.candidates()[0], "星河研修所", "{input}");
        }
        for input in ["n", "ni"] {
            let base = crate::prototype_dictionary().candidates(input);
            typed.replace_composition(input, input.len());
            assert_eq!(
                &typed.candidates[..base.len().min(9)],
                &base[..base.len().min(9)]
            );
        }
        typed.replace_composition("nhpy", 4);
        assert_eq!(typed.candidates()[0], "你好朋友");
        typed.replace_composition("ydgzs", 5);
        assert!(!typed.candidates.iter().any(|word| word == "云朵工作室"));
        let saved = learning.lock().unwrap().export();
        *learning.lock().unwrap() = crate::learning::Learning::from_bytes(&saved).unwrap();
        typed.replace_composition("xhyjs", 5);
        assert_eq!(typed.candidates()[0], "星河研修所");
        *shared.lock().unwrap() = Arc::new(crate::user_words::UserWords::default());
        typed.replace_composition("xhyjs", 5);
        assert!(!typed.candidates.iter().any(|word| word == "星河研修所"));
    }
    #[test]
    fn personal_words_compose_with_base_without_changing_other_profiles() {
        let mut words = crate::user_words::UserWords::default();
        words.add("xing he yan jiu suo", "星河研究所").unwrap();
        let shared = Arc::new(Mutex::new(Arc::new(words)));
        let mut with_words = engine();
        let mut without_words = engine();
        with_words.set_user_words(Some(shared.clone()));
        for input in [
            "xingheyanjiusuo",
            "nihaoxingheyanjiusuo",
            "xingheyanjiusuonihao",
        ] {
            assert!(with_words.replace_composition(input, input.len()));
            let expected = if input.starts_with("nihao") {
                "你好星河研究所"
            } else if input.ends_with("nihao") {
                "星河研究所你好"
            } else {
                "星河研究所"
            };
            assert_eq!(with_words.candidates()[0], expected);
            assert!(without_words.replace_composition(input, input.len()));
            assert!(!without_words.candidates().contains(&expected.to_owned()));
        }
        *shared.lock().unwrap() = Arc::new(crate::user_words::UserWords::default());
        with_words.replace_composition("xingheyanjiusuo", 15);
        assert!(!with_words.candidates().contains(&"星河研究所".to_owned()));
    }
    #[test]
    fn personal_homophones_page_and_deduplicate_without_displacing_short_input() {
        let mut words = crate::user_words::UserWords::default();
        for i in 0..32 {
            words.add("ni hao", &format!("你好词{i:02}")).unwrap();
        }
        words.add("ni hao", "你好").unwrap_err(); // per-spelling limit, no eviction
        let shared = Arc::new(Mutex::new(Arc::new(words)));
        let mut typed = engine();
        typed.set_user_words(Some(shared));
        typed.replace_composition("nihao", 5);
        assert!(typed.candidate_total() >= 32);
        typed.set_page(2);
        assert_eq!(typed.candidates()[0], "你好词18");
        typed.replace_composition("n", 1);
        let expected = crate::prototype_dictionary().candidates("n");
        assert_eq!(typed.candidates(), &expected[..expected.len().min(9)]);
    }
    #[test]
    fn complete_snapshot_query_is_transactional_and_does_not_commit() {
        let mut engine = engine();
        assert!(engine.replace_composition("NIHAO", 3));
        assert_eq!(engine.preedit(), "nihao");
        assert_eq!(engine.cursor(), 3);
        assert_eq!(engine.candidates()[0], "你好");
        let revision = engine.revision();
        for (source, cursor) in [("你", 0), ("nihao", 6), ("ni\0hao", 2)] {
            assert!(!engine.replace_composition(source, cursor));
            assert_eq!(engine.preedit(), "nihao");
            assert_eq!(engine.revision(), revision);
        }
        assert_eq!(engine.apply(Event::Space), Outcome::Commit("你好".into()));
    }
    fn type_text(engine: &mut Engine, text: &str) {
        for c in text.chars() {
            assert_eq!(engine.apply(Event::Letter(c)), Outcome::Updated);
        }
    }
    #[test]
    fn commit_clears_once_and_preserves_selected_candidate() {
        let mut engine = engine();
        type_text(&mut engine, "ni");
        engine.apply(Event::Down);
        assert_eq!(engine.apply(Event::Space), Outcome::Commit("呢".into()));
        assert_eq!(engine.apply(Event::Space), Outcome::PassThrough);
        assert!(!engine.composing());
    }
    #[test]
    fn middle_edit_is_not_an_append() {
        let mut engine = engine();
        type_text(&mut engine, "nihao");
        engine.apply(Event::Left);
        engine.apply(Event::Left);
        engine.apply(Event::Backspace);
        assert_eq!(engine.preedit(), "niao");
        engine.apply(Event::Letter('h'));
        assert_eq!(engine.preedit(), "nihao");
        assert_eq!(engine.candidates()[0], "你好");
    }
    #[test]
    fn cancel_and_context_reset_remove_all_state() {
        let mut engine = engine();
        type_text(&mut engine, "nihao");
        let previous = engine.revision();
        assert_eq!(engine.apply(Event::Escape), Outcome::Cancelled);
        assert!(engine.preedit().is_empty());
        assert!(engine.candidates().is_empty());
        assert_ne!(previous, engine.revision());
        type_text(&mut engine, "zhongwen");
        engine.reset();
        assert_eq!(engine.cursor(), 0);
        assert_eq!(engine.apply(Event::Select(0)), Outcome::PassThrough);
    }
    #[test]
    fn unknown_pinyin_can_be_committed_as_literal() {
        let mut engine = engine();
        type_text(&mut engine, "zzzz");
        assert!(engine.candidates().is_empty());
        assert_eq!(engine.apply(Event::Enter), Outcome::Commit("zzzz".into()));
    }
    #[test]
    fn input_and_selection_limits_do_not_panic() {
        let mut engine = engine();
        type_text(&mut engine, &"n".repeat(MAX_INPUT_BYTES));
        assert_eq!(engine.apply(Event::Letter('a')), Outcome::InputLimit);
        assert_eq!(engine.preedit().len(), MAX_INPUT_BYTES);
        assert_eq!(engine.apply(Event::Select(usize::MAX)), Outcome::Updated);
        assert_eq!(engine.apply(Event::Letter('你')), Outcome::PassThrough);
    }
    #[test]
    fn independent_instances_do_not_share_composition() {
        let mut a = engine();
        let mut b = engine();
        type_text(&mut a, "nihao");
        type_text(&mut b, "zhongwen");
        assert_eq!(a.apply(Event::Space), Outcome::Commit("你好".into()));
        assert_eq!(b.apply(Event::Space), Outcome::Commit("中文".into()));
    }
}
