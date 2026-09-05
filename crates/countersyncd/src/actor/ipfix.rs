use std::{
    collections::VecDeque,
    error::Error,
    fmt::{Display, Formatter},
    sync::Arc,
    time::Duration,
};

use ahash::{HashMap, HashMapExt, HashSet, HashSetExt};
use byteorder::{ByteOrder, NetworkEndian};
use log::{debug, error, warn};
use tokio::{
    select,
    sync::mpsc::{Receiver, Sender},
    time::{sleep_until, Instant},
};

use super::super::message::{
    buffer::SocketBufferMessage,
    ipfix::{IPFixTemplateOperation, IPFixTemplatesMessage},
    saistats::{decode_sai_ids, SAIStat, SAIStatsBatch, SAIStatsBatchMessage},
};
use crate::utilities::{record_comm_stats, ChannelLabel};

const IPFIX_VERSION: u16 = 10;
const IPFIX_HEADER_LEN: usize = 16;
const SET_HEADER_LEN: usize = 4;
const TEMPLATE_SET_ID: u16 = 2;
const MIN_DATA_SET_ID: u16 = 256;
const OBSERVATION_TIME_NANOSECONDS: u16 = 325;
const OBSERVATION_TIME_LEN: u16 = 8;
const MIN_HFT_TEMPLATE_RECORD_LEN: usize = 16;
const MAX_UNKNOWN_SETS: usize = 256;
const MAX_UNKNOWN_SET_BYTES: usize = 4 * 1024 * 1024;
const UNKNOWN_SET_TTL: Duration = Duration::from_secs(5);
const MAX_INSTALLED_TEMPLATE_KEYS: usize = 4096;
const MAX_LIVE_TEMPLATE_BYTES: usize = 64 * 1024 * 1024;
const MAX_TRACKED_OBSERVATION_DOMAINS: usize = 256;
const TEMPLATE_ID_HISTORY_WORDS: usize = 1024;
const MAX_DELETED_SESSION_MARKERS: usize = 4096;
const MAX_DELETED_SESSION_MARKER_BYTES: usize = 1024 * 1024;
const MAX_TEMPLATE_CONFIG_BYTES: usize = 4 * 1024 * 1024;
const MAX_TEMPLATES_PER_UPDATE: usize = 1024;
const MAX_DATA_SETS_PER_RECORD_INPUT: usize = 4096;
const MAX_UNRESOLVED_SETS_PER_RECORD_INPUT: usize = MAX_UNKNOWN_SETS;
const MAX_RECORD_INPUTS_PER_BATCH: usize = 64;
const MAX_RECORD_INPUT_BYTES_PER_BATCH: usize = 4 * 1024 * 1024;
const MAX_COUNTERS_PER_BATCH: usize = 8192;
const MAX_OBJECT_METADATA_BYTES: usize = 4 * 1024 * 1024;
const MAX_OBJECTS_PER_UPDATE: usize = 32_767;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
struct TemplateKey {
    observation_domain_id: u32,
    template_id: u16,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct CompiledCounter {
    offset: usize,
    len: u8,
    object_name: Arc<str>,
    type_id: u32,
    stat_id: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct CompiledTemplate {
    key: TemplateKey,
    owner: Arc<str>,
    observation_time_offset: usize,
    counters: Arc<[CompiledCounter]>,
    record_len: usize,
}

#[derive(Debug, Clone, Copy)]
struct DataSetLayout {
    record_bytes: usize,
    record_count: usize,
    counter_count: usize,
}

#[derive(Debug)]
struct ValidatedDataSet<'a> {
    key: TemplateKey,
    bytes: &'a [u8],
    template: Option<Arc<CompiledTemplate>>,
    layout: Option<DataSetLayout>,
    enabled: bool,
    retired: bool,
    domain_blocked: bool,
}

#[derive(Debug)]
struct ValidatedDataMessage<'a> {
    sets: Vec<ValidatedDataSet<'a>>,
    counter_count: usize,
}

#[derive(Debug)]
struct ValidatedRecordInput<'a> {
    messages: Vec<ValidatedDataMessage<'a>>,
}

#[derive(Debug)]
struct PendingUpdatePlan {
    cutover_keys: HashSet<TemplateKey>,
    retire_keys: HashSet<TemplateKey>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct TemplateGeneration {
    owner: Arc<str>,
    templates: HashMap<TemplateKey, Arc<CompiledTemplate>>,
    compiled_bytes: usize,
}

impl TemplateGeneration {
    fn equivalent(&self, other: &Self) -> bool {
        self.templates == other.templates
    }
}

#[derive(Debug, Default)]
struct SessionTemplates {
    active: Option<TemplateGeneration>,
    pending: Option<TemplateGeneration>,
    pending_cutover_keys: HashSet<TemplateKey>,
    enabled: bool,
    deferred_floor: u64,
}

#[derive(Debug, Clone)]
struct BufferedSet {
    key: TemplateKey,
    bytes: Arc<[u8]>,
    sequence: u64,
    expires_at: Option<Instant>,
}

#[derive(Debug, Default)]
struct DeferredSetBuffer {
    sets: VecDeque<BufferedSet>,
    bytes: usize,
    dropped: u64,
}

impl DeferredSetBuffer {
    fn expire(&mut self, now: Instant) -> u64 {
        let dropped_before = self.dropped;
        let mut index = 0usize;
        while index < self.sets.len() {
            if self.sets[index]
                .expires_at
                .is_some_and(|deadline| now >= deadline)
            {
                self.remove(index).expect("index checked above");
                self.dropped = self.dropped.saturating_add(1);
            } else {
                index += 1;
            }
        }
        self.dropped - dropped_before
    }

    fn push(&mut self, set: BufferedSet) {
        let len = set.bytes.len();
        if len > MAX_UNKNOWN_SET_BYTES
            || self.sets.len() >= MAX_UNKNOWN_SETS
            || self.bytes.saturating_add(len) > MAX_UNKNOWN_SET_BYTES
        {
            self.dropped = self.dropped.saturating_add(1);
            return;
        }
        self.bytes += len;
        self.sets.push_back(set);
    }

    fn remove(&mut self, index: usize) -> Option<BufferedSet> {
        let set = self.sets.remove(index)?;
        self.bytes = self.bytes.saturating_sub(set.bytes.len());
        Some(set)
    }

    fn next_deadline(&self) -> Option<Instant> {
        self.sets.iter().filter_map(|set| set.expires_at).min()
    }

    fn remove_keys(&mut self, keys: &HashSet<TemplateKey>) {
        let mut retained = VecDeque::with_capacity(self.sets.len());
        while let Some(set) = self.sets.pop_front() {
            if keys.contains(&set.key) {
                self.bytes = self.bytes.saturating_sub(set.bytes.len());
            } else {
                retained.push_back(set);
            }
        }
        self.sets = retained;
    }

    fn remove_keys_before(&mut self, keys: &HashSet<TemplateKey>, sequence: u64) {
        let mut retained = VecDeque::with_capacity(self.sets.len());
        while let Some(set) = self.sets.pop_front() {
            if keys.contains(&set.key) && set.sequence < sequence {
                self.bytes = self.bytes.saturating_sub(set.bytes.len());
            } else {
                retained.push_back(set);
            }
        }
        self.sets = retained;
    }

    fn remove_domains(&mut self, domains: &HashSet<u32>) {
        let mut retained = VecDeque::with_capacity(self.sets.len());
        while let Some(set) = self.sets.pop_front() {
            if domains.contains(&set.key.observation_domain_id) {
                self.bytes = self.bytes.saturating_sub(set.bytes.len());
            } else {
                retained.push_back(set);
            }
        }
        self.sets = retained;
    }

    fn remove_domains_before(&mut self, domains: &HashSet<u32>, sequence: u64) {
        let mut retained = VecDeque::with_capacity(self.sets.len());
        while let Some(set) = self.sets.pop_front() {
            if domains.contains(&set.key.observation_domain_id) && set.sequence < sequence {
                self.bytes = self.bytes.saturating_sub(set.bytes.len());
            } else {
                retained.push_back(set);
            }
        }
        self.sets = retained;
    }
}

#[derive(Debug, Default)]
struct TemplateHistory {
    used_ids: HashMap<u32, Box<[u64]>>,
}

impl TemplateHistory {
    fn validate_tracking(&self, generation: &TemplateGeneration) -> Result<(), IpfixError> {
        let new_domains = generation
            .templates
            .keys()
            .filter_map(|key| {
                (!self.used_ids.contains_key(&key.observation_domain_id))
                    .then_some(key.observation_domain_id)
            })
            .collect::<HashSet<_>>();
        if self.used_ids.len().saturating_add(new_domains.len()) > MAX_TRACKED_OBSERVATION_DOMAINS {
            return Err(format!(
                "template history observation-domain limit {} exceeded",
                MAX_TRACKED_OBSERVATION_DOMAINS
            )
            .into());
        }
        Ok(())
    }

    fn mark_used(&mut self, key: TemplateKey) -> bool {
        if !self.used_ids.contains_key(&key.observation_domain_id)
            && self.used_ids.len() >= MAX_TRACKED_OBSERVATION_DOMAINS
        {
            return false;
        }
        let words = self
            .used_ids
            .entry(key.observation_domain_id)
            .or_insert_with(|| vec![0u64; TEMPLATE_ID_HISTORY_WORDS].into_boxed_slice());
        let index = key.template_id as usize;
        words[index / u64::BITS as usize] |= 1u64 << (index % u64::BITS as usize);
        true
    }

    fn was_used(&self, key: &TemplateKey) -> bool {
        let Some(words) = self.used_ids.get(&key.observation_domain_id) else {
            return false;
        };
        let index = key.template_id as usize;
        words[index / u64::BITS as usize] & (1u64 << (index % u64::BITS as usize)) != 0
    }

    fn validate_reactivation(&self, template: &CompiledTemplate) -> Result<(), IpfixError> {
        if self.was_used(&template.key) {
            return Err(format!(
                "template ({}, {}) cannot reuse a retired ID without an exporter generation boundary",
                template.key.observation_domain_id, template.key.template_id
            )
            .into());
        }
        Ok(())
    }

    fn activate_generation(&mut self, generation: &TemplateGeneration) {
        for key in generation.templates.keys() {
            assert!(
                self.mark_used(*key),
                "template history capacity validated before activation"
            );
        }
    }

    fn retire(&mut self, template: &CompiledTemplate) {
        assert!(
            self.mark_used(template.key),
            "installed template domain must already be tracked"
        );
    }

    fn poison(&mut self, key: TemplateKey) {
        // Failure to allocate a new domain is still fail-closed: all future
        // updates for that domain fail validate_tracking while the map is full.
        self.mark_used(key);
    }
}

#[derive(Debug, Default)]
struct LifecycleMarkers {
    owners: HashSet<Arc<str>>,
    domains: HashSet<u32>,
    bytes: usize,
    drop_all_preinstall: bool,
}

impl LifecycleMarkers {
    fn insert(&mut self, owner: Arc<str>) {
        if self.drop_all_preinstall || self.owners.contains(owner.as_ref()) {
            return;
        }
        if self.owners.len() >= MAX_DELETED_SESSION_MARKERS
            || self.bytes.saturating_add(owner.len()) > MAX_DELETED_SESSION_MARKER_BYTES
        {
            // Losing an owner-specific marker must reduce availability rather
            // than permit stale pre-install data to cross a delete boundary.
            self.owners.clear();
            self.bytes = 0;
            self.drop_all_preinstall = true;
            return;
        }
        self.bytes += owner.len();
        self.owners.insert(owner);
    }

    fn remove(&mut self, owner: &str) {
        if let Some(owner) = self.owners.take(owner) {
            self.bytes = self.bytes.saturating_sub(owner.len());
        }
    }

    fn requires_fresh_data(&self, owner: &str) -> bool {
        self.drop_all_preinstall || self.owners.contains(owner)
    }

    fn insert_domains(&mut self, domains: impl IntoIterator<Item = u32>) {
        for domain in domains {
            if self.drop_all_preinstall {
                return;
            }
            if !self.domains.contains(&domain)
                && self.domains.len() >= MAX_TRACKED_OBSERVATION_DOMAINS
            {
                self.domains.clear();
                self.drop_all_preinstall = true;
                return;
            }
            self.domains.insert(domain);
        }
    }

