// SPDX-License-Identifier: AGPL-3.0-only
use std::{
    io::{self, BufRead},
    sync::Arc,
    time::Instant,
};
use transinput_core::{Engine, Event};

fn main() {
    let started = Instant::now();
    let dictionary = Arc::new(transinput_core::bundled_dictionary());
    let load_us = started.elapsed().as_micros();
    let entries = dictionary.len();
    let mut engine = Engine::new(dictionary);
    let arguments: Vec<String> = std::env::args().skip(1).collect();
    if let Some(input) = arguments.first() {
        if input == "--benchmark" || input == "--benchmark-spelling" {
            benchmark(
                &mut engine,
                load_us,
                entries,
                input == "--benchmark-spelling",
            );
            return;
        }
        for c in input.chars() {
            engine.apply(Event::Letter(c));
        }
        for (index, text) in engine.candidates().iter().enumerate() {
            println!("{}\t{}", index + 1, text);
        }
        return;
    }
    println!(
        "TransInput core preview: {entries} bundled word entries; no translation or system installation."
    );
    println!("Enter pinyin lines; :clear, :space, :backspace, :quit.");
    for line in io::stdin().lock().lines() {
        let Ok(line) = line else { break };
        match line.as_str() {
            ":quit" => break,
            ":clear" => engine.reset(),
            ":space" => println!("{:?}", engine.apply(Event::Space)),
            ":backspace" => {
                engine.apply(Event::Backspace);
            }
            _ => {
                for c in line.chars() {
                    engine.apply(Event::Letter(c));
                }
            }
        }
        println!(
            "preedit={} candidates={:?}",
            engine.preedit(),
            engine.candidates()
        );
    }
}

fn benchmark(engine: &mut Engine, load_us: u128, entries: usize, mixed: bool) {
    let inputs: &[&str] = if mixed {
        &[
            "zhongg",
            "zhguo",
            "bjing",
            "shuruf",
            "shurfa",
            "srufa",
            "srfa",
            "rengongzn",
            "rgongzhineng",
            "dnao",
            "jtian",
            "mguanxi",
            "dbuqi",
            "zhong'guo'r",
            "shu'ru'f",
            "ni'h",
        ]
    } else {
        &[
            "nihao",
            "zhongwen",
            "shurufa",
            "fanyi",
            "xiexie",
            "beijing",
            "shanghai",
            "jintian",
            "woshizhongguoren",
            "nihaoshijie",
            "nh",
            "nhm",
            "srf",
            "rgzn",
            "bj",
            "zg",
            "xi'an",
            "zhongg",
        ]
    };
    let mut times = Vec::new();
    let started = Instant::now();
    let mut commits = 0;
    for _ in 0..100 {
        for input in inputs {
            engine.reset();
            for c in input.chars() {
                let key_started = Instant::now();
                std::hint::black_box(engine.apply(Event::Letter(c)));
                times.push(key_started.elapsed().as_nanos());
            }
            if matches!(
                engine.apply(Event::Space),
                transinput_core::Outcome::Commit(_)
            ) {
                commits += 1;
            }
        }
    }
    let elapsed_us = started.elapsed().as_micros();
    let compositions = inputs.len() * 100;
    times.sort_unstable();
    let percentile = |percentage: usize| times[(times.len() - 1) * percentage / 100];
    println!(
        "{{\"entries\":{entries},\"cold_load_us\":{load_us},\"compositions\":{compositions},\"commits\":{commits},\"letter_events\":{},\"elapsed_us\":{elapsed_us},\"p50_ns\":{},\"p95_ns\":{},\"p99_ns\":{},\"max_ns\":{}}}",
        times.len(),
        percentile(50),
        percentile(95),
        percentile(99),
        times[times.len() - 1]
    );
}
