// SPDX-License-Identifier: AGPL-3.0-only
//! Scheduling only. The platform transport owns HTTPS, cancellation and secrets.
use std::collections::VecDeque;

pub const STABLE_MS: u64 = 160;
pub const MAX_SOURCE_CHARS: usize = 200;
pub const MAX_REPLY_BYTES: usize = 4096;
pub const CACHE_CAPACITY: usize = 512;
pub const PROMPT_VERSION: u32 = 1;
pub const SYSTEM_PROMPT: &str = include_str!("../../../prompts/translation-system-v1.txt");

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Context {
    pub session: u64,
    pub revision: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Failure {
    Network,
    Timeout,
    Authentication,
    RateLimited,
    InvalidResponse,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Status {
    Disabled,
    Idle,
    Waiting,
    Running,
    Ready,
    Failed(Failure),
}

#[derive(Clone)]
pub struct Job {
    pub id: u64,
    pub context: Context,
    pub configuration: u64,
    epoch: u64,
    source: String,
}
impl Job {
    pub fn source(&self) -> &str {
        &self.source
    }
}
impl std::fmt::Debug for Job {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Job")
            .field("id", &self.id)
            .field("context", &self.context)
            .field("configuration", &self.configuration)
            .finish_non_exhaustive()
    }
}

struct Current {
    context: Context,
    source: String,
    ready_at: u64,
    dispatched: bool,
}
struct Cached {
    configuration: u64,
    source: String,
    translation: String,
}

pub struct Scheduler {
    enabled: bool,
    configuration: u64,
    epoch: u64,
    next_id: u64,
    current: Option<Current>,
    active: Option<Job>,
    cache: VecDeque<Cached>,
    visible: Option<String>,
    status: Status,
}

impl Default for Scheduler {
    fn default() -> Self {
        Self {
            enabled: false,
            configuration: 0,
            epoch: 0,
            next_id: 1,
            current: None,
            active: None,
            cache: VecDeque::new(),
            visible: None,
            status: Status::Disabled,
        }
    }
}

impl Scheduler {
    /// Identity must change whenever account, model, prompt or style changes.
    /// An active transport retains its slot until it acknowledges completion.
    pub fn configure(&mut self, identity: u64, enabled: bool) {
        if self.configuration == identity && self.enabled == enabled {
            return;
        }
        self.configuration = identity;
        self.epoch = self.epoch.wrapping_add(1);
        self.enabled = enabled;
        self.current = None;
        self.visible = None;
        self.cache.clear();
        self.status = if enabled {
            Status::Idle
        } else {
            Status::Disabled
        };
    }

    pub fn observe(&mut self, context: Context, source: &str, now_ms: u64) {
        if !self.enabled {
            return;
        }
        if !eligible(source) {
            self.clear_context();
            return;
        }
        if self
            .current
            .as_ref()
            .is_some_and(|current| current.context == context && current.source == source)
        {
            return;
        }
        self.current = Some(Current {
            context,
            source: source.to_owned(),
            ready_at: now_ms.saturating_add(STABLE_MS),
            dispatched: false,
        });
        self.visible = None;
        self.status = Status::Waiting;
    }

    pub fn clear_context(&mut self) {
        self.current = None;
        self.visible = None;
        self.status = if self.enabled {
            Status::Idle
        } else {
            Status::Disabled
        };
    }

    /// Returns at most one new task. Latest observation replaces pending work,
    /// never the in-flight transport or the actual input event stream.
    pub fn poll(&mut self, now_ms: u64) -> Option<Job> {
        if !self.enabled {
            return None;
        }
        let current = self.current.as_mut()?;
        if current.dispatched || now_ms < current.ready_at {
            return None;
        }
        if let Some(index) = self.cache.iter().position(|entry| {
            entry.configuration == self.configuration && entry.source == current.source
        }) {
            let cached = self.cache.remove(index).expect("position was valid");
            self.visible = Some(cached.translation.clone());
            self.cache.push_front(cached);
            current.dispatched = true;
            self.status = Status::Ready;
            return None;
        }
        if self.active.is_some() {
            return None;
        }
        let next_id = self.next_id.checked_add(1)?;
        let job = Job {
            id: self.next_id,
            context: current.context,
            configuration: self.configuration,
            epoch: self.epoch,
            source: current.source.clone(),
        };
        self.next_id = next_id;
        self.active = Some(job.clone());
        current.dispatched = true;
        self.status = Status::Running;
        Some(job)
    }

    /// The transport calls this only after it has actually stopped using its
    /// network resources. Timers alone cannot free the active task slot.
    pub fn finish(&mut self, id: u64, result: Result<String, Failure>) -> bool {
        if !self.active.as_ref().is_some_and(|job| job.id == id) {
            return false;
        }
        let job = self.active.take().expect("active ID was checked");
        if !self.enabled || job.configuration != self.configuration || job.epoch != self.epoch {
            return false;
        }
        let result = result.and_then(|text| {
            if text.trim().is_empty()
                || text.len() > MAX_REPLY_BYTES
                || text
                    .chars()
                    .any(|c| c.is_control() && c != '\n' && c != '\t')
            {
                Err(Failure::InvalidResponse)
            } else {
                Ok(text)
            }
        });
        if let Ok(text) = &result {
            self.cache.retain(|entry| {
                !(entry.configuration == job.configuration && entry.source == job.source)
            });
            self.cache.push_front(Cached {
                configuration: job.configuration,
                source: job.source.clone(),
                translation: text.clone(),
            });
            self.cache.truncate(CACHE_CAPACITY);
        }
        let belongs = self
            .current
            .as_ref()
            .is_some_and(|current| current.context == job.context && current.source == job.source);
        if !belongs {
            return false;
        }
        match result {
            Ok(text) => {
                self.visible = Some(text);
                self.status = Status::Ready;
            }
            Err(failure) => {
                self.visible = None;
                self.status = Status::Failed(failure);
            }
        }
        true
    }

    pub fn retry(&mut self, now_ms: u64) -> bool {
        if !self.enabled || self.active.is_some() {
            return false;
        }
        let Some(current) = self.current.as_mut() else {
            return false;
        };
        current.dispatched = false;
        current.ready_at = now_ms.saturating_add(STABLE_MS);
        self.visible = None;
        self.status = Status::Waiting;
        true
    }

    pub fn visible(&self) -> Option<&str> {
        self.visible.as_deref()
    }
    pub fn status(&self) -> Status {
        self.status
    }
    pub fn active_count(&self) -> usize {
        usize::from(self.active.is_some())
    }
    pub fn pending_count(&self) -> usize {
        usize::from(self.current.as_ref().is_some_and(|c| !c.dispatched))
    }
    pub fn cache_len(&self) -> usize {
        self.cache.len()
    }
    pub fn current_context(&self) -> Option<Context> {
        self.current.as_ref().map(|current| current.context)
    }
    pub fn configuration_id(&self) -> u64 {
        self.configuration
    }
    pub fn active_id(&self) -> u64 {
        self.active.as_ref().map_or(0, |job| job.id)
    }
    pub fn cancellation_requested(&self) -> bool {
        self.active.as_ref().is_some_and(|job| {
            !self.enabled
                || job.configuration != self.configuration
                || job.epoch != self.epoch
                || self
                    .current
                    .as_ref()
                    .is_none_or(|current| current.source != job.source)
        })
    }
    pub fn clear_if_current(&mut self, context: Context) -> bool {
        if self.current_context() != Some(context) {
            return false;
        }
        self.clear_context();
        true
    }
}

fn eligible(source: &str) -> bool {
    !source.is_empty()
        && source.chars().count() <= MAX_SOURCE_CHARS
        && !source
            .chars()
            .any(|c| c == '\0' || (c.is_control() && c != '\n' && c != '\t'))
        && source
            .chars()
            .any(|c| matches!(c as u32, 0x3400..=0x4dbf | 0x4e00..=0x9fff | 0x20000..=0x3134f))
}

#[cfg(test)]
mod tests {
    use super::*;
    fn context(revision: u64) -> Context {
        Context {
            session: 1,
            revision,
        }
    }
    fn scheduler() -> Scheduler {
        let mut s = Scheduler::default();
        s.configure(1, true);
        s
    }
    #[test]
    fn waits_for_stable_candidate_not_each_poll() {
        let mut s = scheduler();
        s.observe(context(1), "你好", 0);
        assert!(s.poll(159).is_none());
        s.observe(context(1), "你好", 159);
        let job = s.poll(160).unwrap();
        assert_eq!(job.source(), "你好");
        assert!(s.poll(1000).is_none());
        assert_eq!(s.active_count(), 1);
    }
    #[test]
    fn only_one_transport_and_one_latest_pending() {
        let mut s = scheduler();
        s.observe(context(1), "你", 0);
        let first = s.poll(160).unwrap();
        for revision in 2..1000 {
            s.observe(
                context(revision),
                &format!("测试{revision}"),
                revision * 200,
            );
            assert!(s.poll(revision * 200 + 160).is_none());
            assert_eq!(s.active_count(), 1);
            assert_eq!(s.pending_count(), 1);
        }
        assert!(!s.finish(first.id, Ok("you".into())));
        assert!(s.visible().is_none());
        let latest = s.poll(1_000_000).unwrap();
        assert_eq!(latest.source(), "测试999");
    }
    #[test]
    fn stale_context_same_text_cannot_receive_old_result() {
        let mut s = scheduler();
        s.observe(context(1), "你好", 0);
        let old = s.poll(160).unwrap();
        s.clear_context();
        s.observe(
            Context {
                session: 2,
                revision: 1,
            },
            "你好",
            200,
        );
        assert!(!s.finish(old.id, Ok("Hello".into())));
        assert!(s.visible().is_none());
        assert!(s.poll(360).is_none());
        assert_eq!(s.visible(), Some("Hello")); // explicit cache resolution in the new context
    }
    #[test]
    fn switching_configuration_keeps_network_slot_until_ack() {
        let mut s = scheduler();
        s.observe(context(1), "你好", 0);
        let old = s.poll(160).unwrap();
        s.configure(2, false);
        s.configure(3, true);
        s.observe(context(2), "中文", 200);
        assert!(s.poll(360).is_none());
        assert_eq!(s.active_count(), 1);
        assert!(!s.finish(old.id, Ok("old account".into())));
        assert_eq!(s.cache_len(), 0);
        assert_eq!(s.poll(400).unwrap().configuration, 3);
    }
    #[test]
    fn disable_enable_same_account_invalidates_old_transport_epoch() {
        let mut s = scheduler();
        s.observe(context(1), "你好", 0);
        let old = s.poll(160).unwrap();
        s.configure(1, false);
        s.configure(1, true);
        s.observe(context(1), "你好", 200);
        assert!(!s.finish(old.id, Ok("old result".into())));
        assert!(s.visible().is_none());
        assert_eq!(s.cache_len(), 0);
        assert!(s.poll(360).is_some());
    }
    #[test]
    fn wrong_completion_id_does_not_release_new_job() {
        let mut s = scheduler();
        s.observe(context(1), "你好", 0);
        let job = s.poll(160).unwrap();
        assert!(!s.finish(job.id + 1, Ok("bad".into())));
        assert_eq!(s.active_count(), 1);
        assert!(s.finish(job.id, Err(Failure::Timeout)));
        assert!(s.poll(10000).is_none());
        assert!(s.retry(10000));
        assert!(s.poll(10159).is_none());
        assert!(s.poll(10160).is_some());
    }
    #[test]
    fn filters_and_disabled_mode_send_nothing() {
        let mut s = scheduler();
        for text in ["", "hello", "123", "中\0文", &"中".repeat(201)] {
            s.observe(context(1), text, 0);
            assert!(s.poll(1000).is_none());
        }
        s.configure(2, false);
        s.observe(context(2), "中文", 0);
        assert!(s.poll(1000).is_none());
        assert_eq!(s.status(), Status::Disabled);
    }
    #[test]
    fn rejects_empty_and_oversized_replies() {
        for text in [
            " ".into(),
            "bad\0text".into(),
            "a".repeat(MAX_REPLY_BYTES + 1),
        ] {
            let mut s = scheduler();
            s.observe(context(1), "中文", 0);
            let job = s.poll(160).unwrap();
            assert!(s.finish(job.id, Ok(text)));
            assert_eq!(s.status(), Status::Failed(Failure::InvalidResponse));
            assert!(s.visible().is_none());
        }
    }
    #[test]
    fn bounded_cache_and_clear_on_config_change() {
        let mut s = scheduler();
        for revision in 0..600 {
            let now = revision * 200;
            s.observe(context(revision), &format!("中文{revision}"), now);
            let job = s.poll(now + 160).unwrap();
            s.finish(job.id, Ok(format!("translation {revision}")));
        }
        assert_eq!(s.cache_len(), CACHE_CAPACITY);
        s.configure(2, true);
        assert_eq!(s.cache_len(), 0);
        assert!(s.visible().is_none());
    }
}