    fn consume_owner(&mut self, owner: &str) {
        self.remove(owner);
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct IpfixError(String);

impl Display for IpfixError {
    fn fmt(&self, f: &mut Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl Error for IpfixError {}

impl From<&str> for IpfixError {
    fn from(value: &str) -> Self {
        Self(value.to_string())
    }
}

impl From<String> for IpfixError {
    fn from(value: String) -> Self {
        Self(value)
    }
}

/// Decodes the fixed-width SONiC HFT IPFIX profile into SAI statistics.
pub struct IpfixActor {
    saistats_recipients: Vec<Sender<SAIStatsBatchMessage>>,
    template_recipient: Receiver<IPFixTemplatesMessage>,
    record_recipient: Receiver<SocketBufferMessage>,
    sessions: HashMap<Arc<str>, SessionTemplates>,
    installed: HashMap<TemplateKey, Arc<CompiledTemplate>>,
    history: TemplateHistory,
    lifecycle_markers: LifecycleMarkers,
    live_template_bytes: usize,
    deferred_sets: DeferredSetBuffer,
    deferred_set_ttl: Duration,
    next_deferred_sequence: u64,
    restart_notifier: Option<Sender<String>>,
}

impl IpfixActor {
    pub fn new(
        template_recipient: Receiver<IPFixTemplatesMessage>,
        record_recipient: Receiver<SocketBufferMessage>,
    ) -> Self {
        Self {
            saistats_recipients: Vec::new(),
            template_recipient,
            record_recipient,
            sessions: HashMap::new(),
            installed: HashMap::new(),
            history: TemplateHistory::default(),
            lifecycle_markers: LifecycleMarkers::default(),
            live_template_bytes: 0,
            deferred_sets: DeferredSetBuffer::default(),
            deferred_set_ttl: UNKNOWN_SET_TTL,
            next_deferred_sequence: 1,
            restart_notifier: None,
        }
    }

    pub fn add_recipient(&mut self, recipient: Sender<SAIStatsBatchMessage>) {
        self.saistats_recipients.push(recipient);
    }

    pub fn set_restart_notifier(&mut self, notifier: Sender<String>) {
        self.restart_notifier = Some(notifier);
    }

    fn compile_generation(
        templates: &IPFixTemplatesMessage,
    ) -> Result<TemplateGeneration, IpfixError> {
        let bytes = templates
            .templates
            .as_deref()
            .ok_or("template update has no template data")?;
        if bytes.len() > MAX_TEMPLATE_CONFIG_BYTES {
            return Err(format!(
                "template update is {} bytes; maximum is {}",
                bytes.len(),
                MAX_TEMPLATE_CONFIG_BYTES
            )
            .into());
        }
        let names = templates
            .object_names
            .as_ref()
            .ok_or("HFT template update has no object_names")?;
        let ids = templates
            .object_ids
            .as_ref()
            .ok_or("HFT template update has no object_ids")?;

        if names.len() != ids.len() || names.is_empty() {
            return Err(format!(
                "object_names/object_ids must be non-empty and equal length (names={}, ids={})",
                names.len(),
                ids.len()
            )
            .into());
        }
        if names.len() > MAX_OBJECTS_PER_UPDATE {
            return Err(format!(
                "template update has {} objects; maximum is {}",
                names.len(),
                MAX_OBJECTS_PER_UPDATE
            )
            .into());
        }
        let metadata_bytes = names
            .iter()
            .try_fold(0usize, |total, name| total.checked_add(name.len()))
            .ok_or("object metadata size overflow")?;
        if metadata_bytes > MAX_OBJECT_METADATA_BYTES {
            return Err(format!(
                "object metadata is {metadata_bytes} bytes; maximum is {MAX_OBJECT_METADATA_BYTES}"
            )
            .into());
        }

        let mut object_names = HashMap::with_capacity(ids.len());
        for (id, name) in ids.iter().copied().zip(names) {
            if !(1..=0x7fff).contains(&id) {
                return Err(format!("object ID {id} is outside the IPFIX 15-bit IE range").into());
            }
            if name.is_empty() {
                return Err(format!("object ID {id} has an empty object name").into());
            }
            if object_names
                .insert(id, Arc::<str>::from(name.as_str()))
                .is_some()
            {
                return Err(format!("duplicate object ID {id}").into());
            }
        }

        let owner = Arc::<str>::from(templates.key.as_str());
        let mut compiled = HashMap::new();
        for message in IpfixMessages::new(bytes) {
            let message = message?;
            let domain = NetworkEndian::read_u32(&message[12..16]);
            let mut offset = IPFIX_HEADER_LEN;
            while offset < message.len() {
                let (set_id, set) = next_set(message, &mut offset)?;
                if set.is_empty() {
                    break;
                }
                if set_id != TEMPLATE_SET_ID {
                    return Err(format!(
                        "template channel only supports Template Set ID 2, got {set_id}"
                    )
                    .into());
                }
                compile_template_set(set, domain, &owner, &object_names, &mut compiled)?;
            }
        }
        if compiled.is_empty() {
            return Err("template update contains no HFT templates".into());
        }
        let compiled_bytes = compiled.values().try_fold(0usize, |total, template| {
            total
                .checked_add(compiled_template_weight(template)?)
                .ok_or_else(|| IpfixError::from("compiled template size overflow"))
        })?;
        const SHARED_ALLOCATION_OVERHEAD: usize = 32;
        let shared_bytes = names.iter().try_fold(
            owner
                .len()
                .checked_add(SHARED_ALLOCATION_OVERHEAD)
                .ok_or("compiled template size overflow")?,
            |total, name| {
                total
                    .checked_add(name.len())
                    .and_then(|total| total.checked_add(SHARED_ALLOCATION_OVERHEAD))
                    .ok_or_else(|| IpfixError::from("compiled template size overflow"))
            },
        )?;
        let compiled_bytes = compiled_bytes
            .checked_add(shared_bytes)
            .ok_or("compiled template size overflow")?;

        Ok(TemplateGeneration {
            owner,
            templates: compiled,
            compiled_bytes,
        })
    }

    fn handle_template(
        &mut self,
        templates: IPFixTemplatesMessage,
    ) -> Result<SAIStatsBatch, IpfixError> {
        match templates.operation {
            IPFixTemplateOperation::Deactivate => {
                self.handle_template_deactivation(&templates.key);
                return Ok(SAIStatsBatch::default());
            }
            IPFixTemplateOperation::Delete => {
                self.handle_template_deletion(&templates.key);
                return Ok(SAIStatsBatch::default());
            }
            IPFixTemplateOperation::Quarantine => {
                self.handle_template_quarantine(&templates)?;
                return Ok(SAIStatsBatch::default());
            }
            IPFixTemplateOperation::Update => {}
        }
        if let Err(err) = validate_template_update_limits(&templates) {
            self.handle_template_deactivation(&templates.key);
            if let Some(bytes) = templates.templates.as_deref() {
                let keys = extract_template_keys(bytes.as_slice()).map_err(|_| {
                    restart_error(format!(
                        "cannot identify every template in rejected update for {}",
                        templates.key
                    ))
                })?;
                let owners = keys
                    .iter()
                    .filter_map(|key| self.installed.get(key))
                    .map(|template| Arc::clone(&template.owner))
                    .collect::<HashSet<_>>();
                for owner in owners {
                    self.quarantine_session(&owner);
                }
            }
            return Err(err);
        }

        let generation = match Self::compile_generation(&templates) {
            Ok(generation) => generation,
            Err(err) => {
                error!(
                    "Rejecting invalid HFT template update for {} and quarantining any active generation: {}",
                    templates.key, err
                );
                self.handle_template_quarantine(&IPFixTemplatesMessage::quarantine(
                    templates.key.clone(),
                    templates.templates.clone(),
                ))?;
                return Err(err);
            }
        };
        if self.lifecycle_markers.drop_all_preinstall {
            return Err(restart_error(
                "lifecycle marker capacity was exceeded before template installation",
            ));
        }
        let owner = Arc::clone(&generation.owner);
        let updated_keys = generation.templates.keys().copied().collect::<HashSet<_>>();

        let conflicts = generation
            .templates
            .keys()
            .filter_map(|key| {
                self.installed.get(key).and_then(|existing| {
                    (existing.owner != owner).then(|| (*key, Arc::clone(&existing.owner)))
                })
            })
            .collect::<Vec<_>>();
        if let Some((conflict_key, conflict_owner)) = conflicts.first() {
            let err = IpfixError::from(format!(
                "template ({}, {}) is already owned by session {}",
                conflict_key.observation_domain_id, conflict_key.template_id, conflict_owner
            ));
            let conflicting_owners = conflicts
                .into_iter()
                .map(|(_, conflict_owner)| conflict_owner)
                .collect::<HashSet<_>>();
            for conflict_owner in conflicting_owners {
                self.quarantine_session(conflict_owner.as_ref());
            }
            self.quarantine_session(owner.as_ref());
            self.poison_generation(&generation);
            return Err(err);
        }

        let is_first_install = self
            .sessions
            .get(owner.as_ref())
            .is_none_or(|session| session.active.is_none());
        if is_first_install {
            let blocked_domains = generation_domains(&generation)
                .into_iter()
                .filter(|domain| self.lifecycle_markers.domains.contains(domain))
                .collect::<HashSet<_>>();
            if !blocked_domains.is_empty() {
                self.poison_generation(&generation);
                return Err(restart_error(format!(
                    "observation domain(s) {blocked_domains:?} cannot accept a new generation"
                )));
            }
            self.history.validate_tracking(&generation)?;
            if let Err(err) = validate_generation_reactivation(&generation, &self.history) {
                self.poison_generation(&generation);
                return Err(err);
            }
            if let Err(err) = validate_projected_capacity(
                &generation,
                &self.installed,
                &HashSet::new(),
                self.live_template_bytes,
                0,
            ) {
                return Err(err);
            }
            let domains = generation_domains(&generation);
            let fenced_domains = domains
                .iter()
                .filter(|domain| {
                    self.lifecycle_markers.drop_all_preinstall
                        || self.lifecycle_markers.domains.contains(domain)
                })
                .copied()
                .collect::<HashSet<_>>();
            let has_owner_fence = self.lifecycle_markers.requires_fresh_data(&owner);
            let deferred_floor = if has_owner_fence || !fenced_domains.is_empty() {
                self.next_deferred_sequence
            } else {
                0
            };
            if has_owner_fence {
                self.deferred_sets
                    .remove_keys_before(&updated_keys, deferred_floor);
                self.deferred_sets
                    .remove_domains_before(&domains, deferred_floor);
            }
            self.deferred_sets
                .remove_domains_before(&fenced_domains, deferred_floor);
            self.lifecycle_markers.consume_owner(&owner);
            self.history.activate_generation(&generation);
            self.live_template_bytes = self
                .live_template_bytes
                .checked_add(generation.compiled_bytes)
                .ok_or("live template byte accounting overflow")?;
            install_generation(&generation, &mut self.installed);
            self.sessions.insert(
                owner,
                SessionTemplates {
                    active: Some(generation),
                    pending: None,
                    pending_cutover_keys: HashSet::new(),
                    enabled: true,
                    deferred_floor,
                },
            );
            return self.finish_template_update();
        }

        let (active, pending, pending_cutover_keys, was_enabled) = {
            let session = self
                .sessions
                .get(owner.as_ref())
                .expect("session existence checked above");
            (
                session.active.clone().expect("checked above"),
                session.pending.clone(),
                session.pending_cutover_keys.clone(),
                session.enabled,
            )
        };
        if active.equivalent(&generation) {
            let session = self
                .sessions
                .get_mut(owner.as_ref())
                .expect("session existence checked above");
            if let Some(pending) = session.pending.take() {
                self.live_template_bytes = self
                    .live_template_bytes
                    .checked_sub(pending.compiled_bytes)
                    .ok_or("live template byte accounting underflow")?;
                retire_canceled_pending_generation(
                    &pending,
                    session.active.as_ref(),
                    &mut self.installed,
                    &mut self.history,
                );
                restore_generation(session.active.as_ref(), &mut self.installed);
                session.pending_cutover_keys.clear();
                debug!("Canceled pending HFT template generation for {owner}");
            }
            debug!("Refreshed unchanged HFT template generation for {owner}");
        } else if pending
            .as_ref()
            .is_some_and(|pending| pending.equivalent(&generation))
        {
            debug!("Refreshed unchanged pending HFT template generation for {owner}");
        } else {
            let blocked_domains = generation_domains(&generation)
                .into_iter()
                .filter(|domain| self.lifecycle_markers.domains.contains(domain))
                .collect::<HashSet<_>>();
            if !blocked_domains.is_empty() {
                self.quarantine_session(owner.as_ref());
                self.poison_generation(&generation);
                return Err(restart_error(format!(
                    "observation domain(s) {blocked_domains:?} cannot accept a new generation"
                )));
            }
            let plan = match (|| {
                for (key, candidate) in &generation.templates {
                    if let Some(current) = active.templates.get(key) {
                        if current.as_ref() != candidate.as_ref() {
                            return Err(format!(
                                "session {owner} cannot change template ({}, {}) in place; use a new template ID",
                                key.observation_domain_id, key.template_id
                            )
                            .into());
                        }
                    }
                }
                if !generation
                    .templates
                    .keys()
                    .any(|key| !active.templates.contains_key(key))
                {
                    return Err(format!(
                        "session {owner} update has no new template ID to mark a cutover"
                    )
                    .into());
                }
                plan_pending_update(
                    &generation,
                    pending.as_ref(),
                    &active,
                    &self.history,
                    &pending_cutover_keys,
                )
            })() {
                Ok(plan) => plan,
                Err(err) => {
                    self.quarantine_session(owner.as_ref());
                    self.poison_generation(&generation);
                    return Err(err);
                }
            };
            self.history.validate_tracking(&generation)?;
            if let Err(err) = validate_projected_capacity(
                &generation,
                &self.installed,
                &plan.retire_keys,
                self.live_template_bytes,
                pending.as_ref().map_or(0, |pending| pending.compiled_bytes),
            ) {
                return Err(err);
            }
            let domains = generation_domains(&generation);
            let fenced_domains = domains
                .iter()
                .filter(|domain| {
                    self.lifecycle_markers.drop_all_preinstall
                        || self.lifecycle_markers.domains.contains(domain)
                })
                .copied()
                .collect::<HashSet<_>>();
            let update_floor = self.next_deferred_sequence;

            let session = self
                .sessions
                .get_mut(owner.as_ref())
                .expect("session existence checked above");
            if let Some(pending) = session.pending.take() {
                self.live_template_bytes = self
                    .live_template_bytes
                    .checked_sub(pending.compiled_bytes)
                    .ok_or("live template byte accounting underflow")?;
                retire_replaced_pending_generation(
                    &pending,
                    &generation,
                    session.active.as_ref(),
                    &mut self.installed,
                    &mut self.history,
                );
                restore_generation(session.active.as_ref(), &mut self.installed);
            }
            self.history.activate_generation(&generation);
            self.live_template_bytes = self
                .live_template_bytes
                .checked_add(generation.compiled_bytes)
                .ok_or("live template byte accounting overflow")?;
            install_generation(&generation, &mut self.installed);
            session.pending = Some(generation);
            session.pending_cutover_keys = plan.cutover_keys;
            self.deferred_sets
                .remove_domains_before(&fenced_domains, update_floor);
            self.lifecycle_markers.consume_owner(&owner);
        }

        let session = self
            .sessions
            .get_mut(owner.as_ref())
            .expect("session existence checked above");
        if !was_enabled {
            session.deferred_floor = self.next_deferred_sequence;
            let domains = generation_domains(
                session
                    .pending
                    .as_ref()
                    .unwrap_or_else(|| session.active.as_ref().expect("active generation")),
            );
            self.deferred_sets
                .remove_domains_before(&domains, session.deferred_floor);
            self.lifecycle_markers.consume_owner(&owner);
        }
        self.deferred_sets
            .remove_keys_before(&updated_keys, session.deferred_floor);
        session.enabled = true;

        self.finish_template_update()
    }

    fn quarantine_session(&mut self, owner: &str) {
        let marker = Arc::<str>::from(owner);
        let mut keys = HashSet::new();
        let mut domains = HashSet::new();
        if let Some((session_owner, session)) = self.sessions.remove_entry(owner) {
            for generation in [session.active, session.pending].into_iter().flatten() {
                self.live_template_bytes = self
                    .live_template_bytes
                    .checked_sub(generation.compiled_bytes)
                    .expect("live template byte accounting underflow");
                keys.extend(generation.templates.keys().copied());
            }
            for key in &keys {
                domains.insert(key.observation_domain_id);
                if self
                    .installed
                    .get(key)
                    .is_some_and(|template| template.owner == session_owner)
                {
                    self.installed.remove(key);
                }
                self.history.poison(*key);
            }
        }
        self.deferred_sets.remove_domains(&domains);
        self.lifecycle_markers.insert(marker);
        self.lifecycle_markers.insert_domains(domains);
    }

    fn handle_template_quarantine(
        &mut self,
        templates: &IPFixTemplatesMessage,
    ) -> Result<(), IpfixError> {
        self.quarantine_session(&templates.key);
        let keys = templates
            .templates
            .as_deref()
            .map(|bytes| extract_template_keys(bytes.as_slice()))
            .transpose();
        match keys {
            Ok(Some(keys)) if !keys.is_empty() => {
                let owners: HashSet<Arc<str>> = keys
                    .iter()
                    .filter_map(|key| self.installed.get(key))
                    .map(|template| Arc::clone(&template.owner))
                    .collect::<HashSet<_>>();
                for owner in owners {
                    self.quarantine_session(&owner);
                }
                let domains = keys
                    .iter()
                    .map(|key| key.observation_domain_id)
                    .collect::<HashSet<_>>();
                for key in keys {
                    self.history.poison(key);
                }
                self.deferred_sets.remove_domains(&domains);
                self.lifecycle_markers.insert_domains(domains);
            }
            _ => {
                return Err(restart_error(format!(
                    "cannot identify every template affected by quarantine for {}",
                    templates.key
                )))
            }
        }
        Ok(())
    }

    fn poison_generation(&mut self, generation: &TemplateGeneration) {
        let keys = generation.templates.keys().copied().collect::<HashSet<_>>();
        for key in &keys {
            self.history.poison(*key);
        }
        self.deferred_sets.remove_keys(&keys);
    }

    fn finish_template_update(&mut self) -> Result<SAIStatsBatch, IpfixError> {
        let mut batch = SAIStatsBatch::default();
        self.drain_deferred_sets(&mut batch, Instant::now())?;
        Ok(batch)
    }

    fn promote_pending_for(&mut self, key: TemplateKey) {
        let Some(owner) = self
            .installed
            .get(&key)
            .map(|template| Arc::clone(&template.owner))
        else {
            return;
        };
        let Some(session) = self.sessions.get_mut(&owner) else {
            return;
        };
        if !session.enabled {
            return;
        }
        let should_promote = session
            .pending
            .as_ref()
            .is_some_and(|pending| pending.templates.contains_key(&key))
            && session.pending_cutover_keys.contains(&key);
        if !should_promote {
            return;
        }

        let pending = session.pending.take().expect("checked above");
        session.pending_cutover_keys.clear();
        if let Some(active) = session.active.replace(pending.clone()) {
            self.live_template_bytes = self
                .live_template_bytes
                .checked_sub(active.compiled_bytes)
                .expect("live template byte accounting underflow");
            for old_key in active.templates.keys() {
                if !pending.templates.contains_key(old_key) {
                    if let Some(old_template) = self.installed.remove(old_key) {
                        self.history.retire(&old_template);
                    }
                }
            }
        }
        restore_generation(session.active.as_ref(), &mut self.installed);
        debug!("Promoted pending HFT template generation for {owner}");
    }

    fn handle_template_deletion(&mut self, key: &str) {
        let mut domains = HashSet::new();
        if let Some((owner, session)) = self.sessions.remove_entry(key) {
            for generation in [session.active, session.pending].into_iter().flatten() {
                self.live_template_bytes = self
                    .live_template_bytes
                    .checked_sub(generation.compiled_bytes)
                    .expect("live template byte accounting underflow");
                for template_key in generation.templates.keys() {
                    domains.insert(template_key.observation_domain_id);
                    if self
                        .installed
                        .get(template_key)
                        .is_some_and(|template| template.owner == owner)
                    {
                        if let Some(template) = self.installed.remove(template_key) {
                            self.history.retire(&template);
                        }
                    }
                }
            }
            self.lifecycle_markers.insert(owner);
            self.lifecycle_markers
                .insert_domains(domains.iter().copied());
        } else {
            self.lifecycle_markers.insert(Arc::<str>::from(key));
        }
        self.deferred_sets.remove_domains(&domains);
    }

    fn handle_template_deactivation(&mut self, key: &str) {
        let Some(session) = self.sessions.get_mut(key) else {
            self.lifecycle_markers.insert(Arc::<str>::from(key));
            return;
        };
        session.enabled = false;
        let keys = session
            .active
            .iter()
            .chain(session.pending.iter())
            .flat_map(|generation| generation.templates.keys())
            .copied()
            .collect::<HashSet<_>>();
        let domains = keys
            .iter()
            .map(|key| key.observation_domain_id)
            .collect::<HashSet<_>>();
        self.deferred_sets.remove_keys(&keys);
        self.lifecycle_markers.insert(Arc::<str>::from(key));
        self.deferred_sets.remove_domains(&domains);
    }

    #[cfg(test)]
    fn handle_record(&mut self, records: &[u8]) -> Result<SAIStatsBatch, IpfixError> {
        let mut batch = SAIStatsBatch::default();
        let input = self.validate_record_input(records)?;
        for validated in input.messages {
            self.process_data_message(validated, &mut batch)?;
        }
        Ok(batch)
    }

    fn validate_record_input<'a>(
        &self,
        records: &'a [u8],
    ) -> Result<ValidatedRecordInput<'a>, IpfixError> {
        if records.is_empty() {
            return Err("empty IPFIX payload".into());
        }
        let mut messages = Vec::new();
        let mut total_sets = 0usize;
        let mut unresolved_sets = 0usize;
        let mut blocked_domains = self
            .deferred_sets
            .sets
            .iter()
            .map(|set| set.key.observation_domain_id)
            .collect::<HashSet<_>>();
        for message in IpfixMessages::new(records) {
            let validated = self.validate_data_message(message?, &mut blocked_domains)?;
            total_sets = total_sets
                .checked_add(validated.sets.len())
                .ok_or("data Set count overflow")?;
            unresolved_sets = unresolved_sets
                .checked_add(
                    validated
                        .sets
                        .iter()
                        .filter(|set| set.template.is_none() && !set.retired)
                        .count(),
                )
                .ok_or("unresolved data Set count overflow")?;
            if total_sets > MAX_DATA_SETS_PER_RECORD_INPUT {
                return Err(format!(
                    "record input exceeds {MAX_DATA_SETS_PER_RECORD_INPUT} data Sets"
                )
                .into());
            }
            if unresolved_sets > MAX_UNRESOLVED_SETS_PER_RECORD_INPUT {
                return Err(format!(
                    "record input exceeds {MAX_UNRESOLVED_SETS_PER_RECORD_INPUT} unresolved data Sets"
                )
                .into());
            }
            messages.push(validated);
        }
        Ok(ValidatedRecordInput { messages })
    }

    fn validate_data_message<'a>(
        &self,
        message: &'a [u8],
        blocked_domains: &mut HashSet<u32>,
    ) -> Result<ValidatedDataMessage<'a>, IpfixError> {
        let domain = NetworkEndian::read_u32(&message[12..16]);
        let mut offset = IPFIX_HEADER_LEN;
        let mut sets = Vec::new();
        let mut counter_count = 0usize;
        while offset < message.len() {
            let (set_id, set) = next_set(message, &mut offset)?;
            if set.is_empty() {
                break;
            }
            if set_id < MIN_DATA_SET_ID {
                return Err(format!("data channel contains non-data IPFIX Set ID {set_id}").into());
            }
            let key = TemplateKey {
                observation_domain_id: domain,
                template_id: set_id,
            };
            let template = self.installed.get(&key).cloned();
            let enabled = template.as_ref().is_some_and(|template| {
                self.sessions
                    .get(template.owner.as_ref())
                    .is_some_and(|session| session.enabled)
            });
            let layout = if enabled {
                template
                    .as_deref()
                    .map(|template| validate_data_set(template, set))
                    .transpose()?
            } else {
                None
            };
            let domain_blocked = blocked_domains.contains(&key.observation_domain_id);
            if template.is_none() && !self.history.was_used(&key) {
                blocked_domains.insert(key.observation_domain_id);
            }
            if enabled {
                counter_count = counter_count
                    .checked_add(layout.expect("installed template has layout").counter_count)
                    .ok_or("decoded counter count overflow")?;
            }
            sets.push(ValidatedDataSet {
                key,
                bytes: set,
                template,
                layout,
                enabled,
                retired: self.history.was_used(&key),
                domain_blocked,
            });
        }
        if sets.is_empty() {
            return Err("IPFIX data message contains no sets".into());
        }
        Ok(ValidatedDataMessage {
            sets,
            counter_count,
        })
    }

