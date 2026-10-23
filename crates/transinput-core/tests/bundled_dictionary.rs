// SPDX-License-Identifier: AGPL-3.0-only
//! Integration checks use the shipped third-party data, not the small fixture.
use std::sync::{Arc, OnceLock};
use transinput_core::{Dictionary, Engine, Event, Outcome};

fn dictionary() -> Arc<Dictionary> {
    static DATA: OnceLock<Arc<Dictionary>> = OnceLock::new();
    DATA.get_or_init(|| Arc::new(transinput_core::bundled_dictionary()))
        .clone()
}

#[test]
fn pinned_data_and_representative_first_candidates() {
    let data = dictionary();
    assert_eq!(data.len(), 65_125);
    for (pinyin, expected) in [
        ("nihao", "你好"),
        ("zhongwen", "中文"),
        ("shurufa", "输入法"),
        ("fanyi", "翻译"),
        ("xiexie", "谢谢"),
        ("beijing", "北京"),
        ("shanghai", "上海"),
        ("jintian", "今天"),
        ("woshizhongguoren", "我是中国人"),
        ("nihaoshijie", "你好世界"),
    ] {
        let candidates = data.candidates(pinyin);
        assert_eq!(
            candidates.first().map(String::as_str),
            Some(expected),
            "{pinyin}"
        );
    }
}

#[test]
fn fixed_full_pinyin_initials_and_completion_quality() {
    let data = dictionary();
    let mut cases = 0;
    for line in include_str!("../fixtures/chinese-quality.tsv").lines() {
        if line.starts_with('#') || line.is_empty() {
            continue;
        }
        let fields: Vec<_> = line.split('\t').collect();
        assert_eq!(fields.len(), 3);
        let maximum: usize = fields[2].parse().unwrap();
        let output = data.candidates(fields[0]);
        let rank = output
            .iter()
            .position(|text| text == fields[1])
            .map(|index| index + 1);
        assert!(
            rank.is_some_and(|rank| rank <= maximum),
            "{} expected {} within {}, got {:?}",
            fields[0],
            fields[1],
            maximum,
            output
        );
        cases += 1;
    }
    assert_eq!(cases, 40);
}

#[test]
fn mixed_spelling_quality_and_repeated_edit_commit() {
    let data = dictionary();
    let mut count = 0;
    let mut engine = Engine::new(data.clone());
    for line in include_str!("../fixtures/mixed-spelling-quality.tsv").lines() {
        if line.starts_with('#') || line.is_empty() {
            continue;
        }
        let fields: Vec<_> = line.split('\t').collect();
        assert_eq!(fields.len(), 3);
        let expected_rank: usize = fields[2].parse().unwrap();
        let candidates = data.candidates(fields[0]);
        let rank = candidates.iter().position(|text| text == fields[1]);
        assert!(
            rank.is_some_and(|rank| rank < expected_rank),
            "{} -> {} got {:?}",
            fields[0],
            fields[1],
            candidates
        );
        assert_eq!(
            candidates
                .iter()
                .collect::<std::collections::HashSet<_>>()
                .len(),
            candidates.len()
        );
        for _ in 0..3 {
            assert!(engine.replace_composition(fields[0], fields[0].len()));
            engine.apply(Event::Left);
            engine.apply(Event::Right);
            engine.apply(Event::Letter('q'));
            engine.apply(Event::Backspace);
            let selected = engine
                .candidates()
                .iter()
                .position(|text| text == fields[1])
                .unwrap();
            assert_eq!(
                engine.apply(Event::Select(selected)),
                Outcome::Commit(fields[1].into())
            );
            assert_eq!(engine.apply(Event::Space), Outcome::PassThrough);
        }
        count += 1;
    }
    assert_eq!(count, 22);
}

#[test]
fn real_data_candidates_remain_bounded_for_long_and_invalid_input() {
    let data = dictionary();
    let mut inputs = vec![
        "n".to_owned(),
        "zh".to_owned(),
        "ni'hao".to_owned(),
        "a".repeat(128),
    ];
    inputs.extend([
        "你".to_owned(),
        "a".repeat(129),
        "'ni".to_owned(),
        "ni'".to_owned(),
    ]);
    for input in inputs {
        let candidates = data.candidates(&input);
        assert!(candidates.len() <= 90);
        for text in candidates {
            assert!(!text.is_empty() && text.len() <= 512);
            assert!(!text.chars().any(char::is_control));
        }
    }
    assert!(data.candidates("ni'").is_empty());
    assert!(data.candidates("'ni").is_empty());
}

#[test]
fn real_data_edits_commit_once_and_sessions_remain_independent() {
    let data = dictionary();
    let mut first = Engine::new(data.clone());
    let mut second = Engine::new(data);
    for c in "nihao".chars() {
        first.apply(Event::Letter(c));
    }
    for c in "zhongwen".chars() {
        second.apply(Event::Letter(c));
    }
    first.apply(Event::Left);
    first.apply(Event::Backspace);
    first.apply(Event::Letter('a'));
    assert_eq!(first.apply(Event::Space), Outcome::Commit("你好".into()));
    assert_eq!(first.apply(Event::Space), Outcome::PassThrough);
    assert_eq!(second.preedit(), "zhongwen");
    assert_eq!(second.apply(Event::Space), Outcome::Commit("中文".into()));
}

#[test]
fn pages_cover_bounded_candidates_without_duplicates_and_reset_on_edit() {
    let data = dictionary();
    for input in ["n", "bj", "xi'an"] {
        let expected = data.candidates(input);
        let mut engine = Engine::new(data.clone());
        assert!(engine.replace_composition(input, input.len()));
        assert_eq!(engine.candidate_total(), expected.len());
        let pages = expected.len().div_ceil(9);
        let mut actual = Vec::new();
        for page in 0..pages {
            engine.set_page(page);
            assert_eq!(engine.page(), page);
            assert!(engine.candidates().len() <= 9);
            actual.extend_from_slice(engine.candidates());
        }
        assert_eq!(actual, expected);
        assert_eq!(
            actual
                .iter()
                .collect::<std::collections::HashSet<_>>()
                .len(),
            actual.len()
        );
        engine.set_page(usize::MAX);
        assert_eq!(engine.page(), pages.saturating_sub(1));
        let first = engine.candidates()[0].clone();
        assert_eq!(engine.apply(Event::Select(9)), Outcome::Updated);
        assert_eq!(engine.apply(Event::Select(0)), Outcome::Commit(first));
        assert_eq!(engine.page(), 0);
        assert!(engine.replace_composition("n", 1));
        engine.apply(Event::PageDown);
        assert_eq!(engine.page(), 1);
        engine.apply(Event::Letter('i'));
        assert_eq!(engine.page(), 0);
    }
}
