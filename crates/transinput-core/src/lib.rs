// SPDX-License-Identifier: AGPL-3.0-only
//! Platform-neutral input state. No network, global hooks, or host UI objects.

pub mod dictionary;
pub mod engine;
pub mod ffi;
pub mod learning;
pub mod learning_ffi;
mod spelling;
pub mod translation;
pub mod translation_ffi;
pub mod user_words;
pub mod user_words_ffi;

pub use dictionary::Dictionary;
pub use engine::{Engine, Event, Outcome};

/// Reviewed Apache-2.0 word data, converted by our own script. No Rime engine.
/// Keep upstream LICENSE/NOTICE and sources/pinyin-simp provenance with releases.
pub fn bundled_dictionary() -> Dictionary {
    Dictionary::from_tsv(include_str!("../data/base.tsv"))
        .expect("the checked-in verified base dictionary must be valid")
}

/// Authored test vocabulary, not a production dictionary or an offline translator.
#[cfg(test)]
pub fn prototype_dictionary() -> Dictionary {
    Dictionary::from_tsv(include_str!("../fixtures/prototype.tsv"))
        .expect("the checked-in prototype vocabulary must be valid")
}