    fn process_data_message(
        &mut self,
        message: ValidatedDataMessage<'_>,
        batch: &mut SAIStatsBatch,
    ) -> Result<(), IpfixError> {
        for set in message.sets {
            let ValidatedDataSet {
                key,
                bytes,
                template,
                layout,
                enabled,
                retired,
                domain_blocked,
            } = set;
            match (template, layout) {
                (Some(template), Some(layout)) if enabled && !domain_blocked => {
                    self.promote_pending_for(key);
                    if self
                        .installed
                        .get(&key)
                        .is_some_and(|installed| Arc::ptr_eq(installed, &template))
                    {
                        self.decode_set(&template, bytes, layout, batch);
                    }
                }
                (Some(_), Some(_)) if enabled => {
                    let sequence = self.next_deferred_sequence;
                    self.next_deferred_sequence = self.next_deferred_sequence.saturating_add(1);
                    self.deferred_sets.push(BufferedSet {
                        key,
                        bytes: Arc::from(bytes),
                        sequence,
                        expires_at: None,
                    });
                }
                (Some(_), _) => {}
                (None, _) if !retired => {
                    let received_at = Instant::now();
                    let sequence = self.next_deferred_sequence;
                    self.next_deferred_sequence = self.next_deferred_sequence.saturating_add(1);
                    self.deferred_sets.push(BufferedSet {
                        key,
                        bytes: Arc::from(bytes),
                        sequence,
                        expires_at: Some(received_at + self.deferred_set_ttl),
                    });
                }
                (None, _) => {}
            }
        }
        self.drain_deferred_sets(batch, Instant::now())?;
        Ok(())
    }

    fn drain_deferred_sets(
        &mut self,
        batch: &mut SAIStatsBatch,
        now: Instant,
    ) -> Result<(), IpfixError> {
        let expired = self.deferred_sets.expire(now);
        self.log_deferred_drops(expired);

        let installed = &self.installed;
        let sessions = &self.sessions;
        for set in &mut self.deferred_sets.sets {
            if installed.get(&set.key).is_some_and(|template| {
                sessions
                    .get(template.owner.as_ref())
                    .is_some_and(|session| session.enabled)
            }) {
                set.expires_at = None;
            }
        }

        let mut blocked_domains = HashSet::new();
        let mut index = 0usize;
        while index < self.deferred_sets.sets.len() {
            let key = self.deferred_sets.sets[index].key;
            if blocked_domains.contains(&key.observation_domain_id) {
                index += 1;
                continue;
            }
            let Some(template) = self.installed.get(&key).cloned() else {
                if self.history.was_used(&key) {
                    self.deferred_sets.remove(index);
                    continue;
                }
                blocked_domains.insert(key.observation_domain_id);
                index += 1;
                continue;
            };
            let enabled = self
                .sessions
                .get(template.owner.as_ref())
                .is_some_and(|session| session.enabled);
            if !enabled {
                self.deferred_sets.remove(index);
                continue;
            }
            let layout = match validate_data_set(&template, &self.deferred_sets.sets[index].bytes) {
                Ok(layout) => layout,
                Err(err) => {
                    let invalid = self
                        .deferred_sets
                        .remove(index)
                        .expect("index checked above");
                    warn!(
                        "Dropping invalid deferred HFT data set ({}, {}): {}",
                        invalid.key.observation_domain_id, invalid.key.template_id, err
                    );
                    continue;
                }
            };
            if !batch.is_empty()
                && batch.counter_count().saturating_add(layout.counter_count)
                    > MAX_COUNTERS_PER_BATCH
            {
                return Ok(());
            }

            let ready = self
                .deferred_sets
                .remove(index)
                .expect("index checked above");
            self.promote_pending_for(ready.key);
            if self
                .installed
                .get(&ready.key)
                .is_some_and(|installed| Arc::ptr_eq(installed, &template))
            {
                self.decode_set(&template, &ready.bytes, layout, batch);
            }
        }
        Ok(())
    }

    fn log_deferred_drops(&self, dropped: u64) {
        if dropped > 0 {
            warn!(
                "HFT deferred-set buffer dropped {} set(s); total drops={}",
                dropped, self.deferred_sets.dropped
            );
        }
    }

    fn decode_set(
        &self,
        template: &CompiledTemplate,
        set: &[u8],
        layout: DataSetLayout,
        batch: &mut SAIStatsBatch,
    ) {
        let payload = &set[SET_HEADER_LEN..];
        batch.reserve(layout.record_count, layout.counter_count);

        for record in payload[..layout.record_bytes].chunks_exact(template.record_len) {
            let time_offset = template.observation_time_offset;
            let observation_time = NetworkEndian::read_u64(&record[time_offset..time_offset + 8]);
            batch.push_record(
                observation_time,
                template.counters.iter().map(|counter| SAIStat {
                    object_name: Arc::clone(&counter.object_name),
                    type_id: counter.type_id,
                    stat_id: counter.stat_id,
                    counter: read_be_u64(
                        &record[counter.offset..counter.offset + counter.len as usize],
                    ),
                }),
            );
        }
    }

    async fn process_record_input(
        &mut self,
        records: &[u8],
        batch: &mut SAIStatsBatch,
    ) -> Result<(), IpfixError> {
        let input = self.validate_record_input(records)?;
        let dropped_before = self.deferred_sets.dropped;
        for validated in input.messages {
            let counters = validated.counter_count;
            if !batch.is_empty()
                && batch.counter_count().saturating_add(counters) > MAX_COUNTERS_PER_BATCH
            {
                self.send_batch(std::mem::take(batch)).await?;
            }
            self.process_data_message(validated, batch)?;
            if batch.counter_count() >= MAX_COUNTERS_PER_BATCH {
                self.send_batch(std::mem::take(batch)).await?;
            }
        }
        self.log_deferred_drops(self.deferred_sets.dropped - dropped_before);
        Ok(())
    }

    async fn send_batch(&self, batch: SAIStatsBatch) -> Result<(), IpfixError> {
        if batch.is_empty() || self.saistats_recipients.is_empty() {
            return Ok(());
        }
        if batch.counter_count() <= MAX_COUNTERS_PER_BATCH {
            let closed = self.send_bounded_batch(batch).await;
            return if closed == 0 {
                Ok(())
            } else {
                Err(format!("{closed} SAI stats recipient(s) closed").into())
            };
        }
        let mut closed = 0usize;
        for batch in batch.into_counter_bounded_batches(MAX_COUNTERS_PER_BATCH) {
            closed = closed.saturating_add(self.send_bounded_batch(batch).await);
        }
        if closed > 0 {
            return Err(format!("{closed} SAI stats recipient send(s) closed").into());
        }
        Ok(())
    }

    async fn send_bounded_batch(&self, batch: SAIStatsBatch) -> usize {
        debug_assert!(batch.counter_count() <= MAX_COUNTERS_PER_BATCH);
        let batch = Arc::new(batch);
        let mut blocked = Vec::new();
        let mut closed = 0usize;
        for recipient in &self.saistats_recipients {
            match recipient.try_reserve() {
                Ok(permit) => permit.send(Arc::clone(&batch)),
                Err(tokio::sync::mpsc::error::TrySendError::Full(_)) => blocked.push(recipient),
                Err(tokio::sync::mpsc::error::TrySendError::Closed(_)) => {
                    closed += 1;
                }
            }
        }
        for recipient in blocked {
            if recipient.send(Arc::clone(&batch)).await.is_err() {
                closed += 1;
            }
        }
        closed
    }

    async fn send_batch_and_drain_deferred(
        &mut self,
        batch: SAIStatsBatch,
    ) -> Result<(), IpfixError> {
        self.send_batch(batch).await?;
        loop {
            let mut ready = SAIStatsBatch::default();
            self.drain_deferred_sets(&mut ready, Instant::now())?;
            if ready.is_empty() {
                return Ok(());
            }
            self.send_batch(ready).await?;
        }
    }

    pub async fn run(mut actor: IpfixActor) -> Result<(), IpfixError> {
        loop {
            select! {
                _ = async {
                    if let Some(deadline) = actor.deferred_sets.next_deadline() {
                        sleep_until(deadline).await;
                    }
                }, if actor.deferred_sets.next_deadline().is_some() => {
                    let mut batch = SAIStatsBatch::default();
                    actor.drain_deferred_sets(&mut batch, Instant::now())?;
                    actor.send_batch_and_drain_deferred(batch).await?;
                },
                template = actor.template_recipient.recv() => match template {
                    Some(template) => {
                        record_comm_stats(
                            ChannelLabel::SwssToIpfixTemplates,
                            actor.template_recipient.len(),
                        );
                        match actor.handle_template(template) {
                            Ok(batch) => actor.send_batch_and_drain_deferred(batch).await?,
                            Err(err) if is_restart_required(&err) => {
                                if let Some(notifier) = &actor.restart_notifier {
                                    let _ = notifier.try_send(err.to_string());
                                }
                                return Err(err);
                            }
                            Err(err) => error!("HFT template update rejected: {err}"),
                        }
                    }
                    None => return Err("IPFIX template input channel closed".into()),
                },
                record = actor.record_recipient.recv() => match record {
                    Some(record) => {
                        record_comm_stats(
                            ChannelLabel::DataNetlinkToIpfixRecords,
                            actor.record_recipient.len(),
                        );
                        let mut batch = SAIStatsBatch::default();
                        let mut input_count = 1usize;
                        let mut input_bytes = record.len();
                        if let Err(err) = actor.process_record_input(&record, &mut batch).await {
                            warn!("Dropping invalid HFT IPFIX message: {err}");
                        }
                        while input_count < MAX_RECORD_INPUTS_PER_BATCH
                            && input_bytes < MAX_RECORD_INPUT_BYTES_PER_BATCH
                            && actor.template_recipient.is_empty()
                        {
                            let Ok(next) = actor.record_recipient.try_recv() else {
                                break;
                            };
                            input_count += 1;
                            input_bytes = input_bytes.saturating_add(next.len());
                            if let Err(err) = actor.process_record_input(&next, &mut batch).await {
                                warn!("Dropping invalid HFT IPFIX message: {err}");
                            }
                        }
                        actor.send_batch_and_drain_deferred(batch).await?;
                    }
                    None => return Err("IPFIX record input channel closed".into()),
                }
            }
        }
    }
}

fn install_generation(
    generation: &TemplateGeneration,
    installed: &mut HashMap<TemplateKey, Arc<CompiledTemplate>>,
) {
    for (key, template) in &generation.templates {
        installed.insert(*key, Arc::clone(template));
    }
}

fn restore_generation(
    generation: Option<&TemplateGeneration>,
    installed: &mut HashMap<TemplateKey, Arc<CompiledTemplate>>,
) {
    if let Some(generation) = generation {
        install_generation(generation, installed);
    }
}

fn retire_canceled_pending_generation(
    pending: &TemplateGeneration,
    active: Option<&TemplateGeneration>,
    installed: &mut HashMap<TemplateKey, Arc<CompiledTemplate>>,
    history: &mut TemplateHistory,
) {
    for key in pending.templates.keys() {
        if !active.is_some_and(|generation| generation.templates.contains_key(key)) {
            if let Some(template) = installed.remove(key) {
                history.retire(&template);
            }
        }
    }
}

fn retire_replaced_pending_generation(
    pending: &TemplateGeneration,
    replacement: &TemplateGeneration,
    active: Option<&TemplateGeneration>,
    installed: &mut HashMap<TemplateKey, Arc<CompiledTemplate>>,
    history: &mut TemplateHistory,
) {
    for key in pending.templates.keys() {
        if !active.is_some_and(|generation| generation.templates.contains_key(key))
            && !replacement.templates.contains_key(key)
        {
            if let Some(template) = installed.remove(key) {
                history.retire(&template);
            }
        }
    }
}

fn plan_pending_update(
    replacement: &TemplateGeneration,
    pending: Option<&TemplateGeneration>,
    active: &TemplateGeneration,
    history: &TemplateHistory,
    pending_cutover_keys: &HashSet<TemplateKey>,
) -> Result<PendingUpdatePlan, IpfixError> {
    for template in replacement.templates.values() {
        if let Some(pending_template) =
            pending.and_then(|generation| generation.templates.get(&template.key))
        {
            if pending_template.as_ref() != template.as_ref() {
                return Err(format!(
                    "pending template ({}, {}) cannot change in place; use a new template ID",
                    template.key.observation_domain_id, template.key.template_id
                )
                .into());
            }
        } else if !active.templates.contains_key(&template.key) {
            history.validate_reactivation(template)?;
        }
    }

    let cutover_keys = replacement
        .templates
        .iter()
        .filter(|(key, _)| {
            !active.templates.contains_key(key)
                && (!history.was_used(key) || pending_cutover_keys.contains(key))
        })
        .map(|(key, _)| *key)
        .collect::<HashSet<_>>();
    if cutover_keys.is_empty() {
        return Err(format!(
            "session {} update has no previously unused template ID to mark a cutover",
            replacement.owner
        )
        .into());
    }
    let retire_keys = pending
        .into_iter()
        .flat_map(|pending| pending.templates.keys())
        .filter(|key| {
            !active.templates.contains_key(key) && !replacement.templates.contains_key(key)
        })
        .copied()
        .collect();
    Ok(PendingUpdatePlan {
        cutover_keys,
        retire_keys,
    })
}

fn validate_generation_reactivation(
    generation: &TemplateGeneration,
    history: &TemplateHistory,
) -> Result<(), IpfixError> {
    for template in generation.templates.values() {
        history.validate_reactivation(template)?;
    }
    Ok(())
}

fn validate_projected_capacity(
    generation: &TemplateGeneration,
    installed: &HashMap<TemplateKey, Arc<CompiledTemplate>>,
    retire_keys: &HashSet<TemplateKey>,
    live_template_bytes: usize,
    replaced_generation_bytes: usize,
) -> Result<(), IpfixError> {
    let mut projected = installed.keys().copied().collect::<HashSet<_>>();
    projected.retain(|key| !retire_keys.contains(key));
    projected.extend(generation.templates.keys().copied());
    if projected.len() > MAX_INSTALLED_TEMPLATE_KEYS {
        return Err(format!(
            "template key registry limit {} exceeded",
            MAX_INSTALLED_TEMPLATE_KEYS
        )
        .into());
    }
    let projected_bytes = live_template_bytes
        .checked_sub(replaced_generation_bytes)
        .ok_or("live template byte accounting underflow")?
        .checked_add(generation.compiled_bytes)
        .ok_or("live template byte accounting overflow")?;
    if projected_bytes > MAX_LIVE_TEMPLATE_BYTES {
        return Err(format!(
            "live compiled template limit {MAX_LIVE_TEMPLATE_BYTES} bytes exceeded"
        )
        .into());
    }
    Ok(())
}

fn validate_record_counter_count(template_id: u16, counter_count: usize) -> Result<(), IpfixError> {
    if counter_count > MAX_COUNTERS_PER_BATCH {
        return Err(format!(
            "template {template_id} contains {counter_count} counters per record; maximum is {MAX_COUNTERS_PER_BATCH}"
        )
        .into());
    }
    Ok(())
}

const RESTART_ERROR_PREFIX: &str = "restart required: ";

fn restart_error(message: impl Display) -> IpfixError {
    format!("{RESTART_ERROR_PREFIX}{message}").into()
}

pub fn is_restart_required(err: &IpfixError) -> bool {
    err.0.starts_with(RESTART_ERROR_PREFIX)
}

fn generation_domains(generation: &TemplateGeneration) -> HashSet<u32> {
    generation
        .templates
        .keys()
        .map(|key| key.observation_domain_id)
        .collect()
}

fn validate_template_update_limits(templates: &IPFixTemplatesMessage) -> Result<(), IpfixError> {
    let bytes = templates
        .templates
        .as_ref()
        .ok_or("template update has no template data")?;
    if bytes.len() > MAX_TEMPLATE_CONFIG_BYTES {
        return Err(format!(
            "template update is {} bytes; maximum is {MAX_TEMPLATE_CONFIG_BYTES}",
            bytes.len()
        )
        .into());
    }
    let names = templates
        .object_names
        .as_ref()
        .ok_or("HFT template update has no object_names")?;
    let ids = templates
        .object_ids
        .as_ref()
        .ok_or("HFT template update has no object_ids")?;
    if names.len() > MAX_OBJECTS_PER_UPDATE || ids.len() > MAX_OBJECTS_PER_UPDATE {
        return Err(format!("object metadata exceeds {MAX_OBJECTS_PER_UPDATE} entries").into());
    }
    let metadata_bytes = names
        .iter()
        .try_fold(0usize, |total, name| total.checked_add(name.len()))
        .ok_or("object metadata size overflow")?
        .checked_add(
            ids.len()
                .checked_mul(std::mem::size_of::<u16>())
                .ok_or("object metadata size overflow")?,
        )
        .ok_or("object metadata size overflow")?;
    if metadata_bytes > MAX_OBJECT_METADATA_BYTES {
        return Err(format!(
            "object metadata is {metadata_bytes} bytes; maximum is {MAX_OBJECT_METADATA_BYTES}"
        )
        .into());
    }
    Ok(())
}

fn extract_template_keys(bytes: &[u8]) -> Result<HashSet<TemplateKey>, IpfixError> {
    if bytes.len() > MAX_TEMPLATE_CONFIG_BYTES {
        return Err(format!(
            "template input is {} bytes; maximum is {MAX_TEMPLATE_CONFIG_BYTES}",
            bytes.len()
        )
        .into());
    }
    let mut keys = HashSet::new();
    for message in IpfixMessages::new(bytes) {
        let message = message?;
        let domain = NetworkEndian::read_u32(&message[12..16]);
        let mut message_offset = IPFIX_HEADER_LEN;
        while message_offset < message.len() {
            let (set_id, set) = next_set(message, &mut message_offset)?;
            if set.is_empty() {
                break;
            }
            if set_id != TEMPLATE_SET_ID {
                return Err(format!("template input contains Set ID {set_id}").into());
            }
            let mut offset = SET_HEADER_LEN;
            while offset < set.len() {
                let remaining = &set[offset..];
                if remaining.len() < MIN_HFT_TEMPLATE_RECORD_LEN
                    && remaining.iter().all(|byte| *byte == 0)
                {
                    break;
                }
                if remaining.len() < 4 {
                    return Err("template set has a truncated template record".into());
                }
                let template_id = NetworkEndian::read_u16(&set[offset..offset + 2]);
                let field_count = NetworkEndian::read_u16(&set[offset + 2..offset + 4]) as usize;
                if template_id < MIN_DATA_SET_ID || field_count == 0 {
                    return Err("template key extraction found an invalid template header".into());
                }
                offset += 4;
                for _ in 0..field_count {
                    if set.len().saturating_sub(offset) < 4 {
                        return Err("template key extraction found a truncated field".into());
                    }
                    let raw_id = NetworkEndian::read_u16(&set[offset..offset + 2]);
                    offset += 4;
                    if raw_id & 0x8000 != 0 {
                        if set.len().saturating_sub(offset) < 4 {
                            return Err(
                                "template key extraction found a truncated enterprise number"
                                    .into(),
                            );
                        }
                        offset += 4;
                    }
                }
                keys.insert(TemplateKey {
                    observation_domain_id: domain,
                    template_id,
                });
                if keys.len() > MAX_TEMPLATES_PER_UPDATE {
                    return Err(format!(
                        "template input exceeds {MAX_TEMPLATES_PER_UPDATE} templates"
                    )
                    .into());
                }
            }
        }
    }
    if keys.is_empty() {
        return Err("template key extraction found no templates".into());
    }
    Ok(keys)
}

fn compiled_template_weight(template: &CompiledTemplate) -> Result<usize, IpfixError> {
    const ARC_ALLOCATION_OVERHEAD: usize = 32;
    const HASH_ENTRY_OVERHEAD: usize = 64;
    let counter_bytes = template
        .counters
        .len()
        .checked_mul(std::mem::size_of::<CompiledCounter>())
        .ok_or("compiled template size overflow")?;
    std::mem::size_of::<CompiledTemplate>()
        .checked_add(ARC_ALLOCATION_OVERHEAD)
        .and_then(|total| total.checked_add(HASH_ENTRY_OVERHEAD))
        .and_then(|total| total.checked_add(counter_bytes))
        .and_then(|total| total.checked_add(ARC_ALLOCATION_OVERHEAD))
        .ok_or_else(|| IpfixError::from("compiled template size overflow"))
}

struct IpfixMessages<'a> {
    remaining: &'a [u8],
    failed: bool,
}

impl<'a> IpfixMessages<'a> {
    fn new(bytes: &'a [u8]) -> Self {
        Self {
            remaining: bytes,
            failed: false,
        }
    }
}

impl<'a> Iterator for IpfixMessages<'a> {
    type Item = Result<&'a [u8], IpfixError>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.failed || self.remaining.is_empty() {
            return None;
        }
        if self.remaining.len() < IPFIX_HEADER_LEN {
            self.failed = true;
            return Some(Err(format!(
                "IPFIX payload has {} trailing bytes, shorter than the 16-byte header",
                self.remaining.len()
            )
            .into()));
        }
        let version = NetworkEndian::read_u16(&self.remaining[0..2]);
        if version != IPFIX_VERSION {
            self.failed = true;
            return Some(Err(format!("unsupported IPFIX version {version}").into()));
        }
        let len = NetworkEndian::read_u16(&self.remaining[2..4]) as usize;
        if len < IPFIX_HEADER_LEN {
            self.failed = true;
            return Some(Err(format!("invalid IPFIX message length {len}").into()));
        }
        if len > self.remaining.len() {
            self.failed = true;
            return Some(Err(format!(
                "IPFIX message length {len} exceeds remaining payload {}",
                self.remaining.len()
            )
            .into()));
        }
        let (message, remaining) = self.remaining.split_at(len);
        self.remaining = remaining;
        Some(Ok(message))
    }
}

fn next_set<'a>(message: &'a [u8], offset: &mut usize) -> Result<(u16, &'a [u8]), IpfixError> {
    if message.len().saturating_sub(*offset) < SET_HEADER_LEN {
        if message[*offset..].iter().all(|byte| *byte == 0) {
            *offset = message.len();
            return Ok((0, &message[message.len()..]));
        }
        return Err("IPFIX message has a truncated set header".into());
    }
    let set_id = NetworkEndian::read_u16(&message[*offset..*offset + 2]);
    let set_len = NetworkEndian::read_u16(&message[*offset + 2..*offset + 4]) as usize;
    if set_len <= SET_HEADER_LEN {
        return Err(format!("invalid IPFIX set length {set_len}").into());
    }
    let end = (*offset)
        .checked_add(set_len)
        .ok_or("IPFIX set length overflow")?;
    if end > message.len() {
        return Err(format!("IPFIX set length {set_len} exceeds message boundary").into());
    }
    let set = &message[*offset..end];
    *offset = end;
    Ok((set_id, set))
}

fn validate_data_set(template: &CompiledTemplate, set: &[u8]) -> Result<DataSetLayout, IpfixError> {
    if set.len() <= SET_HEADER_LEN {
        return Err(format!("data set {} contains no records", template.key.template_id).into());
    }
    let payload = &set[SET_HEADER_LEN..];
    let record_bytes = payload.len() / template.record_len * template.record_len;
    if record_bytes == 0 {
        return Err(format!(
            "data set {} is shorter than its {}-byte record",
            template.key.template_id, template.record_len
        )
        .into());
    }
    let padding = &payload[record_bytes..];
    if padding.len() >= template.record_len || padding.iter().any(|byte| *byte != 0) {
        return Err(format!(
            "data set {} has {} invalid trailing bytes",
            template.key.template_id,
            padding.len()
        )
        .into());
    }
    let record_count = record_bytes / template.record_len;
    let counter_count = record_count
        .checked_mul(template.counters.len())
        .ok_or_else(|| IpfixError::from("decoded counter count overflow"))?;
    Ok(DataSetLayout {
        record_bytes,
        record_count,
        counter_count,
    })
}

fn read_be_u64(bytes: &[u8]) -> u64 {
    debug_assert!((1..=8).contains(&bytes.len()));
    if bytes.len() == 8 {
        return NetworkEndian::read_u64(bytes);
    }
    bytes
        .iter()
        .fold(0u64, |value, byte| (value << 8) | u64::from(*byte))
}

fn compile_template_set(
    set: &[u8],
    domain: u32,
    owner: &Arc<str>,
    object_names: &HashMap<u16, Arc<str>>,
    output: &mut HashMap<TemplateKey, Arc<CompiledTemplate>>,
) -> Result<(), IpfixError> {
    let mut offset = SET_HEADER_LEN;
    while offset < set.len() {
        let remaining = &set[offset..];
        if remaining.len() < MIN_HFT_TEMPLATE_RECORD_LEN && remaining.iter().all(|byte| *byte == 0)
        {
            break;
        }
        if remaining.len() < 4 {
            return Err("template set has a truncated template record".into());
        }
        let template_id = NetworkEndian::read_u16(&set[offset..offset + 2]);
        let field_count = NetworkEndian::read_u16(&set[offset + 2..offset + 4]) as usize;
        offset += 4;
        if template_id < MIN_DATA_SET_ID {
            return Err(format!("reserved template ID {template_id}").into());
        }
        if field_count == 0 {
            return Err(format!(
                "template withdrawal/zero-field template {template_id} is outside the HFT profile"
            )
            .into());
        }

        let mut counters = Vec::with_capacity(field_count.saturating_sub(1));
        let mut observation_time_offset = None;
        let mut observation_fields = 0usize;
        let mut field_keys = HashSet::with_capacity(field_count);
        let mut record_len = 0usize;
        for _ in 0..field_count {
            if set.len() - offset < 4 {
                return Err(format!("template {template_id} has a truncated field").into());
            }
            let raw_id = NetworkEndian::read_u16(&set[offset..offset + 2]);
            let field_len = NetworkEndian::read_u16(&set[offset + 2..offset + 4]);
            offset += 4;
            let enterprise = raw_id & 0x8000 != 0;
            let field_id = raw_id & 0x7fff;
            if enterprise {
                if !(1..=8).contains(&field_len) {
                    return Err(format!(
                        "template {template_id} counter field {field_id} has unsupported length {field_len}; expected 1..=8 bytes"
                    )
                    .into());
                }
                if set.len() - offset < 4 {
                    return Err(format!(
                        "template {template_id} has a truncated enterprise number"
                    )
                    .into());
                }
                let enterprise_number = NetworkEndian::read_u32(&set[offset..offset + 4]);
                offset += 4;
                if enterprise_number == 0 {
                    return Err(format!(
                        "template {template_id} uses reserved enterprise number zero"
                    )
                    .into());
                }
                let object_name = object_names.get(&field_id).ok_or_else(|| {
                    format!("template {template_id} references unmapped object ID {field_id}")
                })?;
                let (type_id, stat_id) = decode_sai_ids(enterprise_number);
                if !field_keys.insert((field_id, Some(enterprise_number))) {
                    return Err(format!(
                        "template {template_id} contains a duplicate counter field"
                    )
                    .into());
                }
                counters.push(CompiledCounter {
                    offset: record_len,
                    len: u8::try_from(field_len).expect("counter length is at most 8"),
                    object_name: Arc::clone(object_name),
                    type_id,
                    stat_id,
                });
            } else if field_id == OBSERVATION_TIME_NANOSECONDS {
                if field_len != OBSERVATION_TIME_LEN {
                    return Err(format!(
                        "template {template_id} observation time has length {field_len}; expected {OBSERVATION_TIME_LEN}"
                    )
                    .into());
                }
                if !field_keys.insert((field_id, None)) {
                    return Err(format!(
                        "template {template_id} contains duplicate observation time fields"
                    )
                    .into());
                }
                observation_time_offset = Some(record_len);
                observation_fields += 1;
            } else {
                return Err(format!(
                    "template {template_id} contains unsupported standard IE {field_id}"
                )
                .into());
            }
            record_len = record_len
                .checked_add(usize::from(field_len))
                .ok_or("template record length overflow")?;
        }
        if observation_fields != 1 || counters.is_empty() {
            return Err(format!(
                "template {template_id} requires exactly one observation time and at least one counter"
            )
            .into());
        }
        validate_record_counter_count(template_id, counters.len())?;

        let key = TemplateKey {
            observation_domain_id: domain,
            template_id,
        };
        let template = Arc::new(CompiledTemplate {
            key,
            owner: Arc::clone(owner),
            observation_time_offset: observation_time_offset.expect("validated above"),
            counters: counters.into(),
            record_len,
        });
        if output.insert(key, template).is_some() {
            return Err(
                format!("duplicate template ({domain}, {template_id}) in one update").into(),
            );
        }
        if output.len() > MAX_TEMPLATES_PER_UPDATE {
            return Err(format!(
                "template update exceeds {} templates",
                MAX_TEMPLATES_PER_UPDATE
            )
            .into());
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::sync::mpsc::channel;

    fn template_message(
        key: &str,
        domain: u32,
        template_id: u16,
        fields: &[(u16, u32)],
    ) -> IPFixTemplatesMessage {
        let mut message = Vec::new();
        let message_len = IPFIX_HEADER_LEN + 12 + fields.len() * 8;
        let set_len = message_len - IPFIX_HEADER_LEN;
        message.extend_from_slice(&IPFIX_VERSION.to_be_bytes());
        message.extend_from_slice(&(message_len as u16).to_be_bytes());
        message.extend_from_slice(&0u32.to_be_bytes());
        message.extend_from_slice(&0u32.to_be_bytes());
        message.extend_from_slice(&domain.to_be_bytes());
        message.extend_from_slice(&TEMPLATE_SET_ID.to_be_bytes());
        message.extend_from_slice(&(set_len as u16).to_be_bytes());
        message.extend_from_slice(&template_id.to_be_bytes());
        message.extend_from_slice(&((fields.len() + 1) as u16).to_be_bytes());
        message.extend_from_slice(&OBSERVATION_TIME_NANOSECONDS.to_be_bytes());
        message.extend_from_slice(&OBSERVATION_TIME_LEN.to_be_bytes());
        for (label, enterprise) in fields {
            message.extend_from_slice(&(0x8000 | label).to_be_bytes());
            message.extend_from_slice(&8u16.to_be_bytes());
            message.extend_from_slice(&enterprise.to_be_bytes());
        }
        IPFixTemplatesMessage::new(
            key.to_string(),
            Arc::new(message),
            Some(
                fields
                    .iter()
                    .map(|(id, _)| format!("Ethernet{id}"))
                    .collect(),
            ),
            Some(fields.iter().map(|(id, _)| *id).collect()),
        )
    }

    fn data_message(domain: u32, sets: &[(u16, Vec<(u64, Vec<u64>)>)]) -> Vec<u8> {
        let message_len = IPFIX_HEADER_LEN
            + sets
                .iter()
                .map(|(_, records)| {
                    SET_HEADER_LEN
                        + records
                            .iter()
                            .map(|(_, values)| 8 + values.len() * 8)
                            .sum::<usize>()
                })
                .sum::<usize>();
        let mut message = Vec::with_capacity(message_len);
        message.extend_from_slice(&IPFIX_VERSION.to_be_bytes());
        message.extend_from_slice(&(message_len as u16).to_be_bytes());
        message.extend_from_slice(&0u32.to_be_bytes());
        message.extend_from_slice(&0u32.to_be_bytes());
        message.extend_from_slice(&domain.to_be_bytes());
        for (template_id, records) in sets {
            let set_len = SET_HEADER_LEN
                + records
                    .iter()
                    .map(|(_, values)| 8 + values.len() * 8)
                    .sum::<usize>();
            message.extend_from_slice(&template_id.to_be_bytes());
            message.extend_from_slice(&(set_len as u16).to_be_bytes());
            for (time, values) in records {
                message.extend_from_slice(&time.to_be_bytes());
                for value in values {
                    message.extend_from_slice(&value.to_be_bytes());
                }
            }
        }
        message
    }

    fn actor() -> IpfixActor {
        let (_, template_rx) = channel(4);
        let (_, record_rx) = channel(4);
        IpfixActor::new(template_rx, record_rx)
    }

    #[test]
    fn rejects_all_non_progress_message_lengths() {
        for len in 0u16..IPFIX_HEADER_LEN as u16 {
            let mut message = [0u8; IPFIX_HEADER_LEN];
            message[0..2].copy_from_slice(&IPFIX_VERSION.to_be_bytes());
            message[2..4].copy_from_slice(&len.to_be_bytes());
            assert!(
                IpfixMessages::new(&message).next().unwrap().is_err(),
                "length {len}"
            );
        }
    }

    #[test]
    fn decodes_hft_records_in_template_order_without_shared_state() {
        let mut actor = actor();
        actor
            .handle_template(template_message(
                "session",
                42,
                300,
                &[(2, 0x1234_0567), (1, 0x0001_0002)],
            ))
            .unwrap();
        let batch = actor
            .handle_record(&data_message(42, &[(300, vec![(7, vec![10, 20])])]))
            .unwrap();
        let record = batch.iter().next().unwrap();
        assert_eq!(record.observation_time, 7);
        assert_eq!(record.stats[0].object_name.as_ref(), "Ethernet2");
        assert_eq!(
            (record.stats[0].type_id, record.stats[0].stat_id),
            (0x1234, 0x0567)
        );
        assert_eq!(record.stats[1].object_name.as_ref(), "Ethernet1");
    }

    #[test]
    fn decodes_template_defined_counter_widths() {
        let mut message = template_message(
            "mixed-width",
            42,
            300,
            &[
                (1, 0x0001_0001),
                (2, 0x0001_0002),
                (3, 0x0001_0003),
                (4, 0x0001_0004),
                (5, 0x0001_0005),
                (6, 0x0001_0006),
                (7, 0x0001_0007),
                (8, 0x0001_0008),
            ],
        );
        let bytes = Arc::make_mut(message.templates.as_mut().unwrap());
        for (index, len) in (1u16..=8).enumerate() {
            let field_offset = 28 + index * 8;
            bytes[field_offset + 2..field_offset + 4].copy_from_slice(&len.to_be_bytes());
        }

        let mut actor = actor();
        actor.handle_template(message).unwrap();
        let mut data = Vec::new();
        let record = [
            0, 0, 0, 0, 0, 0, 0, 7,      // observation time
            0xabu8, // 8-bit
            0x01, 0x23, // 16-bit
            0x01, 0x23, 0x45, // 24-bit
            0x89, 0xab, 0xcd, 0xef, // 32-bit
            0x01, 0x23, 0x45, 0x67, 0x89, // 40-bit
            0x01, 0x23, 0x45, 0x67, 0x89, 0xab, // 48-bit
            0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, // 56-bit
            0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10, // 64-bit
        ];
        let message_len = IPFIX_HEADER_LEN + SET_HEADER_LEN + record.len();
        data.extend_from_slice(&IPFIX_VERSION.to_be_bytes());
        data.extend_from_slice(&(message_len as u16).to_be_bytes());
        data.extend_from_slice(&0u32.to_be_bytes());
        data.extend_from_slice(&0u32.to_be_bytes());
        data.extend_from_slice(&42u32.to_be_bytes());
        data.extend_from_slice(&300u16.to_be_bytes());
        data.extend_from_slice(&((SET_HEADER_LEN + record.len()) as u16).to_be_bytes());
        data.extend_from_slice(&record);

        let batch = actor.handle_record(&data).unwrap();
        let decoded = batch.iter().next().unwrap();
        assert_eq!(decoded.observation_time, 7);
        assert_eq!(
            decoded
                .stats
                .iter()
                .map(|stat| stat.counter)
                .collect::<Vec<_>>(),
            vec![
                0xab,
                0x01_23,
                0x01_23_45,
                0x89_ab_cd_ef,
                0x01_23_45_67_89,
                0x01_23_45_67_89_ab,
                0x01_23_45_67_89_ab_cd,
                0xfe_dc_ba_98_76_54_32_10,
            ]
        );
    }

    #[test]
    fn mixed_width_offsets_follow_template_order() {
        let mut message =
            template_message("offsets", 7, 300, &[(1, 0x0001_0001), (2, 0x0001_0002)]);
        let bytes = Arc::make_mut(message.templates.as_mut().unwrap());
        let original = bytes.clone();
        bytes[24..32].copy_from_slice(&original[28..36]);
        bytes[26..28].copy_from_slice(&3u16.to_be_bytes());
        bytes[32..40].copy_from_slice(&original[36..44]);
        bytes[34..36].copy_from_slice(&6u16.to_be_bytes());
        bytes[40..44].copy_from_slice(&original[24..28]);

        let generation = IpfixActor::compile_generation(&message).unwrap();
        let template = generation.templates.values().next().unwrap();
        assert_eq!(template.counters[0].offset, 0);
        assert_eq!(template.counters[1].offset, 3);
        assert_eq!(template.observation_time_offset, 9);
        assert_eq!(template.record_len, 17);
    }

    #[test]
    fn same_template_id_is_scoped_by_domain() {
        let mut actor = actor();
        actor
            .handle_template(template_message("a", 1, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_template(template_message("b", 2, 300, &[(2, 0x0003_0004)]))
            .unwrap();
        let mut bytes = data_message(1, &[(300, vec![(10, vec![11])])]);
        bytes.extend_from_slice(&data_message(2, &[(300, vec![(20, vec![22])])]));
        let batch = actor.handle_record(&bytes).unwrap();
        let records: Vec<_> = batch.iter().collect();
        assert_eq!(records[0].stats[0].object_name.as_ref(), "Ethernet1");
        assert_eq!(records[1].stats[0].object_name.as_ref(), "Ethernet2");
    }

    #[test]
    fn cross_owner_conflict_quarantines_every_incumbent() {
        let mut actor = actor();
        actor
            .handle_template(template_message("a", 1, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_template(template_message("b", 1, 301, &[(1, 0x0001_0003)]))
            .unwrap();

        let mut conflicting = template_message("c", 1, 300, &[(1, 0x0001_0004)]);
        let second = template_message("c", 1, 301, &[(1, 0x0001_0005)]);
        Arc::make_mut(conflicting.templates.as_mut().unwrap())
            .extend_from_slice(second.templates.as_ref().unwrap());
        assert!(actor.handle_template(conflicting).is_err());

        assert!(actor.installed.is_empty());
        assert!(!actor.sessions.contains_key("a"));
        assert!(!actor.sessions.contains_key("b"));
    }

    #[test]
    fn malformed_update_quarantines_active_generation() {
        let mut actor = actor();
        let valid = template_message("session", 0, 300, &[(1, 0x0001_0002)]);
        actor.handle_template(valid.clone()).unwrap();
        let mut invalid = valid;
        invalid.templates = Some(Arc::new(vec![0, 10, 0, 0]));
        assert!(actor.handle_template(invalid).is_err());
        assert!(actor.installed.is_empty());
        assert!(!actor.sessions.contains_key("session"));
    }

    #[test]
    fn promotes_new_ids_only_when_new_data_arrives() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0003)]))
            .unwrap();

        assert!(
            actor
                .handle_record(&data_message(0, &[(300, vec![(1, vec![2])])]))
                .unwrap()
                .record_count()
                == 1
        );
        assert!(actor.installed.contains_key(&TemplateKey {
            observation_domain_id: 0,
            template_id: 300
        }));
        assert!(
            actor
                .handle_record(&data_message(0, &[(301, vec![(2, vec![3])])]))
                .unwrap()
                .record_count()
                == 1
        );
        assert!(!actor.installed.contains_key(&TemplateKey {
            observation_domain_id: 0,
            template_id: 300
        }));
    }

    #[test]
    fn pending_cutover_drops_later_old_id_in_same_message() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0003)]))
            .unwrap();

        let batch = actor
            .handle_record(&data_message(
                0,
                &[(301, vec![(2, vec![20])]), (300, vec![(1, vec![10])])],
            ))
            .unwrap();
        let times: Vec<_> = batch.iter().map(|record| record.observation_time).collect();
        assert_eq!(times, vec![2]);
        assert!(!actor.installed.contains_key(&TemplateKey {
            observation_domain_id: 0,
            template_id: 300,
        }));
    }

    #[test]
    fn rejects_ambiguous_same_id_schema_change() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        assert!(actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0003)]))
            .is_err());
        assert!(actor.installed.is_empty());
        assert!(!actor.sessions.contains_key("session"));
        assert!(actor
            .handle_record(&data_message(0, &[(300, vec![(1, vec![10])])]))
            .unwrap()
            .is_empty());
    }

    #[test]
    fn identical_pending_refresh_is_idempotent() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        let pending = template_message("session", 0, 301, &[(1, 0x0001_0003)]);
        actor.handle_template(pending.clone()).unwrap();
        actor.handle_template(pending).unwrap();
        assert!(actor.sessions["session"].pending.is_some());
    }

    #[test]
    fn active_refresh_cancels_pending_generation() {
        let mut actor = actor();
        let active = template_message("session", 0, 300, &[(1, 0x0001_0002)]);
        actor.handle_template(active.clone()).unwrap();
        actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0003)]))
            .unwrap();
        actor.handle_template(active).unwrap();
        assert!(actor.sessions["session"].pending.is_none());
        assert!(!actor.installed.contains_key(&TemplateKey {
            observation_domain_id: 0,
            template_id: 301,
        }));
        assert!(actor.history.was_used(&TemplateKey {
            observation_domain_id: 0,
            template_id: 301,
        }));
    }

    #[test]
    fn newer_update_supersedes_pending_generation() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0003)]))
            .unwrap();
        actor
            .handle_template(template_message("session", 0, 302, &[(1, 0x0001_0004)]))
            .unwrap();
        assert!(!actor.installed.contains_key(&TemplateKey {
            observation_domain_id: 0,
            template_id: 301,
        }));
        assert!(actor.installed.contains_key(&TemplateKey {
            observation_domain_id: 0,
            template_id: 302,
        }));
    }

    #[test]
    fn rejected_supersession_quarantines_the_session() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0003)]))
            .unwrap();

        assert!(actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0004)]))
            .is_err());
        assert!(!actor.sessions.contains_key("session"));
        assert!(!actor.installed.contains_key(&TemplateKey {
            observation_domain_id: 0,
            template_id: 301,
        }));
    }

    #[test]
    fn overlapping_pending_supersession_is_atomic() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0003)]))
            .unwrap();

        let mut replacement = template_message("session", 0, 301, &[(1, 0x0001_0003)]);
        let extra = template_message("session", 0, 302, &[(1, 0x0001_0004)]);
        Arc::make_mut(replacement.templates.as_mut().unwrap())
            .extend_from_slice(extra.templates.as_ref().unwrap());
        actor.handle_template(replacement).unwrap();

        let pending = actor.sessions["session"].pending.as_ref().unwrap();
        assert!(pending.templates.contains_key(&TemplateKey {
            observation_domain_id: 0,
            template_id: 301,
        }));
        assert!(pending.templates.contains_key(&TemplateKey {
            observation_domain_id: 0,
            template_id: 302,
        }));
        assert!(actor.installed.contains_key(&TemplateKey {
            observation_domain_id: 0,
            template_id: 301,
        }));
    }

    #[test]
    fn overlapping_pending_supersession_rejects_changed_schema() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0003)]))
            .unwrap();

        assert!(actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0004)]))
            .is_err());
        assert!(!actor.sessions.contains_key("session"));
        assert!(actor.installed.is_empty());
    }

    #[test]
    fn delete_while_pending_retires_both_generations() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0003)]))
            .unwrap();
        actor.handle_template_deletion("session");
        assert!(actor.installed.is_empty());
        assert!(actor.history.was_used(&TemplateKey {
            observation_domain_id: 0,
            template_id: 300,
        }));
        assert!(actor.history.was_used(&TemplateKey {
            observation_domain_id: 0,
            template_id: 301,
        }));
    }

    #[test]
    fn unknown_set_does_not_drop_known_sets_and_is_replayed() {
        let mut actor = actor();
        actor
            .handle_template(template_message("known", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        let batch = actor
            .handle_record(&data_message(
                0,
                &[
                    (300, vec![(1, vec![10])]),
                    (301, vec![(2, vec![20])]),
                    (300, vec![(3, vec![30])]),
                ],
            ))
            .unwrap();
        assert_eq!(batch.record_count(), 1);
        assert_eq!(batch.iter().next().unwrap().observation_time, 1);
        assert_eq!(actor.deferred_sets.sets.len(), 2);

        let replayed = actor
            .handle_template(template_message("late", 0, 301, &[(1, 0x0001_0002)]))
            .unwrap();
        assert_eq!(replayed.record_count(), 2);
        let replayed_times: Vec<_> = replayed
            .iter()
            .map(|record| record.observation_time)
            .collect();
        assert_eq!(replayed_times, vec![2, 3]);
    }

    #[test]
    fn malformed_deferred_set_does_not_promote_pending_generation() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();

        let future = data_message(0, &[(301, vec![(2, vec![20])])]);
        actor.handle_record(&future).unwrap();
        let buffered = actor.deferred_sets.sets.front_mut().unwrap();
        let mut malformed = buffered.bytes.to_vec();
        malformed.truncate(SET_HEADER_LEN + 8);
        malformed[2..4].copy_from_slice(&((SET_HEADER_LEN + 8) as u16).to_be_bytes());
        buffered.bytes = malformed.into();

        actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0003)]))
            .unwrap();
        assert!(actor.installed.contains_key(&TemplateKey {
            observation_domain_id: 0,
            template_id: 300,
        }));
        assert!(actor.sessions["session"].pending.is_some());
    }

    #[test]
    fn delete_drops_late_data_and_rejects_changed_schema_reuse() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor.handle_template_deletion("session");

        let late = actor
            .handle_record(&data_message(0, &[(300, vec![(1, vec![99])])]))
            .unwrap();
        assert!(late.is_empty());
        assert!(actor.deferred_sets.sets.is_empty());

        assert!(actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0003)]))
            .is_err());
        assert!(actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .is_err());
    }

    #[test]
    fn delete_and_rejoin_rejects_an_identical_schema() {
        let mut actor = actor();
        let template = template_message("session", 0, 300, &[(1, 0x0001_0002)]);
        actor.handle_template(template.clone()).unwrap();
        actor.handle_template_deletion("session");

        assert!(actor.handle_template(template).is_err());
        assert!(actor
            .handle_record(&data_message(0, &[(300, vec![(2, vec![20])])]))
            .unwrap()
            .is_empty());
    }

    #[test]
    fn identical_schema_rejoin_with_a_new_session_name_is_rejected() {
        let mut actor = actor();
        actor
            .handle_template(template_message("old", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor.handle_template_deletion("old");

        assert!(actor
            .handle_template(template_message("new", 0, 300, &[(1, 0x0001_0002)]))
            .is_err());
    }

    #[test]
    fn deleted_history_does_not_consume_the_installed_template_limit() {
        let mut actor = actor();
        for offset in 0..MAX_INSTALLED_TEMPLATE_KEYS + 1 {
            let template_id =
                MIN_DATA_SET_ID + u16::try_from(offset).expect("test template ID must fit in u16");
            actor
                .handle_template(template_message(
                    "session",
                    0,
                    template_id,
                    &[(1, 0x0001_0002)],
                ))
                .unwrap();
            let session = actor.sessions.remove("session").unwrap();
            for generation in [session.active, session.pending].into_iter().flatten() {
                actor.live_template_bytes -= generation.compiled_bytes;
                for template in generation.templates.values() {
                    actor.installed.remove(&template.key);
                    actor.history.retire(template);
                }
            }
        }

        assert!(actor.installed.is_empty());
        assert_eq!(actor.history.used_ids.len(), 1);
        assert!(actor.history.was_used(&TemplateKey {
            observation_domain_id: 0,
            template_id: MIN_DATA_SET_ID + MAX_INSTALLED_TEMPLATE_KEYS as u16,
        }));
        assert!(actor
            .handle_template(template_message(
                "oldest",
                0,
                MIN_DATA_SET_ID,
                &[(1, 0x0001_0002)],
            ))
            .is_err());
    }

    #[test]
    fn delete_boundary_rejects_new_generation_in_the_same_domain() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor.handle_template_deletion("session");
        actor
            .handle_record(&data_message(0, &[(301, vec![(1, vec![10])])]))
            .unwrap();
        assert_eq!(actor.deferred_sets.sets.len(), 1);

        assert!(actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0003)]))
            .is_err());
        assert!(actor.deferred_sets.sets.is_empty());
    }

    #[test]
    fn delete_domain_fence_survives_owner_change() {
        let mut actor = actor();
        actor
            .handle_template(template_message("old", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_record(&data_message(0, &[(301, vec![(1, vec![10])])]))
            .unwrap();
        actor.handle_template_deletion("old");

        assert!(actor
            .handle_template(template_message("new", 0, 301, &[(1, 0x0001_0003)]))
            .is_err());
        assert!(actor.deferred_sets.sets.is_empty());
    }

    #[test]
    fn malformed_trailer_does_not_promote_or_emit_valid_prefix() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0003)]))
            .unwrap();
        let mut input = data_message(0, &[(301, vec![(2, vec![20])])]);
        input.extend_from_slice(&[0; IPFIX_HEADER_LEN - 1]);

        assert!(actor.handle_record(&input).is_err());
        assert!(actor.sessions["session"].pending.is_some());
        assert!(actor.installed.contains_key(&TemplateKey {
            observation_domain_id: 0,
            template_id: 300,
        }));
    }

    #[test]
    fn invalid_metadata_quarantine_removes_conflicting_incumbent() {
        let mut actor = actor();
        actor
            .handle_template(template_message("incumbent", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        let raw = template_message("invalid", 0, 300, &[(1, 0x0001_0003)])
            .templates
            .unwrap();

        actor
            .handle_template(IPFixTemplatesMessage::quarantine(
                "invalid".to_string(),
                Some(raw),
            ))
            .unwrap();
        assert!(actor.installed.is_empty());
        assert!(!actor.sessions.contains_key("incumbent"));
    }

    #[test]
    fn unidentifiable_quarantine_requests_restart() {
        let mut actor = actor();
        actor
            .handle_template(template_message("incumbent", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        let error = actor
            .handle_template(IPFixTemplatesMessage::quarantine(
                "invalid".to_string(),
                Some(Arc::new(vec![1, 2, 3])),
            ))
            .unwrap_err();
        assert!(is_restart_required(&error));
    }

    #[test]
    fn delete_preserves_preinstall_lifecycle_boundary() {
        let mut actor = actor();
        actor.handle_template_deactivation("session");
        actor
            .handle_record(&data_message(0, &[(300, vec![(1, vec![10])])]))
            .unwrap();
        actor.handle_template_deletion("session");

        let replayed = actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        assert!(replayed.is_empty());
        assert!(actor.deferred_sets.sets.is_empty());
    }

    #[test]
    fn unknown_lifecycle_events_use_bounded_marker_state() {
        let mut actor = actor();
        for index in 0..MAX_DELETED_SESSION_MARKERS + 1 {
            actor.handle_template_deactivation(&format!("deactivate-{index}"));
            actor.handle_template_deletion(&format!("delete-{index}"));
        }

        assert!(actor.sessions.is_empty());
        assert!(actor.lifecycle_markers.drop_all_preinstall);
        assert!(actor.lifecycle_markers.owners.is_empty());
        assert_eq!(actor.lifecycle_markers.bytes, 0);
    }

    #[test]
    fn capacity_rejection_does_not_poison_a_fresh_id() {
        let mut actor = actor();
        actor.live_template_bytes = MAX_LIVE_TEMPLATE_BYTES;
        let candidate = template_message("candidate", 0, 6000, &[(1, 0x0001_0002)]);

        assert!(actor.handle_template(candidate.clone()).is_err());
        assert!(!actor.history.was_used(&TemplateKey {
            observation_domain_id: 0,
            template_id: 6000,
        }));

        actor.live_template_bytes = 0;
        actor.handle_template(candidate).unwrap();
    }

    #[test]
    fn live_template_byte_limit_is_transactional() {
        let mut actor = actor();
        let active = template_message("session", 0, 300, &[(1, 0x0001_0002)]);
        actor.handle_template(active).unwrap();
        let bytes_before = actor.live_template_bytes;
        actor.live_template_bytes = MAX_LIVE_TEMPLATE_BYTES;

        assert!(actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0003)]))
            .is_err());
        assert!(actor.sessions["session"].pending.is_none());
        assert!(actor.installed.contains_key(&TemplateKey {
            observation_domain_id: 0,
            template_id: 300,
        }));
        assert!(!actor.history.was_used(&TemplateKey {
            observation_domain_id: 0,
            template_id: 301,
        }));
        actor.live_template_bytes = bytes_before;
        actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0003)]))
            .unwrap();
        assert!(actor.sessions["session"].pending.is_some());
    }

    #[test]
    fn admission_limit_quarantines_affected_wire_keys() {
        let mut actor = actor();
        let active = template_message("session", 0, 300, &[(1, 0x0001_0002)]);
        actor.handle_template(active.clone()).unwrap();
        let mut oversized = active.clone();
        oversized.object_names = Some(vec!["x".repeat(MAX_OBJECT_METADATA_BYTES + 1)]);

        assert!(actor.handle_template(oversized).is_err());
        assert!(!actor.sessions.contains_key("session"));
        assert!(actor
            .handle_record(&data_message(0, &[(300, vec![(1, vec![10])])]))
            .unwrap()
            .is_empty());

        assert!(actor.handle_template(active).is_err());
    }

    #[test]
    fn pending_replacement_projects_retired_keys_before_capacity_check() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0003)]))
            .unwrap();

        for offset in 0..MAX_INSTALLED_TEMPLATE_KEYS - 2 {
            let key = TemplateKey {
                observation_domain_id: 1,
                template_id: MIN_DATA_SET_ID + u16::try_from(offset).unwrap(),
            };
            actor.installed.insert(
                key,
                Arc::new(CompiledTemplate {
                    key,
                    owner: Arc::from("filler"),
                    observation_time_offset: 0,
                    counters: Arc::from([]),
                    record_len: 8,
                }),
            );
        }
        assert_eq!(actor.installed.len(), MAX_INSTALLED_TEMPLATE_KEYS);

        actor
            .handle_template(template_message("session", 0, 302, &[(1, 0x0001_0004)]))
            .unwrap();
        assert_eq!(actor.installed.len(), MAX_INSTALLED_TEMPLATE_KEYS);
        assert!(!actor.installed.contains_key(&TemplateKey {
            observation_domain_id: 0,
            template_id: 301,
        }));
        assert!(actor.installed.contains_key(&TemplateKey {
            observation_domain_id: 0,
            template_id: 302,
        }));
    }

    #[test]
    fn too_many_unresolved_sets_are_rejected_before_mutation() {
        let mut message = data_message(
            0,
            &(0..=MAX_UNRESOLVED_SETS_PER_RECORD_INPUT)
                .map(|index| {
                    (
                        MIN_DATA_SET_ID + u16::try_from(index).unwrap(),
                        vec![(1, vec![])],
                    )
                })
                .collect::<Vec<_>>(),
        );
        // Unknown Sets need no valid record layout; shrink each to one byte.
        let mut offset = IPFIX_HEADER_LEN;
        while offset < message.len() {
            let set_len = NetworkEndian::read_u16(&message[offset + 2..offset + 4]) as usize;
            message.drain(offset + SET_HEADER_LEN + 1..offset + set_len);
            message[offset + 2..offset + 4]
                .copy_from_slice(&((SET_HEADER_LEN + 1) as u16).to_be_bytes());
            offset += SET_HEADER_LEN + 1;
        }
        let message_len = u16::try_from(message.len()).unwrap();
        message[2..4].copy_from_slice(&message_len.to_be_bytes());

        let mut actor = actor();
        assert!(actor.handle_record(&message).is_err());
        assert!(actor.deferred_sets.sets.is_empty());
        assert_eq!(actor.deferred_sets.dropped, 0);
        assert_eq!(actor.next_deferred_sequence, 1);
    }

    #[test]
    fn full_deferred_buffer_does_not_drop_known_other_domain() {
        let mut actor = actor();
        actor
            .handle_template(template_message("known", 1, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        for index in 0..MAX_UNKNOWN_SETS {
            actor
                .handle_record(&data_message(
                    0,
                    &[(
                        MIN_DATA_SET_ID + u16::try_from(index).unwrap(),
                        vec![(1, vec![])],
                    )],
                ))
                .unwrap();
        }
        let dropped_before = actor.deferred_sets.dropped;

        let mut input = data_message(0, &[(600, vec![(2, vec![])])]);
        input.extend_from_slice(&data_message(1, &[(300, vec![(3, vec![30])])]));
        let batch = actor.handle_record(&input).unwrap();
        assert_eq!(batch.record_count(), 1);
        assert_eq!(batch.iter().next().unwrap().observation_time, 3);
        assert_eq!(actor.deferred_sets.dropped, dropped_before + 1);
    }

    #[test]
    fn deactivate_then_reactivate_identical_generation() {
        let mut actor = actor();
        let template = template_message("session", 0, 300, &[(1, 0x0001_0002)]);
        actor.handle_template(template.clone()).unwrap();
        actor
            .handle_template(IPFixTemplatesMessage::deactivate("session".to_string()))
            .unwrap();

        let disabled = actor
            .handle_record(&data_message(0, &[(300, vec![(1, vec![10])])]))
            .unwrap();
        assert!(disabled.is_empty());
        assert!(actor.installed.contains_key(&TemplateKey {
            observation_domain_id: 0,
            template_id: 300,
        }));
        assert!(actor.history.was_used(&TemplateKey {
            observation_domain_id: 0,
            template_id: 300,
        }));

        actor.handle_template(template).unwrap();
        let enabled = actor
            .handle_record(&data_message(0, &[(300, vec![(2, vec![20])])]))
            .unwrap();
        assert_eq!(enabled.record_count(), 1);
    }

    #[test]
    fn rejected_update_does_not_reactivate_stale_generation() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_template(IPFixTemplatesMessage::deactivate("session".to_string()))
            .unwrap();

        assert!(actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0003)]))
            .is_err());
        assert!(!actor.sessions.contains_key("session"));
        assert!(actor.installed.is_empty());
        assert!(actor
            .handle_record(&data_message(0, &[(300, vec![(1, vec![10])])]))
            .unwrap()
            .is_empty());
    }

    #[test]
    fn reactivation_drops_data_received_while_disabled() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_template(IPFixTemplatesMessage::deactivate("session".to_string()))
            .unwrap();
        actor
            .handle_record(&data_message(0, &[(301, vec![(1, vec![10])])]))
            .unwrap();
        assert_eq!(actor.deferred_sets.sets.len(), 1);

        let replayed = actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0003)]))
            .unwrap();
        assert!(replayed.is_empty());
        assert!(actor.deferred_sets.sets.is_empty());

        let live = actor
            .handle_record(&data_message(0, &[(301, vec![(2, vec![20])])]))
            .unwrap();
        assert_eq!(live.record_count(), 1);
        assert_eq!(live.iter().next().unwrap().observation_time, 2);
    }

    #[test]
    fn first_activation_drops_data_received_after_preinstall_deactivation() {
        let mut actor = actor();
        actor
            .handle_template(IPFixTemplatesMessage::deactivate("session".to_string()))
            .unwrap();
        actor
            .handle_record(&data_message(0, &[(300, vec![(1, vec![10])])]))
            .unwrap();
        assert_eq!(actor.deferred_sets.sets.len(), 1);

        let replayed = actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        assert!(replayed.is_empty());
        assert!(actor.deferred_sets.sets.is_empty());

        let live = actor
            .handle_record(&data_message(0, &[(300, vec![(2, vec![20])])]))
            .unwrap();
        assert_eq!(live.record_count(), 1);
        assert_eq!(live.iter().next().unwrap().observation_time, 2);
    }

    #[test]
    fn stale_unknown_data_does_not_cross_intermediate_reactivation() {
        let mut actor = actor();
        let active = template_message("session", 0, 300, &[(1, 0x0001_0002)]);
        actor.handle_template(active.clone()).unwrap();
        actor.handle_template_deactivation("session");
        actor
            .handle_record(&data_message(0, &[(301, vec![(1, vec![10])])]))
            .unwrap();

        actor.handle_template(active).unwrap();
        assert!(actor.deferred_sets.sets.is_empty());
        let replayed = actor
            .handle_template(template_message("session", 0, 301, &[(1, 0x0001_0003)]))
            .unwrap();
        assert!(replayed.is_empty());
        assert!(actor.deferred_sets.sets.is_empty());

        let live = actor
            .handle_record(&data_message(0, &[(301, vec![(2, vec![20])])]))
            .unwrap();
        assert_eq!(live.record_count(), 1);
        assert_eq!(live.iter().next().unwrap().observation_time, 2);
    }

    #[tokio::test(start_paused = true)]
    async fn resolved_deferred_sets_do_not_expire_between_batches() {
        let mut actor = actor();
        actor.deferred_set_ttl = Duration::from_millis(20);
        let counters = MAX_COUNTERS_PER_BATCH - 4;
        let fields = (1..=counters)
            .map(|label| {
                (
                    u16::try_from(label).expect("counter label fits u16"),
                    0x0001_0002,
                )
            })
            .collect::<Vec<_>>();
        let values = vec![1; counters];

        actor
            .handle_record(&data_message(0, &[(300, vec![(1, values.clone())])]))
            .unwrap();
        actor
            .handle_record(&data_message(0, &[(300, vec![(2, values)])]))
            .unwrap();
        tokio::time::advance(Duration::from_millis(19)).await;

        let first = actor
            .handle_template(template_message("late", 0, 300, &fields))
            .unwrap();
        assert_eq!(first.counter_count(), counters);
        assert_eq!(actor.deferred_sets.sets.len(), 1);
        assert!(actor.deferred_sets.sets[0].expires_at.is_none());

        tokio::time::advance(Duration::from_secs(1)).await;
        let mut second = SAIStatsBatch::default();
        actor
            .drain_deferred_sets(&mut second, Instant::now())
            .unwrap();
        assert_eq!(second.counter_count(), counters);
        assert!(actor.deferred_sets.sets.is_empty());
        assert_eq!(actor.deferred_sets.dropped, 0);
    }

    #[test]
    fn malformed_disabled_set_does_not_drop_enabled_telemetry() {
        let mut actor = actor();
        actor
            .handle_template(template_message("disabled", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_template(template_message("enabled", 0, 301, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_template(IPFixTemplatesMessage::deactivate("disabled".to_string()))
            .unwrap();

        let batch = actor
            .handle_record(&data_message(
                0,
                &[(300, vec![(1, vec![])]), (301, vec![(2, vec![20])])],
            ))
            .unwrap();
        assert_eq!(batch.record_count(), 1);
        assert_eq!(batch.iter().next().unwrap().observation_time, 2);
    }

    #[test]
    fn unknown_template_does_not_block_known_other_domain() {
        let mut actor = actor();
        actor
            .handle_template(template_message("known", 2, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        assert!(actor
            .handle_record(&data_message(1, &[(301, vec![(1, vec![10])])]))
            .unwrap()
            .is_empty());
        let known = actor
            .handle_record(&data_message(2, &[(300, vec![(2, vec![20])])]))
            .unwrap();
        assert_eq!(known.record_count(), 1);
        assert_eq!(actor.deferred_sets.sets.len(), 1);
    }

    #[test]
    fn expired_unknown_does_not_drop_known_telemetry() {
        let mut actor = actor();
        actor.deferred_set_ttl = Duration::ZERO;
        actor
            .handle_template(template_message("known", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_record(&data_message(0, &[(301, vec![(1, vec![10])])]))
            .unwrap();
        let known = actor
            .handle_record(&data_message(0, &[(300, vec![(2, vec![20])])]))
            .unwrap();
        assert_eq!(known.record_count(), 1);
        assert!(actor.deferred_sets.sets.is_empty());
    }

    #[test]
    fn rejected_first_install_does_not_create_empty_session() {
        let mut actor = actor();
        actor
            .handle_template(template_message("original", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor.handle_template_deletion("original");

        for index in 0..100 {
            assert!(actor
                .handle_template(template_message(
                    &format!("rejected-{index}"),
                    0,
                    300,
                    &[(1, 0x0001_0003)],
                ))
                .is_err());
        }
        assert!(actor.sessions.is_empty());
        assert!(actor.lifecycle_markers.requires_fresh_data("original"));
    }

    #[test]
    fn malformed_later_set_does_not_emit_earlier_set() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        let mut message =
            data_message(0, &[(300, vec![(1, vec![10])]), (300, vec![(2, vec![20])])]);
        let second_set_offset = IPFIX_HEADER_LEN + SET_HEADER_LEN + 16;
        message[second_set_offset + 2..second_set_offset + 4].copy_from_slice(&3u16.to_be_bytes());
        assert!(actor.handle_record(&message).is_err());
    }

    #[test]
    fn arbitrary_inputs_never_panic() {
        let mut actor = actor();
        let mut state = 0x0123_4567_89ab_cdefu64;
        for len in 0..512usize {
            let mut bytes = vec![0u8; len];
            for byte in &mut bytes {
                state ^= state << 13;
                state ^= state >> 7;
                state ^= state << 17;
                *byte = state as u8;
            }
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let _ = actor.handle_record(&bytes);
            }));
            assert!(result.is_ok(), "panicked for arbitrary input length {len}");
        }
    }

    #[test]
    fn framing_and_padding_errors_are_rejected() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();

        for set_len in 0u16..SET_HEADER_LEN as u16 {
            let mut message = data_message(0, &[(300, vec![(1, vec![2])])]);
            message[18..20].copy_from_slice(&set_len.to_be_bytes());
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                actor.handle_record(&message)
            }));
            assert!(result.is_ok(), "set length {set_len} panicked");
            assert!(result.unwrap().is_err(), "set length {set_len}");
        }

        let mut truncated = data_message(0, &[(300, vec![(1, vec![2])])]);
        truncated[18..20].copy_from_slice(&u16::MAX.to_be_bytes());
        assert!(actor.handle_record(&truncated).is_err());

        let mut bad_padding = data_message(0, &[(300, vec![(1, vec![2])])]);
        bad_padding.extend_from_slice(&[1]);
        let message_len = bad_padding.len() as u16;
        let set_len = (bad_padding.len() - IPFIX_HEADER_LEN) as u16;
        bad_padding[2..4].copy_from_slice(&message_len.to_be_bytes());
        bad_padding[18..20].copy_from_slice(&set_len.to_be_bytes());
        assert!(actor.handle_record(&bad_padding).is_err());
    }

    #[test]
    fn zero_message_padding_is_ignored_without_losing_records() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        let mut message = data_message(0, &[(300, vec![(1, vec![2])])]);
        message.extend_from_slice(&[0, 0, 0]);
        let message_len = message.len() as u16;
        message[2..4].copy_from_slice(&message_len.to_be_bytes());
        assert_eq!(actor.handle_record(&message).unwrap().record_count(), 1);
    }

    #[test]
    fn template_set_accepts_four_bytes_of_zero_padding() {
        let mut message = template_message("session", 0, 300, &[(1, 0x0001_0002)]);
        let bytes = Arc::make_mut(message.templates.as_mut().unwrap());
        bytes.extend_from_slice(&[0; 4]);
        let message_len = bytes.len() as u16;
        let set_len = (bytes.len() - IPFIX_HEADER_LEN) as u16;
        bytes[2..4].copy_from_slice(&message_len.to_be_bytes());
        bytes[18..20].copy_from_slice(&set_len.to_be_bytes());

        assert_eq!(
            IpfixActor::compile_generation(&message)
                .unwrap()
                .templates
                .len(),
            1
        );
    }

    #[test]
    fn data_set_padding_may_exceed_three_bytes() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        let mut message = data_message(0, &[(300, vec![(1, vec![2])])]);
        message.extend_from_slice(&[0; 8]);
        let message_len = message.len() as u16;
        let set_len = (message.len() - IPFIX_HEADER_LEN) as u16;
        message[2..4].copy_from_slice(&message_len.to_be_bytes());
        message[18..20].copy_from_slice(&set_len.to_be_bytes());
        assert_eq!(actor.handle_record(&message).unwrap().record_count(), 1);
    }

    #[test]
    fn rejects_non_hft_fields_and_bad_object_ids() {
        let mut message = template_message("session", 0, 300, &[(1, 0x0001_0002)]);
        message.object_ids = Some(vec![0]);
        assert!(IpfixActor::compile_generation(&message).is_err());

        let bytes = Arc::make_mut(message.templates.as_mut().unwrap());
        bytes[24..26].copy_from_slice(&322u16.to_be_bytes());
        message.object_ids = Some(vec![1]);
        assert!(IpfixActor::compile_generation(&message).is_err());
    }

    #[test]
    fn rejects_zero_and_oversized_counter_widths() {
        for len in [0u16, 9] {
            let mut message = template_message("session", 0, 300, &[(1, 0x0001_0002)]);
            Arc::make_mut(message.templates.as_mut().unwrap())[30..32]
                .copy_from_slice(&len.to_be_bytes());
            assert!(IpfixActor::compile_generation(&message).is_err());
        }

        let mut message = template_message("session", 0, 300, &[(1, 0x0001_0002)]);
        Arc::make_mut(message.templates.as_mut().unwrap())[26..28]
            .copy_from_slice(&4u16.to_be_bytes());
        assert!(IpfixActor::compile_generation(&message).is_err());
    }

    #[tokio::test]
    async fn sends_one_batch_to_every_recipient() {
        let (template_tx, template_rx) = channel(2);
        let (record_tx, record_rx) = channel(2);
        let (sink_a_tx, mut sink_a_rx) = channel(1);
        let (sink_b_tx, mut sink_b_rx) = channel(1);
        let mut actor = IpfixActor::new(template_rx, record_rx);
        actor.add_recipient(sink_a_tx);
        actor.add_recipient(sink_b_tx);
        let task = tokio::spawn(IpfixActor::run(actor));

        template_tx
            .send(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .await
            .unwrap();
        record_tx
            .send(Arc::new(data_message(
                0,
                &[(300, vec![(1, vec![2]), (3, vec![4])])],
            )))
            .await
            .unwrap();

        let a = sink_a_rx.recv().await.unwrap();
        let b = sink_b_rx.recv().await.unwrap();
        assert!(Arc::ptr_eq(&a, &b));
        assert_eq!(a.record_count(), 2);
        drop(template_tx);
        drop(record_tx);
        assert!(task.await.unwrap().is_err());
    }

    #[tokio::test]
    async fn full_recipient_does_not_delay_available_recipient() {
        let (_, template_rx) = channel(1);
        let (_, record_rx) = channel(1);
        let (blocked_tx, mut blocked_rx) = channel(1);
        let (ready_tx, mut ready_rx) = channel(1);
        let mut actor = IpfixActor::new(template_rx, record_rx);
        actor.add_recipient(blocked_tx.clone());
        actor.add_recipient(ready_tx);
        blocked_tx
            .send(Arc::new(SAIStatsBatch::default()))
            .await
            .unwrap();

        let mut batch = SAIStatsBatch::default();
        batch.push_record(1, [SAIStat::new("Ethernet0", 1, 2, 3)]);
        let send = actor.send_batch(batch);
        tokio::pin!(send);

        tokio::select! {
            ready = ready_rx.recv() => assert_eq!(ready.unwrap().record_count(), 1),
            result = &mut send => panic!("send completed before blocked recipient drained: {result:?}"),
        }
        assert!(blocked_rx.recv().await.is_some());
        assert!(send.await.is_ok());
        assert_eq!(blocked_rx.recv().await.unwrap().record_count(), 1);
    }

    #[tokio::test]
    async fn closed_recipient_is_reported_after_healthy_delivery() {
        let (_, template_rx) = channel(1);
        let (_, record_rx) = channel(1);
        let (closed_tx, closed_rx) = channel(1);
        let (healthy_tx, mut healthy_rx) = channel(1);
        drop(closed_rx);
        let mut actor = IpfixActor::new(template_rx, record_rx);
        actor.add_recipient(healthy_tx);
        actor.add_recipient(closed_tx);

        let mut batch = SAIStatsBatch::default();
        batch.push_record(1, [SAIStat::new("Ethernet0", 1, 2, 3)]);
        let error = actor.send_batch(batch).await.unwrap_err();
        assert!(error.to_string().contains("1 SAI stats recipient"));
        assert_eq!(healthy_rx.recv().await.unwrap().record_count(), 1);
    }

    #[tokio::test]
    async fn closed_recipient_before_healthy_recipient_still_delivers() {
        let (_, template_rx) = channel(1);
        let (_, record_rx) = channel(1);
        let (closed_tx, closed_rx) = channel(1);
        let (healthy_tx, mut healthy_rx) = channel(1);
        drop(closed_rx);
        let mut actor = IpfixActor::new(template_rx, record_rx);
        actor.add_recipient(closed_tx);
        actor.add_recipient(healthy_tx);

        let mut batch = SAIStatsBatch::default();
        batch.push_record(1, [SAIStat::new("Ethernet0", 1, 2, 3)]);
        assert!(actor.send_batch(batch).await.is_err());
        assert_eq!(healthy_rx.recv().await.unwrap().record_count(), 1);
    }

    #[tokio::test]
    async fn send_batch_splits_only_at_record_boundaries() {
        let (_, template_rx) = channel(1);
        let (_, record_rx) = channel(1);
        let (stats_tx, mut stats_rx) = channel(2);
        let mut actor = IpfixActor::new(template_rx, record_rx);
        actor.add_recipient(stats_tx);
        let stat = SAIStat::new("Ethernet0", 1, 2, 3);
        let mut batch = SAIStatsBatch::default();
        for observation_time in 1..=3 {
            batch.push_record(
                observation_time,
                std::iter::repeat(stat.clone()).take(4_000),
            );
        }

        actor.send_batch(batch).await.unwrap();
        let first = stats_rx.recv().await.unwrap();
        let second = stats_rx.recv().await.unwrap();
        assert_eq!(first.counter_count(), 8_000);
        assert_eq!(first.record_count(), 2);
        assert_eq!(second.counter_count(), 4_000);
        assert_eq!(second.iter().next().unwrap().observation_time, 3);
    }

    #[tokio::test]
    async fn split_batch_reaches_healthy_recipient_after_closed_sink() {
        let (_, template_rx) = channel(1);
        let (_, record_rx) = channel(1);
        let (closed_tx, closed_rx) = channel(1);
        let (healthy_tx, mut healthy_rx) = channel(2);
        drop(closed_rx);
        let mut actor = IpfixActor::new(template_rx, record_rx);
        actor.add_recipient(closed_tx);
        actor.add_recipient(healthy_tx);
        let mut batch = SAIStatsBatch::default();
        for observation_time in 1..=3 {
            batch.push_record(
                observation_time,
                (0..4_000).map(|counter| SAIStat::new("Ethernet0", 1, counter, u64::from(counter))),
            );
        }

        assert!(actor.send_batch(batch).await.is_err());
        assert_eq!(healthy_rx.recv().await.unwrap().record_count(), 2);
        assert_eq!(healthy_rx.recv().await.unwrap().record_count(), 1);
    }

    #[test]
    fn rejects_one_logical_record_above_the_batch_limit() {
        let error = validate_record_counter_count(300, MAX_COUNTERS_PER_BATCH + 1).unwrap_err();
        assert!(error.to_string().contains("8193"));
        assert!(error.to_string().contains("8192"));
    }

    #[tokio::test]
    async fn resource_limit_rejection_keeps_the_actor_running() {
        let (template_tx, template_rx) = channel(1);
        let (record_tx, record_rx) = channel(1);
        let mut actor = IpfixActor::new(template_rx, record_rx);
        actor.live_template_bytes = MAX_LIVE_TEMPLATE_BYTES;
        let task = tokio::spawn(IpfixActor::run(actor));

        template_tx
            .send(template_message("candidate", 0, 6000, &[(1, 0x0001_0002)]))
            .await
            .unwrap();
        tokio::task::yield_now().await;
        assert!(!task.is_finished());
        drop(record_tx);
        assert!(task.await.unwrap().is_err());
    }

    #[tokio::test(start_paused = true)]
    async fn deferred_timer_expires_unknown_without_new_input() {
        let (template_tx, template_rx) = channel(1);
        let (record_tx, record_rx) = channel(1);
        let (stats_tx, mut stats_rx) = channel(1);
        let mut actor = IpfixActor::new(template_rx, record_rx);
        actor.deferred_set_ttl = Duration::from_millis(20);
        actor.add_recipient(stats_tx);
        actor
            .handle_template(template_message("known", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        actor
            .handle_template(template_message("barrier", 1, 302, &[(1, 0x0001_0002)]))
            .unwrap();
        let task = tokio::spawn(IpfixActor::run(actor));

        record_tx
            .send(Arc::new(data_message(
                0,
                &[(301, vec![(1, vec![10])]), (300, vec![(2, vec![20])])],
            )))
            .await
            .unwrap();
        record_tx
            .send(Arc::new(data_message(1, &[(302, vec![(3, vec![30])])])))
            .await
            .unwrap();
        let barrier = stats_rx.recv().await.unwrap();
        assert_eq!(barrier.record_count(), 1);
        assert_eq!(barrier.iter().next().unwrap().observation_time, 3);

        tokio::time::advance(Duration::from_millis(19)).await;
        tokio::task::yield_now().await;
        assert!(stats_rx.try_recv().is_err());

        tokio::time::advance(Duration::from_millis(1)).await;
        tokio::task::yield_now().await;
        let released = stats_rx.try_recv().unwrap();
        assert_eq!(released.record_count(), 1);
        assert_eq!(released.iter().next().unwrap().observation_time, 2);

        template_tx
            .send(template_message("late", 0, 301, &[(1, 0x0001_0002)]))
            .await
            .unwrap();
        let template_barrier = template_tx.reserve().await.unwrap();
        tokio::task::yield_now().await;
        assert!(stats_rx.try_recv().is_err());
        drop(template_barrier);

        drop(record_tx);
        drop(template_tx);
        assert!(task.await.unwrap().is_err());
    }
}
