use std::{
    error::Error,
    fmt::{Display, Formatter},
    sync::Arc,
    time::{Duration, SystemTime},
};

use ahash::{HashMap, HashMapExt, HashSet, HashSetExt};
use byteorder::{ByteOrder, NetworkEndian};
use log::{error, warn};
use tokio::{
    select,
    sync::mpsc::{Receiver, Sender},
    time::Instant,
};

use super::super::message::{
    buffer::SocketBufferMessage,
    ipfix::{
        IPFixTemplateOperation, IPFixTemplatesMessage, MAX_OBJECTS_PER_UPDATE,
        MAX_OBJECT_METADATA_BYTES, MAX_TEMPLATE_CONFIG_BYTES,
    },
    saistats::{decode_sai_ids, SAIStatMetadata, SAIStatsBatch, SAIStatsBatchMessage},
};
use crate::utilities::{record_comm_stats, ChannelLabel};
#[cfg(test)]
use crate::message::saistats::SAIStat;

const IPFIX_VERSION: u16 = 10;
const IPFIX_HEADER_LEN: usize = 16;
const SET_HEADER_LEN: usize = 4;
const TEMPLATE_SET_ID: u16 = 2;
const MIN_DATA_SET_ID: u16 = 256;
const OBSERVATION_TIME_SECONDS: u16 = 322;
const OBSERVATION_TIME_NANOSECONDS: u16 = 325;
const OBSERVATION_TIME_LEN: u16 = 8;
const SPLIT_OBSERVATION_TIME_LEN: u16 = 4;
const NANOS_PER_SECOND: u64 = 1_000_000_000;
const MIN_HFT_TEMPLATE_RECORD_LEN: usize = 12;
const DROP_WARNING_INTERVAL: Duration = Duration::from_secs(5);
// Accommodate one Set per queue at 2048 ports * 8 queues, with 2x headroom.
const MAX_DATA_SETS_PER_RECORD_INPUT: usize = 32 * 1024;
const MAX_RECORD_INPUTS_PER_BATCH: usize = 64;
const MAX_RECORD_INPUT_BYTES_PER_BATCH: usize = 4 * 1024 * 1024;
// A batching target, not a limit on a template or logical record.
const TARGET_COUNTERS_PER_BATCH: usize = 8192;
// Reject this namespace on input, including deletes: logical owners cannot be
// impersonated by an ordinary STATE_DB key.
const MIXED_OWNER_PREFIX: &str = "\0hft-mixed|";

fn hft_group(key: &str) -> Option<(&str, u32)> {
    let (profile, group) = key.split_once('|')?;
    if profile.is_empty() {
        return None;
    }
    let type_id = match group {
        "PORT" => 1,
        "QUEUE" => 21,
        "INGRESS_PRIORITY_GROUP" => 26,
        "BUFFER_POOL" => 24,
        _ => return None,
    };
    Some((profile, type_id))
}

#[derive(Debug, Clone, Default)]
struct ProfileContributions {
    // At most four latest rows, each subject to the ordinary update limits.
    groups: HashMap<u32, IPFixTemplatesMessage>,
    mixed: bool,
}

struct TemplateCompilation {
    // Partial descriptors are validation scratch only, never installed.
    generation: TemplateGeneration,
    labels: HashSet<(u16, u32)>,
    missing_labels: bool,
}

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
enum ObservationTime {
    RawNanoseconds {
        offset: usize,
    },
    Split {
        seconds_offset: usize,
        nanos_offset: usize,
    },
    Missing,
}

impl ObservationTime {
    fn extract(&self, record: &[u8]) -> Option<u64> {
        match *self {
            Self::RawNanoseconds { offset } => Some(NetworkEndian::read_u64(
                &record[offset..offset + usize::from(OBSERVATION_TIME_LEN)],
            )),
            Self::Split {
                seconds_offset,
                nanos_offset,
            } => {
                let seconds = NetworkEndian::read_u32(
                    &record
                        [seconds_offset..seconds_offset + usize::from(SPLIT_OBSERVATION_TIME_LEN)],
                );
                let nanos = NetworkEndian::read_u32(
                    &record[nanos_offset..nanos_offset + usize::from(SPLIT_OBSERVATION_TIME_LEN)],
                );
                Some(u64::from(seconds) * NANOS_PER_SECOND + u64::from(nanos))
            }
            Self::Missing => None,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct CompiledTemplate {
    key: TemplateKey,
    owner: Arc<str>,
    observation_time: ObservationTime,
    counters: Arc<[CompiledCounter]>,
    metadata: Arc<[SAIStatMetadata]>,
    record_len: usize,
}

impl CompiledTemplate {
    // Ownership is checked separately. During gathering only missing names
    // are uncertain; every byte offset and all other metadata must match.
    fn matches_schema(&self, candidate: &Self, allow_missing_names: bool) -> bool {
        self.observation_time == candidate.observation_time
            && self.record_len == candidate.record_len
            && self.counters.len() == candidate.counters.len()
            && self.counters.iter().zip(candidate.counters.iter()).all(|(old, new)| {
                old.offset == new.offset && old.len == new.len
                    && old.type_id == new.type_id && old.stat_id == new.stat_id
                    && (old.object_name == new.object_name
                        || (allow_missing_names && new.object_name.is_empty()))
            })
    }
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
    decoder: Option<(Arc<CompiledTemplate>, DataSetLayout)>,
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

#[derive(Debug, Clone, PartialEq, Eq)]
struct TemplateGeneration {
    templates: HashMap<TemplateKey, Arc<CompiledTemplate>>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct SessionTemplates {
    active: TemplateGeneration,
    // Latest complete snapshot, promoted by valid data on a new pending key.
    pending: Option<TemplateGeneration>,
}

#[derive(Debug)]
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
///
/// Template IDs are expected to increase, with reuse only after a long wrap.
/// Unknown Sets are dropped; transition delivery is best effort, without replay.
/// Active and pending snapshots coexist regardless of counter-list changes.
/// Valid nonempty data on a new pending key promotes the entire snapshot and
/// retires all old-only keys. Shared unchanged keys do not trigger promotion.
/// A newer snapshot supersedes pending state; resending active cancels it.
/// Missing times use the last explicit time in the same message, then the last
/// explicit time processed by this actor, then system UTC. History is shared
/// across owners/domains and survives template changes, without an expiry;
/// it is not a numeric maximum and system UTC never seeds it. A lost timestamp
/// message can therefore leave continuations using stale history until the next
/// explicit time arrives (or the actor is recreated).
pub struct IpfixActor {
    saistats_recipients: Vec<Sender<SAIStatsBatchMessage>>,
    template_recipient: Receiver<IPFixTemplatesMessage>,
    record_recipient: Receiver<SocketBufferMessage>,
    sessions: HashMap<Arc<str>, SessionTemplates>,
    installed: HashMap<TemplateKey, Arc<CompiledTemplate>>,
    contributions: HashMap<String, ProfileContributions>,
    last_observation_time: Option<u64>,
    dropped_sets: u64,
    next_drop_warning: Instant,
    next_invalid_warning: Instant,
    suppressed_invalid_warnings: u64,
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
            contributions: HashMap::new(),
            last_observation_time: None,
            dropped_sets: 0,
            next_drop_warning: Instant::now(),
            next_invalid_warning: Instant::now(),
            suppressed_invalid_warnings: 0,
        }
    }

    pub fn add_recipient(&mut self, recipient: Sender<SAIStatsBatchMessage>) {
        self.saistats_recipients.push(recipient);
    }

    #[cfg(test)]
    fn compile_generation(
        templates: &IPFixTemplatesMessage,
    ) -> Result<TemplateGeneration, IpfixError> {
        let compilation = Self::compile_candidate(templates)?;
        if compilation.missing_labels {
            return Err("template references unmapped object IDs".into());
        }
        Ok(compilation.generation)
    }

    fn compile_candidate(
        templates: &IPFixTemplatesMessage,
    ) -> Result<TemplateCompilation, IpfixError> {
        validate_template_update_limits(templates)?;
        let bytes = templates
            .templates
            .as_deref()
            .ok_or("template update has no template data")?;
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
        // Label zero cannot name a counter. Share one scratch sentinel across
        // all unmapped counters, rather than allocating an empty Arc per field.
        object_names.insert(0, Arc::from(""));

        let owner = Arc::<str>::from(templates.key.as_str());
        let mut compilation = TemplateCompilation {
            generation: TemplateGeneration {
                templates: HashMap::new(),
            },
            labels: HashSet::new(),
            missing_labels: false,
        };
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
                compile_template_set(set, domain, &owner, &object_names, &mut compilation)?;
            }
        }
        if compilation.generation.templates.is_empty() {
            return Err("template update contains no HFT templates".into());
        }
        Ok(compilation)
    }

    fn remove_session(&mut self, owner: &str) {
        self.sessions.remove(owner);
        self.installed
            .retain(|_, template| template.owner.as_ref() != owner);
    }

    fn handle_template(&mut self, update: IPFixTemplatesMessage) -> Result<(), IpfixError> {
        if update.key.starts_with(MIXED_OWNER_PREFIX) {
            return Err("reserved logical IPFIX owner namespace".into());
        }
        if matches!(
            update.operation,
            IPFixTemplateOperation::Delete | IPFixTemplateOperation::Deactivate
        ) {
            self.remove_source(&update.key);
            return Ok(());
        }

        let compilation = match Self::compile_candidate(&update) {
            Ok(compilation) => compilation,
            Err(err) => {
                // Malformed configuration locally deactivates its owner.
                self.remove_source(&update.key);
                return Err(err);
            }
        };
        let Some((profile, group)) = hft_group(&update.key) else {
            if compilation.missing_labels {
                self.remove_source(&update.key);
                return Err("template references unmapped object IDs".into());
            }
            return self.install_generation(compilation.generation, &[]);
        };
        let profile = profile.to_string();
        let mut candidate = self
            .contributions
            .get(&profile)
            .cloned()
            .unwrap_or_default();
        let types: HashSet<_> = compilation.labels.iter().map(|(_, t)| *t).collect();
        // A suffix alone is not evidence of MIXED. A one-type initial MIXED
        // snapshot is indistinguishable from SINGLE and uses the strict path.
        candidate.mixed |= types.len() > 1 && types.iter().all(|t| matches!(t, 1 | 21 | 24 | 26));
        if !candidate.mixed {
            if compilation.missing_labels {
                self.remove_source(&update.key);
                return Err("template references unmapped object IDs".into());
            }
            self.install_generation(compilation.generation, &[])?;
            // Only a genuine group-shaped SINGLE row is eligible for later
            // retirement/aggregation; suffix-shaped opaque owners stay strict.
            if types.len() == 1 && types.contains(&group) {
                candidate.groups.insert(group, update);
            } else {
                candidate.groups.remove(&group);
            }
            if candidate.groups.is_empty() {
                self.contributions.remove(&profile);
            } else {
                self.contributions.insert(profile, candidate);
            }
            return Ok(());
        }

        if !types.iter().all(|t| matches!(t, 1 | 21 | 24 | 26)) {
            return Err("unsupported SAI counter type in MIXED snapshot".into());
        }
        let mut label_types = HashMap::new();
        for (id, type_id) in &compilation.labels {
            if label_types.insert(*id, *type_id).is_some_and(|previous| previous != *type_id) {
                return Err(format!("MIXED object ID {id} references multiple SAI types").into());
            }
        }
        if self.sessions.contains_key(update.key.as_str()) && !candidate.groups.contains_key(&group)
        {
            return Err(
                "MIXED source conflicts with an opaque owner, not a prior HFT group".into(),
            );
        }

        let owner = format!("{MIXED_OWNER_PREFIX}{profile}");
        let mut combined = update.clone();
        combined.key = owner.clone();
        combined.object_ids = Some(Vec::new());
        combined.object_names = Some(Vec::new());
        candidate.groups.insert(group, update.clone());
        let mut ids = HashMap::new();
        let mut present_types = HashSet::new();
        for (source_type, source) in &candidate.groups {
            // Compare the entire concatenated wire snapshot, including domains
            // and message headers. Never borrow labels from a stale sibling.
            if source.templates != update.templates {
                continue;
            }
            present_types.insert(*source_type);
            for (id, name) in source
                .object_ids
                .as_ref()
                .expect("validated IDs")
                .iter()
                .zip(source.object_names.as_ref().expect("validated names"))
            {
                if ids.insert(*id, *source_type).is_some() {
                    return Err(format!("duplicate MIXED object ID {id}").into());
                }
                combined.object_ids.as_mut().expect("IDs").push(*id);
                combined
                    .object_names
                    .as_mut()
                    .expect("names")
                    .push(name.clone());
            }
        }
        for (id, type_id) in &compilation.labels {
            if ids
                .get(id)
                .is_some_and(|source_type| source_type != type_id)
            {
                return Err(format!(
                    "MIXED object ID {id} does not belong to its source group type"
                )
                .into());
            }
            if !ids.contains_key(id) && present_types.contains(type_id) {
                return Err(format!("MIXED group type {type_id} omits referenced object ID {id}").into());
            }
        }
        validate_template_update_limits(&combined)?;
        // Mode is effectively fixed per hardware profile. Eligible prior SINGLE
        // owners migrate to the logical owner only after all collision checks.
        let retiring: Vec<_> = self
            .contributions
            .get(&profile)
            .into_iter()
            .flat_map(|previous| previous.groups.values())
            .map(|source| source.key.clone())
            .collect();
        // Reuse the parser, but never retain two full scratch generations at
        // once. Only complete aggregate metadata may reach installation.
        drop(compilation);
        let combined_candidate = Self::compile_candidate(&combined)?;
        for (key, template) in &combined_candidate.generation.templates {
            if let Some(installed) = self.installed.get(key) {
                if (installed.owner.as_ref() != owner
                    && !retiring.iter().any(|s| s == installed.owner.as_ref()))
                    || !installed.matches_schema(template, true)
                {
                    return Err(format!("template collision at {key:?}: incoming owner {owner:?}, existing owner {:?}", installed.owner).into());
                }
            }
        }
        if !combined_candidate.missing_labels {
            self.install_generation(combined_candidate.generation, &retiring)?;
        }
        // All validation and collision checks precede raw-state replacement.
        // Valid incomplete snapshots leave the last active/pending decoder live.
        self.contributions.insert(profile, candidate);
        Ok(())
    }

    fn remove_source(&mut self, source: &str) {
        self.remove_session(source);
        if let Some((profile, group)) = hft_group(source) {
            if let Some(contributions) = self.contributions.get_mut(profile) {
                if contributions.groups.remove(&group).is_none() {
                    return;
                }
                let empty = contributions.groups.is_empty();
                // The producer does not regenerate survivors on partial delete.
                // Pause the whole shared decoder: it embeds the removed labels.
                // Survivors retain only their latest raw row, never the removed
                // mapping; only fresh complete metadata can restore decoding.
                self.remove_session(&format!("{MIXED_OWNER_PREFIX}{profile}"));
                if empty {
                    self.contributions.remove(profile);
                }
            }
        }
    }

    fn install_generation(
        &mut self,
        mut generation: TemplateGeneration,
        retiring: &[String],
    ) -> Result<(), IpfixError> {
        let owner = Arc::clone(
            &generation
                .templates
                .values()
                .next()
                .expect("nonempty generation")
                .owner,
        );
        // Check the entire candidate before changing any installed/session state.
        // Conflicts, unlike compile errors, must preserve both owners' snapshots.
        for (key, template) in &mut generation.templates {
            if let Some(installed) = self.installed.get(key) {
                if (installed.owner != owner
                    && !retiring.iter().any(|source| source == installed.owner.as_ref()))
                    || !installed.matches_schema(template, false)
                {
                    return Err(format!(
                        "template collision at {key:?}: incoming owner {:?}, existing owner {:?}; different schema or owner",
                        owner, installed.owner
                    )
                    .into());
                }
                // Shared keys have one decoder/allocation across both generations.
                if installed.owner == owner {
                    *template = Arc::clone(installed);
                }
            }
        }
        let active = self.sessions.get(owner.as_ref()).map(|previous| previous.active.clone())
            .or_else(|| {
                let mut templates = HashMap::new();
                for previous in retiring.iter().filter_map(|source| self.sessions.get(source.as_str())) {
                    // Preserve old active keys until new-key data promotes MIXED.
                    // Prior pending snapshots are superseded by this complete
                    // update, exactly as in ordinary generation replacement.
                    for (key, old) in &previous.active.templates {
                        let template = generation.templates.get(key).cloned().unwrap_or_else(|| {
                            let mut template = old.as_ref().clone();
                            template.owner = Arc::clone(&owner);
                            Arc::new(template)
                        });
                        templates.insert(*key, template);
                    }
                }
                (!templates.is_empty()).then_some(TemplateGeneration { templates })
            });
        let session = match active {
            Some(active) => SessionTemplates {
                pending: (generation != active).then_some(generation),
                active,
            },
            None => SessionTemplates {
                active: generation,
                pending: None,
            },
        };

        // Keep active plus the latest pending snapshot, never historical pending keys.
        for source in retiring {
            self.remove_session(source);
        }
        self.installed.retain(|_, template| template.owner != owner);
        for generation in std::iter::once(&session.active).chain(session.pending.iter()) {
            self.installed.extend(
                generation
                    .templates
                    .iter()
                    .map(|(key, template)| (*key, Arc::clone(template))),
            );
        }
        self.sessions.insert(owner, session);
        Ok(())
    }

    fn promote_pending_for(&mut self, template: &CompiledTemplate) {
        let session = self
            .sessions
            .get_mut(template.owner.as_ref())
            .expect("installed owner");
        let Some(pending) = &session.pending else {
            return;
        };
        if !pending.templates.contains_key(&template.key)
            || session.active.templates.contains_key(&template.key)
        {
            return;
        }
        for key in session.active.templates.keys() {
            if !pending.templates.contains_key(key) {
                self.installed.remove(key);
            }
        }
        session.active = session.pending.take().expect("pending generation");
    }

    #[cfg(test)]
    fn handle_record(&mut self, records: &[u8]) -> Result<SAIStatsBatch, IpfixError> {
        let mut batch = SAIStatsBatch::default();
        let input = self.validate_record_input(records)?;
        for validated in input.messages {
            self.process_data_message(validated, &mut batch);
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
        // Validate the whole input before emitting data or changing generations.
        for message in IpfixMessages::new(records) {
            let validated = self.validate_data_message(message?)?;
            total_sets = total_sets
                .checked_add(validated.sets.len())
                .ok_or("data Set count overflow")?;
            if total_sets > MAX_DATA_SETS_PER_RECORD_INPUT {
                return Err(format!(
                    "record input exceeds {MAX_DATA_SETS_PER_RECORD_INPUT} data Sets"
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
            let decoder = self
                .installed
                .get(&key)
                .map(|template| {
                    validate_data_set(template, set).map(|layout| (Arc::clone(template), layout))
                })
                .transpose()?;
            if let Some((_, layout)) = &decoder {
                counter_count = counter_count
                    .checked_add(layout.counter_count)
                    .ok_or("decoded counter count overflow")?;
            }
            sets.push(ValidatedDataSet {
                key,
                bytes: set,
                decoder,
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
        mut message: ValidatedDataMessage<'_>,
        batch: &mut SAIStatsBatch,
    ) {
        let dropped_before = self.dropped_sets;
        let mut message_time = None;
        // Resolve cutovers in wire order before looking ahead for a fallback.
        // Keep earlier live descriptors even if a later Set retires them.
        for set in &mut message.sets {
            if let Some((template, layout)) = &set.decoder {
                // A preceding Set in this input may have retired this descriptor.
                if self
                    .installed
                    .get(&set.key)
                    .is_some_and(|installed| Arc::ptr_eq(installed, template))
                {
                    self.promote_pending_for(template);
                    let end = SET_HEADER_LEN + layout.record_bytes;
                    if let Some(time) = template
                        .observation_time
                        .extract(&set.bytes[end - template.record_len..end])
                    {
                        message_time = Some(time);
                    }
                    continue;
                }
            }
            set.decoder = None;
            self.dropped_sets = self.dropped_sets.saturating_add(1);
        }
        let fallback_time = message_time.or(self.last_observation_time);
        for set in message.sets {
            if let Some((template, layout)) = set.decoder {
                self.decode_set(&template, set.bytes, layout, fallback_time, batch);
            }
        }
        if message_time.is_some() {
            self.last_observation_time = message_time;
        }
        let now = Instant::now();
        if self.dropped_sets != dropped_before && now >= self.next_drop_warning {
            warn!(
                "Dropping HFT data Sets without a live template; total dropped Sets={}",
                self.dropped_sets
            );
            self.next_drop_warning = now + DROP_WARNING_INTERVAL;
        }
    }

    fn decode_set(
        &self,
        template: &CompiledTemplate,
        set: &[u8],
        layout: DataSetLayout,
        fallback_time: Option<u64>,
        batch: &mut SAIStatsBatch,
    ) {
        let payload = &set[SET_HEADER_LEN..];
        batch.reserve_shared(layout.record_count, layout.counter_count);
        for record in payload[..layout.record_bytes].chunks_exact(template.record_len) {
            let observation_time = template
                .observation_time
                .extract(record)
                .or(fallback_time)
                .unwrap_or_else(|| {
                    SystemTime::now()
                        .duration_since(SystemTime::UNIX_EPOCH)
                        .expect("System time should be after Unix epoch")
                        .as_nanos() as u64
                });
            batch.push_shared_record(
                observation_time,
                template.metadata.clone(),
                template.counters.iter().map(|counter| read_be_u64(
                        &record[counter.offset..counter.offset + counter.len as usize],
                    )),
            );
        }
    }

    fn record_invalid_input_warning(&mut self) -> Option<u64> {
        let now = Instant::now();
        if now < self.next_invalid_warning {
            self.suppressed_invalid_warnings = self.suppressed_invalid_warnings.saturating_add(1);
            return None;
        }
        self.next_invalid_warning = now + DROP_WARNING_INTERVAL;
        Some(std::mem::take(&mut self.suppressed_invalid_warnings))
    }

    async fn process_record_input(&mut self, records: &[u8], batch: &mut SAIStatsBatch) {
        let input = match self.validate_record_input(records) {
            Ok(input) => input,
            Err(err) => {
                if let Some(suppressed) = self.record_invalid_input_warning() {
                    warn!(
                        "Dropping invalid HFT IPFIX message: {err}; {suppressed} prior invalid-input warning(s) suppressed"
                    );
                }
                return;
            }
        };
        for validated in input.messages {
            if !batch.is_empty()
                && batch
                    .counter_count()
                    .saturating_add(validated.counter_count)
                    > TARGET_COUNTERS_PER_BATCH
            {
                self.send_batch(std::mem::take(batch)).await;
            }
            self.process_data_message(validated, batch);
            if batch.counter_count() >= TARGET_COUNTERS_PER_BATCH {
                self.send_batch(std::mem::take(batch)).await;
            }
        }
    }

    async fn send_batch(&self, batch: SAIStatsBatch) {
        if batch.is_empty() || self.saistats_recipients.is_empty() {
            return;
        }
        if batch.counter_count() <= TARGET_COUNTERS_PER_BATCH || batch.record_count() == 1 {
            self.send_chunk(batch).await;
            return;
        }
        for batch in batch.into_record_batches(TARGET_COUNTERS_PER_BATCH) {
            self.send_chunk(batch).await;
        }
    }

    async fn send_chunk(&self, batch: SAIStatsBatch) {
        let batch = Arc::new(batch);
        let mut blocked = Vec::new();
        for recipient in &self.saistats_recipients {
            match recipient.try_reserve() {
                Ok(permit) => permit.send(Arc::clone(&batch)),
                Err(tokio::sync::mpsc::error::TrySendError::Full(_)) => blocked.push(recipient),
                Err(tokio::sync::mpsc::error::TrySendError::Closed(_)) => {}
            }
        }
        for recipient in blocked {
            let _ = recipient.send(Arc::clone(&batch)).await;
        }
    }

    pub async fn run(mut actor: IpfixActor) {
        loop {
            select! {
                template = actor.template_recipient.recv() => match template {
                    Some(template) => {
                        record_comm_stats(ChannelLabel::SwssToIpfixTemplates, actor.template_recipient.len());
                        if let Err(err) = actor.handle_template(template) {
                            error!("HFT template update rejected: {err}");
                        }
                    }
                    None => break,
                },
                record = actor.record_recipient.recv() => match record {
                    Some(record) => {
                        record_comm_stats(ChannelLabel::DataNetlinkToIpfixRecords, actor.record_recipient.len());
                        let mut batch = SAIStatsBatch::default();
                        let mut input_count = 1usize;
                        let mut input_bytes = record.len();
                        actor.process_record_input(&record, &mut batch).await;
                        while input_count < MAX_RECORD_INPUTS_PER_BATCH
                            && input_bytes < MAX_RECORD_INPUT_BYTES_PER_BATCH
                            && actor.template_recipient.is_empty()
                        {
                            let Ok(next) = actor.record_recipient.try_recv() else { break; };
                            record_comm_stats(ChannelLabel::DataNetlinkToIpfixRecords, actor.record_recipient.len());
                            input_count += 1;
                            input_bytes = input_bytes.saturating_add(next.len());
                            actor.process_record_input(&next, &mut batch).await;
                        }
                        actor.send_batch(batch).await;
                    }
                    None => break,
                }
            }
        }
    }
}

fn validate_template_update_limits(templates: &IPFixTemplatesMessage) -> Result<(), IpfixError> {
    let bytes = templates
        .templates
        .as_ref()
        .ok_or("template update has no template data")?;
    if bytes.len() > MAX_TEMPLATE_CONFIG_BYTES {
        return Err(format!("template update exceeds {MAX_TEMPLATE_CONFIG_BYTES} bytes").into());
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
        .try_fold(templates.key.len(), |total, name| {
            total.checked_add(name.len())
        })
        .ok_or("object metadata size overflow")?
        .checked_add(
            ids.len()
                .checked_mul(std::mem::size_of::<u16>())
                .ok_or("object metadata size overflow")?,
        )
        .ok_or("object metadata size overflow")?;
    if metadata_bytes > MAX_OBJECT_METADATA_BYTES {
        return Err(format!("object metadata exceeds {MAX_OBJECT_METADATA_BYTES} bytes").into());
    }
    Ok(())
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
    if padding.iter().any(|byte| *byte != 0) {
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
        .ok_or("decoded counter count overflow")?;
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
    output: &mut TemplateCompilation,
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
        // Validate field storage before reserving from an untrusted count.
        if field_count > (set.len() - offset) / 4 {
            return Err(format!("template {template_id} has a truncated field list").into());
        }
        let mut counters = Vec::with_capacity(field_count);
        let mut raw_nanos_offset = None;
        let mut seconds_offset = None;
        let mut nanos_offset = None;
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
            let field_offset = record_len;
            record_len = record_len
                .checked_add(usize::from(field_len))
                .ok_or("template record length overflow")?;
            if enterprise {
                if !(1..=8).contains(&field_len) {
                    return Err(format!("template {template_id} counter field {field_id} has unsupported length {field_len}; expected 1..=8 bytes").into());
                }
                if set.len() - offset < 4 {
                    return Err(format!(
                        "template {template_id} has a truncated enterprise number"
                    )
                    .into());
                }
                let enterprise_number = NetworkEndian::read_u32(&set[offset..offset + 4]);
                offset += 4;
                // Hardware placeholders occupy record bytes but are not counters.
                if field_id == 0 && enterprise_number == 0 {
                    continue;
                }
                if enterprise_number == 0 {
                    return Err(format!(
                        "template {template_id} uses reserved enterprise number zero"
                    )
                    .into());
                }
                if field_id == 0 {
                    return Err("counter object ID zero is reserved for placeholders".into());
                }
                let (type_id, stat_id) = decode_sai_ids(enterprise_number);
                if !field_keys.insert((field_id, enterprise_number)) {
                    return Err(format!(
                        "template {template_id} contains a duplicate counter field"
                    )
                    .into());
                }
                output.labels.insert((field_id, type_id));
                let object_name = object_names.get(&field_id).cloned().unwrap_or_else(|| {
                    output.missing_labels = true;
                    Arc::clone(&object_names[&0])
                });
                counters.push(CompiledCounter {
                    offset: field_offset,
                    len: u8::try_from(field_len).expect("counter length is at most 8"),
                    object_name,
                    type_id,
                    stat_id,
                });
            } else if matches!(
                field_id,
                OBSERVATION_TIME_SECONDS | OBSERVATION_TIME_NANOSECONDS
            ) {
                let time_offset = match (field_id, field_len) {
                    (OBSERVATION_TIME_NANOSECONDS, OBSERVATION_TIME_LEN) => &mut raw_nanos_offset,
                    (OBSERVATION_TIME_SECONDS, SPLIT_OBSERVATION_TIME_LEN) => &mut seconds_offset,
                    (OBSERVATION_TIME_NANOSECONDS, SPLIT_OBSERVATION_TIME_LEN) => &mut nanos_offset,
                    _ => return Err(format!("template {template_id} observation time IE {field_id} has unsupported length {field_len}").into()),
                };
                if time_offset.replace(field_offset).is_some() {
                    return Err(format!(
                        "template {template_id} contains duplicate observation time fields"
                    )
                    .into());
                }
            } else {
                return Err(format!(
                    "template {template_id} contains unsupported standard IE {field_id}"
                )
                .into());
            }
        }
        if field_keys.is_empty() {
            return Err(format!("template {template_id} requires at least one counter").into());
        }
        let observation_time = if let Some(offset) = raw_nanos_offset {
            ObservationTime::RawNanoseconds { offset }
        } else if let (Some(seconds_offset), Some(nanos_offset)) = (seconds_offset, nanos_offset) {
            ObservationTime::Split {
                seconds_offset,
                nanos_offset,
            }
        } else {
            ObservationTime::Missing
        };
        let key = TemplateKey {
            observation_domain_id: domain,
            template_id,
        };
        let template = Arc::new(CompiledTemplate {
            key,
            owner: Arc::clone(owner),
            observation_time,
            metadata: counters.iter().map(|counter|SAIStatMetadata::new(counter.object_name.clone(),counter.type_id,counter.stat_id)).collect::<Vec<_>>().into(),
            counters: counters.into(),
            record_len,
        });
        if output.generation.templates.insert(key, template).is_some() {
            return Err(
                format!("duplicate template ({domain}, {template_id}) in one update").into(),
            );
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::sync::mpsc::channel;

    const MIXED_GROUPS: [(&str, u16, u32, &str); 4] = [
        ("PORT", 1, 1, "Ethernet0"),
        ("QUEUE", 2, 21, "Ethernet0:0"),
        ("INGRESS_PRIORITY_GROUP", 3, 26, "Ethernet0:0"),
        ("BUFFER_POOL", 4, 24, "ingress_lossless_pool"),
    ];

    fn mixed_rows(profile: &str, domain: u32, id: u16) -> Vec<IPFixTemplatesMessage> {
        let fields: Vec<_> = MIXED_GROUPS
            .iter()
            .map(|(_, label, t, _)| (*label, (t << 16) | 7))
            .collect();
        MIXED_GROUPS
            .iter()
            .map(|(group, label, _, name)| {
                let mut row = template_message(&format!("{profile}|{group}"), domain, id, &fields);
                row.object_ids = Some(vec![*label]);
                row.object_names = Some(vec![name.to_string()]);
                row
            })
            .collect()
    }

    fn install_mixed(actor: &mut IpfixActor, profile: &str, domain: u32, id: u16) {
        for row in mixed_rows(profile, domain, id) {
            actor.handle_template(row).unwrap();
        }
    }

    #[test]
    fn mixed_startup_all_group_orders_decode_real_typed_names() {
        // All 24 permutations, including QUEUE/IPG sharing the same name.
        for a in 0..4 {
            for b in 0..4 {
                for c in 0..4 {
                    for d in 0..4 {
                        let order = [a, b, c, d];
                        if order.iter().copied().collect::<HashSet<_>>().len() != 4 {
                            continue;
                        }
                        let mut actor = actor();
                        let rows = mixed_rows("p", 9, 300);
                        for (step, index) in order.into_iter().enumerate() {
                            actor.handle_template(rows[index].clone()).unwrap();
                            assert_eq!(actor.installed.len(), usize::from(step == 3));
                        }
                        let batch = actor
                            .handle_record(&data_message(
                                9,
                                &[(300, vec![(42, vec![11, 22, 33, 44])])],
                            ))
                            .unwrap();
                        let record = batch.iter().next().unwrap();
                        for (index, (_, _, t, name)) in MIXED_GROUPS.iter().enumerate() {
                            let stat = record.stats.get(index).unwrap();
                            assert_eq!(stat.object_name.as_ref(), *name);
                            assert_eq!(stat.type_id, *t);
                            assert_eq!(stat.stat_id, 7);
                            assert_eq!(stat.counter, (index as u64 + 1) * 11);
                        }
                        assert_eq!(actor.sessions.len(), 1);
                        assert_eq!(actor.contributions["p"].groups.len(), 4);
                    }
                }
            }
        }
    }

    #[test]
    fn mixed_staggered_metadata_keeps_active_and_promotes_pending_together() {
        let mut actor = actor();
        install_mixed(&mut actor, "p", 0, 300);
        let old = actor.installed.clone();
        let mut next = mixed_rows("p", 0, 400);
        for row in &mut next {
            row.object_names.as_mut().unwrap()[0].push_str("-new");
        }
        for row in &next[..3] {
            actor.handle_template(row.clone()).unwrap();
            assert_eq!(actor.installed, old);
            assert_eq!(
                actor
                    .handle_record(&data_message(0, &[(300, vec![(1, vec![1; 4])])]))
                    .unwrap()
                    .counter_count(),
                4
            );
            assert!(actor
                .handle_record(&data_message(0, &[(400, vec![(1, vec![1; 4])])]))
                .unwrap()
                .is_empty());
        }
        actor.handle_template(next[3].clone()).unwrap();
        assert_eq!(keys(&actor), vec![(0, 300), (0, 400)]);
        let owner = format!("{MIXED_OWNER_PREFIX}p");
        assert!(actor.sessions[owner.as_str()].pending.is_some());
        let batch = actor
            .handle_record(&data_message(0, &[(400, vec![(2, vec![2; 4])])]))
            .unwrap();
        assert!(batch
            .iter()
            .next()
            .unwrap()
            .stats
            .get(0)
            .unwrap()
            .object_name
            .ends_with("-new"));
        assert_eq!(keys(&actor), vec![(0, 400)]);
        assert!(actor.sessions[owner.as_str()].pending.is_none());
    }

    #[test]
    fn mixed_latest_snapshot_only_no_stale_sibling_or_generation_growth() {
        let mut actor = actor();
        install_mixed(&mut actor, "p", 0, 300);
        install_mixed(&mut actor, "p", 0, 400);
        for id in 401..450 {
            let rows = mixed_rows("p", 0, id);
            for row in &rows[..3] {
                actor.handle_template(row.clone()).unwrap();
            }
            let stale = mixed_rows("p", 0, id - 1);
            actor.handle_template(stale[3].clone()).unwrap();
            assert_eq!(keys(&actor), vec![(0, 300), (0, 400)]);
            assert_eq!(actor.contributions["p"].groups.len(), 4);
        }
        actor
            .handle_template(mixed_rows("p", 0, 449)[3].clone())
            .unwrap();
        assert_eq!(keys(&actor), vec![(0, 300), (0, 449)]);
        install_mixed(&mut actor, "p", 0, 450);
        assert_eq!(keys(&actor), vec![(0, 300), (0, 450)]);
        install_mixed(&mut actor, "p", 0, 300);
        assert_eq!(keys(&actor), vec![(0, 300)]);
        assert_eq!(actor.sessions.len(), 1);
    }

    #[test]
    fn mixed_matching_requires_full_bytes_domain_and_profile() {
        let mut actor = actor();
        let rows = mixed_rows("p", 0, 300);
        actor.handle_template(rows[0].clone()).unwrap();
        for row in &mixed_rows("other", 0, 300)[1..] {
            actor.handle_template(row.clone()).unwrap();
        }
        for row in &mixed_rows("p", 1, 300)[1..] {
            actor.handle_template(row.clone()).unwrap();
        }
        assert!(actor.installed.is_empty());
        for row in &rows[1..] {
            let mut different_header = row.clone();
            Arc::make_mut(different_header.templates.as_mut().unwrap())[8] = 1;
            actor.handle_template(different_header).unwrap();
        }
        assert!(actor.installed.is_empty());
        for row in &rows[1..] {
            actor.handle_template(row.clone()).unwrap();
        }
        assert_eq!(keys(&actor), vec![(0, 300)]);
        let sessions = actor.sessions.clone();
        assert!(actor
            .handle_template(mixed_rows("other", 0, 300)[0].clone())
            .is_err());
        assert!(!actor.contributions["other"].groups.contains_key(&1));
        assert_eq!(actor.sessions, sessions);
        install_mixed(&mut actor, "other", 1, 300);
        assert_eq!(keys(&actor), vec![(0, 300), (1, 300)]);
    }

    #[test]
    fn mixed_invalid_framing_and_fields_are_not_incomplete_metadata() {
        let base = mixed_rows("p", 0, 300)[0].clone();
        let mut malformed = Vec::new();
        let mut row = base.clone();
        Arc::make_mut(row.templates.as_mut().unwrap()).extend_from_slice(&[10, 0, 1]);
        malformed.push(row);
        // Errors after an unmapped field must still be found by the one parser.
        for fields in [
            vec![(1, 0x0001_0007), (2, 0x0015_0007), (2, 0x0015_0007)],
            vec![(1, 0x0001_0007), (2, 0x0015_0007), (3, 0)],
        ] {
            let mut row = template_message("p|PORT", 0, 300, &fields);
            row.object_ids = base.object_ids.clone();
            row.object_names = base.object_names.clone();
            malformed.push(row);
        }
        let mut row = base.clone();
        // Last enterprise counter has an invalid width.
        let bytes = Arc::make_mut(row.templates.as_mut().unwrap());
        let offset = bytes.len() - 6;
        bytes[offset..offset + 2].copy_from_slice(&9u16.to_be_bytes());
        malformed.push(row);
        let mut row = base.clone();
        let duplicate = row.templates.clone().unwrap();
        Arc::make_mut(row.templates.as_mut().unwrap()).extend_from_slice(&duplicate);
        malformed.push(row);
        for row in malformed {
            let mut actor = actor();
            assert!(actor.handle_template(row).is_err());
            assert!(actor.contributions.is_empty());
            assert!(actor.installed.is_empty());
        }
    }

    #[test]
    fn mixed_duplicates_associations_limits_and_collisions_are_transactional() {
        let mut actor = actor();
        let rows = mixed_rows("p", 0, 300);
        actor.handle_template(rows[0].clone()).unwrap();
        let raw = format!("{:?}", actor.contributions);
        let mut bad = rows[1].clone();
        bad.object_ids = rows[0].object_ids.clone();
        bad.object_names = rows[0].object_names.clone();
        assert!(actor
            .handle_template(bad)
            .unwrap_err()
            .to_string()
            .contains("duplicate"));
        assert_eq!(format!("{:?}", actor.contributions), raw);
        let mut bad = rows[1].clone();
        bad.object_ids = Some(vec![3]);
        assert!(actor.handle_template(bad).is_err());
        assert_eq!(format!("{:?}", actor.contributions), raw);

        let mut large = rows[0].clone();
        large.object_names = Some(vec!["x".repeat(MAX_OBJECT_METADATA_BYTES / 2)]);
        actor.handle_template(large).unwrap();
        let raw = format!("{:?}", actor.contributions);
        let mut large = rows[1].clone();
        large.object_names = Some(vec!["y".repeat(MAX_OBJECT_METADATA_BYTES / 2)]);
        assert!(actor.handle_template(large).is_err());
        assert_eq!(format!("{:?}", actor.contributions), raw);

        let mut actor = IpfixActor::new(channel(4).1, channel(4).1);
        install_mixed(&mut actor, "p", 0, 300);
        actor
            .handle_template(snapshot("opaque", &[(0, 500, 1)]))
            .unwrap();
        let sessions = actor.sessions.clone();
        let raw = format!("{:?}", actor.contributions);
        assert!(actor
            .handle_template(mixed_rows("p", 0, 500)[0].clone())
            .is_err());
        let mut bad = rows[0].clone();
        bad.object_names = Some(vec!["changed".into()]);
        assert!(actor.handle_template(bad).is_err());
        assert_eq!(actor.sessions, sessions);
        assert_eq!(format!("{:?}", actor.contributions), raw);
        let logical = format!("{MIXED_OWNER_PREFIX}p");
        for bad in [
            snapshot(&logical, &[(0, 600, 1)]),
            IPFixTemplatesMessage::delete(logical.clone()),
            IPFixTemplatesMessage::deactivate(logical),
        ] {
            assert!(actor.handle_template(bad).is_err());
            assert_eq!(actor.sessions, sessions);
        }
    }

    #[test]
    fn mixed_delete_disable_and_malformed_source_pause_without_label_resurrection() {
        for operation in 0..3 {
            let mut actor = actor();
            install_mixed(&mut actor, "p", 0, 300);
            install_mixed(&mut actor, "p", 0, 400);
            install_mixed(&mut actor, "other", 1, 300);
            actor
                .handle_template(snapshot("single|PORT", &[(2, 300, 0x0001_0001)]))
                .unwrap();
            let remove = match operation {
                0 => IPFixTemplatesMessage::delete("p|QUEUE".into()),
                1 => IPFixTemplatesMessage::deactivate("p|QUEUE".into()),
                _ => {
                    let mut bad = mixed_rows("p", 0, 400)[1].clone();
                    bad.templates = Some(Arc::new(vec![0]));
                    bad
                }
            };
            assert_eq!(actor.handle_template(remove).is_err(), operation == 2);
            assert_eq!(keys(&actor), vec![(1, 300), (2, 300)]);
            assert!(!actor.contributions["p"].groups.contains_key(&21));
            for id in [300, 400] {
                for (index, row) in mixed_rows("p", 0, id).into_iter().enumerate() {
                    if index != 1 {
                        actor.handle_template(row).unwrap();
                    }
                }
                assert_eq!(keys(&actor), vec![(1, 300), (2, 300)]);
                assert!(actor
                    .handle_record(&data_message(0, &[(id, vec![(1, vec![1; 4])])]))
                    .unwrap()
                    .is_empty());
            }
            install_mixed(&mut actor, "p", 0, 500);
            assert_eq!(keys(&actor), vec![(0, 500), (1, 300), (2, 300)]);
            for (group, _, _, _) in MIXED_GROUPS {
                actor
                    .handle_template(IPFixTemplatesMessage::delete(format!("p|{group}")))
                    .unwrap();
            }
            assert!(!actor.contributions.contains_key("p"));
            assert_eq!(keys(&actor), vec![(1, 300), (2, 300)]);
        }
    }

    #[test]
    fn single_group_start_and_nonoverlapping_mixed_transition_preserve_old_active() {
        let mut actor = actor();
        for (group, _, t, _) in MIXED_GROUPS {
            actor
                .handle_template(snapshot(
                    &format!("p|{group}"),
                    &[(0, 600 + t as u16, (t << 16) | 1)],
                ))
                .unwrap();
        }
        assert_eq!(actor.sessions.len(), 4);
        let rows = mixed_rows("p", 0, 300);
        for row in &rows[..3] {
            actor.handle_template(row.clone()).unwrap();
        }
        assert_eq!(actor.sessions.len(), 4);
        actor.handle_template(rows[3].clone()).unwrap();
        assert_eq!(keys(&actor), vec![(0, 300), (0, 601), (0, 621), (0, 624), (0, 626)]);
        for (_, _, t, _) in MIXED_GROUPS {
            assert_eq!(actor.handle_record(&data_message(0, &[(600 + t as u16, vec![(1, vec![9])])]))
                .unwrap().counter_count(), 1);
        }
        assert_eq!(actor.sessions.len(), 1);
        actor.handle_record(&data_message(0, &[(300, vec![(2, vec![2; 4])])])).unwrap();
        assert_eq!(keys(&actor), vec![(0, 300)]);
        // A later one-type snapshot in known MIXED mode stays under the one
        // logical owner, without accumulating new per-group sessions.
        actor
            .handle_template(snapshot("p|PORT", &[(0, 700, 0x0001_0001)]))
            .unwrap();
        assert_eq!(keys(&actor), vec![(0, 300), (0, 700)]);
        actor
            .handle_record(&data_message(0, &[(700, vec![(1, vec![1])])]))
            .unwrap();
        assert_eq!(keys(&actor), vec![(0, 700)]);
        assert_eq!(actor.sessions.len(), 1);
    }

    #[test]
    fn mixed_migration_never_exempts_schema_checks_for_old_owners() {
        let mut actor = actor();
        actor.handle_template(snapshot("p|PORT", &[(0, 300, 0x0001_0001)])).unwrap();
        actor.handle_template(snapshot("p|PORT", &[(0, 350, 0x0001_0001)])).unwrap();
        let sessions = actor.sessions.clone();
        let installed = actor.installed.clone();
        let raw = format!("{:?}", actor.contributions);
        // Even an incomplete first row must not reinterpret an active or
        // pending SINGLE template ID as a different MIXED record layout.
        for id in [300, 350] {
            assert!(actor.handle_template(mixed_rows("p", 0, id)[0].clone()).is_err());
            assert_eq!(actor.sessions, sessions);
            assert_eq!(actor.installed, installed);
            assert_eq!(format!("{:?}", actor.contributions), raw);
        }
        // Exercise the final installation boundary independently: exemption
        // from owner equality must never exempt even one schema component.
        let owner = format!("{MIXED_OWNER_PREFIX}p");
        for change in 0..7 {
            let mut generation = IpfixActor::compile_generation(&snapshot(&owner, &[(0, 300, 0x0001_0001)])).unwrap();
            let template = Arc::make_mut(generation.templates.values_mut().next().unwrap());
            match change {
                0 => template.observation_time = ObservationTime::Missing,
                1 => template.record_len += 1,
                change => {
                    let counter = &mut Arc::make_mut(&mut template.counters)[0];
                    match change {
                        2 => counter.offset += 1,
                        3 => counter.len = 4,
                        4 => counter.type_id = 21,
                        5 => counter.stat_id += 1,
                        _ => counter.object_name = Arc::from("changed"),
                    }
                }
            }
            assert!(actor.install_generation(generation, &["p|PORT".into()]).is_err());
            assert_eq!(actor.sessions, sessions);
            assert_eq!(actor.installed, installed);
        }
    }

    #[test]
    fn mixed_migration_reowns_active_supersedes_pending_and_waits_for_new_data() {
        for (shared_key, queue_first) in [(false, false), (true, false), (false, true), (true, true)] {
            let mut actor = actor();
            let old = snapshot("p|PORT", &[(0, 300, 0x0001_0001)]);
            actor.handle_template(old.clone()).unwrap();
            actor.handle_template(snapshot("p|PORT", &[(0, 350, 0x0001_0001)])).unwrap();
            let mut port = template_message("p|PORT", 0, 400, &[(1, 0x0001_0001), (2, 0x0015_0001)]);
            if shared_key {
                Arc::make_mut(port.templates.as_mut().unwrap()).extend_from_slice(old.templates.as_ref().unwrap());
            }
            port.object_ids = Some(vec![1]);
            port.object_names = Some(vec!["Ethernet1".into()]);
            let mut queue = port.clone();
            queue.key = "p|QUEUE".into();
            queue.object_ids = Some(vec![2]);
            queue.object_names = Some(vec!["Ethernet0:0".into()]);
            if shared_key {
                let mut bad = port.clone();
                bad.object_names = Some(vec!["renamed".into()]);
                let raw = format!("{:?}", actor.contributions);
                let sessions = actor.sessions.clone();
                assert!(actor.handle_template(bad).is_err());
                assert_eq!(actor.sessions, sessions);
                assert_eq!(format!("{:?}", actor.contributions), raw);
            }
            let (first, last) = if queue_first { (queue, port) } else { (port, queue) };
            actor.handle_template(first).unwrap();
            assert_eq!(keys(&actor), vec![(0, 300), (0, 350)]);
            actor.handle_template(last).unwrap();
            assert_eq!(keys(&actor), vec![(0, 300), (0, 400)]);
            let owner = format!("{MIXED_OWNER_PREFIX}p");
            assert_eq!(actor.sessions.len(), 1);
            assert!(actor.installed.values().all(|t| t.owner.as_ref() == owner));
            assert_eq!(actor.handle_record(&data_message(0, &[(300, vec![(42, vec![9])])])).unwrap().counter_count(), 1);
            assert!(actor.sessions[owner.as_str()].pending.is_some());
            // Invalid new-key input cannot cut over, nor can superseded pending.
            assert!(actor.handle_record(&hardware_data(400, &[&[0; 8]])).is_err());
            assert!(actor.handle_record(&data_message(0, &[(350, vec![(42, vec![9])])])).unwrap().is_empty());
            assert!(actor.sessions[owner.as_str()].pending.is_some());
            actor.handle_record(&data_message(0, &[(400, vec![(43, vec![10, 11])])])).unwrap();
            assert!(actor.sessions[owner.as_str()].pending.is_none());
            assert_eq!(keys(&actor), if shared_key { vec![(0, 300), (0, 400)] } else { vec![(0, 400)] });
        }
    }

    #[test]
    fn mixed_missing_labels_require_absent_type_not_incomplete_present_group() {
        let mut actor = actor();
        let fields = [(1, 0x0001_0001), (5, 0x0001_0002), (2, 0x0015_0001)];
        let mut port = template_message("p|PORT", 0, 300, &fields);
        port.object_ids = Some(vec![1]);
        port.object_names = Some(vec!["Ethernet1".into()]);
        assert!(actor.handle_template(port.clone()).unwrap_err().to_string().contains("omits referenced"));
        assert!(actor.contributions.is_empty());
        let mut queue = port.clone();
        queue.key = "p|QUEUE".into();
        queue.object_ids = Some(vec![2]);
        queue.object_names = Some(vec!["Ethernet0:0".into()]);
        // PORT metadata is genuinely absent, so QUEUE may wait for it.
        actor.handle_template(queue).unwrap();
        let raw = format!("{:?}", actor.contributions);
        assert!(actor.handle_template(port.clone()).is_err());
        assert_eq!(format!("{:?}", actor.contributions), raw);
        port.object_ids.as_mut().unwrap().push(5);
        port.object_names.as_mut().unwrap().push("Ethernet5".into());
        actor.handle_template(port).unwrap();
        assert_eq!(actor.installed.len(), 1);

        // Label 5 is ambiguous even though neither owning group has arrived.
        let mut ambiguous = template_message("q|PORT", 0, 400,
            &[(1, 0x0001_0001), (5, 0x0015_0001), (5, 0x001a_0001)]);
        ambiguous.object_ids = Some(vec![1]);
        ambiguous.object_names = Some(vec!["Ethernet1".into()]);
        assert!(actor.handle_template(ambiguous).unwrap_err().to_string().contains("multiple SAI types"));
        assert!(!actor.contributions.contains_key("q"));
    }

    #[test]
    fn incomplete_compilation_shares_one_empty_name_allocation() {
        let row = mixed_rows("p", 0, 300)[0].clone();
        let compilation = IpfixActor::compile_candidate(&row).unwrap();
        assert!(compilation.missing_labels);
        let counters = &compilation.generation.templates.values().next().unwrap().counters;
        assert!(counters[1].object_name.is_empty());
        for counter in &counters[2..] {
            assert!(Arc::ptr_eq(&counters[1].object_name, &counter.object_name));
        }
    }

    #[test]
    fn mixed_types_across_template_messages_share_one_generation() {
        let mut rows = mixed_rows("p", 0, 300);
        let mut wire = Vec::new();
        for (index, (_, label, t, _)) in MIXED_GROUPS.iter().enumerate() {
            let part =
                template_message("unused", 0, 300 + index as u16, &[(*label, (t << 16) | 7)]);
            wire.extend_from_slice(part.templates.as_ref().unwrap());
        }
        let mut actor = actor();
        for row in &mut rows {
            row.templates = Some(Arc::new(wire.clone()));
            actor.handle_template(row.clone()).unwrap();
        }
        assert_eq!(actor.installed.len(), 4);
        assert_eq!(actor.sessions.len(), 1);
        let sets: Vec<_> = (300..304)
            .map(|id| (id, vec![(42, vec![u64::from(id)])]))
            .collect();
        let batch = actor.handle_record(&data_message(0, &sets)).unwrap();
        for (record, (_, _, t, name)) in batch.iter().zip(MIXED_GROUPS) {
            let stat = record.stats.get(0).unwrap();
            assert_eq!(stat.type_id, t);
            assert_eq!(stat.object_name.as_ref(), name);
        }
    }

    #[test]
    fn mixed_transition_collision_does_not_retire_single_or_claim_opaque_group() {
        let mut actor = actor();
        actor
            .handle_template(snapshot("p|PORT", &[(0, 600, 0x0001_0001)]))
            .unwrap();
        actor
            .handle_template(snapshot("peer", &[(0, 300, 1)]))
            .unwrap();
        let raw = format!("{:?}", actor.contributions);
        let sessions = actor.sessions.clone();
        assert!(actor
            .handle_template(mixed_rows("p", 0, 300)[0].clone())
            .is_err());
        assert_eq!(actor.sessions, sessions);
        assert_eq!(format!("{:?}", actor.contributions), raw);

        // A QUEUE-suffixed opaque owner actually carrying a non-QUEUE type is
        // not evidence of a prior SINGLE group that MIXED may retire.
        actor
            .handle_template(snapshot("p|QUEUE", &[(0, 400, 0x0002_0001)]))
            .unwrap();
        let raw = format!("{:?}", actor.contributions);
        let sessions = actor.sessions.clone();
        assert!(actor
            .handle_template(mixed_rows("p", 0, 400)[1].clone())
            .is_err());
        assert!(actor
            .handle_template(mixed_rows("p", 0, 500)[1].clone())
            .is_err());
        assert_eq!(actor.sessions, sessions);
        assert_eq!(format!("{:?}", actor.contributions), raw);
    }

    #[test]
    fn mixed_does_not_relax_single_or_opaque_owners() {
        for owner in ["opaque", "p|queue", "p|QUEUE|extra", "|QUEUE"] {
            let mut actor = actor();
            let mut row = mixed_rows("p", 0, 300)[0].clone();
            row.key = owner.into();
            assert!(actor.handle_template(row).is_err());
            assert!(actor.contributions.is_empty());
            let complete = template_message(owner, 0, 300, &[(1, 0x0001_0001), (2, 0x0015_0001)]);
            actor.handle_template(complete).unwrap();
            assert!(actor.sessions.contains_key(owner));
        }
        let mut actor = actor();
        for (group, _, t, _) in MIXED_GROUPS {
            let owner = format!("p|{group}");
            let mut row = template_message(
                &owner,
                0,
                300 + t as u16,
                &[(1, (t << 16) | 1), (2, (t << 16) | 2)],
            );
            row.object_ids = Some(vec![1]);
            row.object_names = Some(vec!["one".into()]);
            assert!(actor.handle_template(row).is_err());
            actor
                .handle_template(snapshot(&owner, &[(0, 300 + t as u16, (t << 16) | 1)]))
                .unwrap();
        }
        assert_eq!(actor.sessions.len(), 4);
        actor
            .handle_template(IPFixTemplatesMessage::delete("p|QUEUE".into()))
            .unwrap();
        assert_eq!(actor.sessions.len(), 3);
    }

    // Field specifiers preserve the E bit independently of the IE number.
    fn hardware_template(id: u16, fields: &[(u16, u16, Option<u32>)]) -> IPFixTemplatesMessage {
        let mut bytes = vec![0; IPFIX_HEADER_LEN + SET_HEADER_LEN];
        bytes[0..2].copy_from_slice(&IPFIX_VERSION.to_be_bytes());
        bytes[16..18].copy_from_slice(&TEMPLATE_SET_ID.to_be_bytes());
        bytes.extend_from_slice(&id.to_be_bytes());
        bytes.extend_from_slice(&u16::try_from(fields.len()).unwrap().to_be_bytes());
        let mut objects = std::collections::BTreeSet::new();
        for &(ie, len, pen) in fields {
            bytes.extend_from_slice(&(ie | if pen.is_some() { 0x8000 } else { 0 }).to_be_bytes());
            bytes.extend_from_slice(&len.to_be_bytes());
            if let Some(pen) = pen {
                bytes.extend_from_slice(&pen.to_be_bytes());
                if pen != 0 {
                    objects.insert(ie);
                }
            }
        }
        let len = u16::try_from(bytes.len()).unwrap();
        bytes[2..4].copy_from_slice(&len.to_be_bytes());
        bytes[18..20].copy_from_slice(&(len - 16).to_be_bytes());
        IPFixTemplatesMessage::new(
            "hardware".into(),
            Arc::new(bytes),
            Some(objects.iter().map(|id| format!("Ethernet{id}")).collect()),
            Some(objects.into_iter().collect()),
        )
    }

    fn hardware_data(id: u16, records: &[&[u8]]) -> Vec<u8> {
        let mut bytes = vec![0; IPFIX_HEADER_LEN + SET_HEADER_LEN];
        bytes[0..2].copy_from_slice(&IPFIX_VERSION.to_be_bytes());
        bytes[16..18].copy_from_slice(&id.to_be_bytes());
        for record in records {
            bytes.extend_from_slice(record);
        }
        let len = u16::try_from(bytes.len()).unwrap();
        bytes[2..4].copy_from_slice(&len.to_be_bytes());
        bytes[18..20].copy_from_slice(&(len - 16).to_be_bytes());
        bytes
    }

    fn utc_nanos() -> u64 {
        SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .unwrap()
            .as_nanos() as u64
    }

    fn timestamp_actor() -> IpfixActor {
        let mut actor = actor();
        let mut update = hardware_template(256, &[(325, 8, None), (1, 8, Some(1))]);
        let continuation = hardware_template(257, &[(1, 8, Some(1))]);
        Arc::make_mut(update.templates.as_mut().unwrap())
            .extend_from_slice(continuation.templates.as_ref().unwrap());
        actor.handle_template(update).unwrap();
        actor
    }

    fn append_hardware_set(message: &mut Vec<u8>, next: &[u8]) {
        message.extend_from_slice(&next[IPFIX_HEADER_LEN..]);
        let len = u16::try_from(message.len()).unwrap();
        message[2..4].copy_from_slice(&len.to_be_bytes());
    }

    #[test]
    fn timestamp_history_follows_processing_order_across_buffers() {
        for separate_buffers in [false, true] {
            let mut actor = timestamp_actor();
            for time in [100u64, 42, 0] {
                let mut record = time.to_be_bytes().to_vec();
                record.extend_from_slice(&7u64.to_be_bytes());
                let mut input = hardware_data(256, &[&record]);
                let continuation = hardware_data(257, &[&8u64.to_be_bytes()]);
                let batch = if separate_buffers {
                    let batch = actor.handle_record(&input).unwrap();
                    assert_eq!(batch.iter().next().unwrap().observation_time, time);
                    actor.handle_record(&continuation).unwrap()
                } else {
                    input.extend_from_slice(&continuation);
                    actor.handle_record(&input).unwrap()
                };
                assert!(batch.iter().all(|r| r.observation_time == time));
                assert_eq!(actor.last_observation_time, Some(time));
            }
        }
    }

    #[test]
    fn message_local_time_overrides_history_but_explicit_records_keep_their_own() {
        let mut actor = timestamp_actor();
        actor
            .handle_record(&data_message(0, &[(256, vec![(999, vec![1])])]))
            .unwrap();
        let continuation = hardware_data(257, &[&7u64.to_be_bytes()]);
        let mut input = continuation.clone();
        append_hardware_set(
            &mut input,
            &data_message(0, &[(256, vec![(100, vec![2]), (42, vec![3])])]),
        );
        append_hardware_set(&mut input, &continuation);
        append_hardware_set(&mut input, &data_message(0, &[(256, vec![(0, vec![4])])]));
        append_hardware_set(&mut input, &continuation);
        input.extend_from_slice(&continuation);
        let batch = actor.handle_record(&input).unwrap();
        assert_eq!(
            batch.iter().map(|r| r.observation_time).collect::<Vec<_>>(),
            vec![0, 100, 42, 0, 0, 0, 0]
        );
        assert_eq!(actor.last_observation_time, Some(0));
    }

    #[test]
    fn startup_fallback_never_seeds_history_or_looks_into_a_later_message() {
        let mut actor = timestamp_actor();
        let continuation = hardware_data(257, &[&7u64.to_be_bytes()]);
        for _ in 0..2 {
            let before = utc_nanos();
            let batch = actor.handle_record(&continuation).unwrap();
            assert!((before..=utc_nanos()).contains(&batch.iter().next().unwrap().observation_time));
            assert_eq!(actor.last_observation_time, None);
        }
        let mut input = continuation;
        input.extend_from_slice(&data_message(0, &[(256, vec![(42, vec![1])])]));
        let before = utc_nanos();
        let batch = actor.handle_record(&input).unwrap();
        assert!((before..=utc_nanos()).contains(&batch.iter().next().unwrap().observation_time));
        assert_eq!(batch.iter().nth(1).unwrap().observation_time, 42);
        assert_eq!(actor.last_observation_time, Some(42));
    }

    #[test]
    fn partial_split_time_uses_message_or_history_fallback() {
        for ie in [322, 325] {
            let mut actor = timestamp_actor();
            let mut update = hardware_template(258, &[(ie, 4, None), (1, 8, Some(1))]);
            update.key = "partial".into();
            actor.handle_template(update).unwrap();
            let record = [0xff; 12];
            let partial = hardware_data(258, &[&record]);
            let before = utc_nanos();
            let batch = actor.handle_record(&partial).unwrap();
            assert!((before..=utc_nanos()).contains(&batch.iter().next().unwrap().observation_time));
            assert_eq!(actor.last_observation_time, None);
            let mut input = partial.clone();
            append_hardware_set(&mut input, &data_message(0, &[(256, vec![(42, vec![1])])]));
            assert!(actor
                .handle_record(&input)
                .unwrap()
                .iter()
                .all(|r| r.observation_time == 42));
            let batch = actor.handle_record(&partial).unwrap();
            assert_eq!(batch.iter().next().unwrap().observation_time, 42);
            assert_eq!(actor.last_observation_time, Some(42));
        }
    }

    #[test]
    fn timestamp_inference_excludes_descriptors_retired_by_earlier_sets() {
        for same_message in [false, true] {
            for old_first in [false, true] {
                let mut actor = timestamp_actor();
                actor
                    .handle_record(&data_message(0, &[(256, vec![(42, vec![1])])]))
                    .unwrap();
                actor
                    .handle_template(hardware_template(258, &[(1, 8, Some(1))]))
                    .unwrap();
                let old = data_message(0, &[(256, vec![(99, vec![2])])]);
                let new = hardware_data(258, &[&3u64.to_be_bytes()]);
                let mut input = if old_first { old.clone() } else { new.clone() };
                let next = if old_first { &new } else { &old };
                if same_message {
                    append_hardware_set(&mut input, next);
                } else {
                    input.extend_from_slice(next);
                }
                let batch = actor.handle_record(&input).unwrap();
                let expected = if old_first { 99 } else { 42 };
                assert_eq!(batch.record_count(), if old_first { 2 } else { 1 });
                assert!(batch.iter().all(|r| r.observation_time == expected));
                assert_eq!(actor.last_observation_time, Some(expected));
                assert_eq!(keys(&actor), vec![(0, 258)]);
                assert!(actor.handle_record(&old).unwrap().is_empty());
                assert_eq!(actor.last_observation_time, Some(expected));
            }
        }
    }

    #[test]
    fn timestamp_history_is_actor_wide_but_instances_are_isolated() {
        let mut first = timestamp_actor();
        let mut second = timestamp_actor();
        first
            .handle_record(&data_message(0, &[(256, vec![(42, vec![1])])]))
            .unwrap();
        let continuation = hardware_data(257, &[&7u64.to_be_bytes()]);
        let before = utc_nanos();
        let batch = second.handle_record(&continuation).unwrap();
        assert!((before..=utc_nanos()).contains(&batch.iter().next().unwrap().observation_time));
        assert_eq!(second.last_observation_time, None);
        second
            .handle_record(&data_message(0, &[(256, vec![(99, vec![1])])]))
            .unwrap();
        assert_eq!(first.last_observation_time, Some(42));

        first
            .handle_template(IPFixTemplatesMessage::delete("hardware".into()))
            .unwrap();
        let mut update = hardware_template(257, &[(1, 8, Some(1))]);
        update.key = "other".into();
        Arc::make_mut(update.templates.as_mut().unwrap())[12..16]
            .copy_from_slice(&1u32.to_be_bytes());
        first.handle_template(update).unwrap();
        let mut continuation = continuation;
        continuation[12..16].copy_from_slice(&1u32.to_be_bytes());
        let batch = first.handle_record(&continuation).unwrap();
        assert_eq!(batch.iter().next().unwrap().observation_time, 42);
    }

    #[test]
    fn invalid_unknown_and_padding_only_inputs_never_update_time() {
        for seed in [None, Some(42)] {
            let mut actor = timestamp_actor();
            if let Some(time) = seed {
                actor
                    .handle_record(&data_message(0, &[(256, vec![(time, vec![1])])]))
                    .unwrap();
            }
            assert_eq!(actor.last_observation_time, seed);
            let good = data_message(0, &[(256, vec![(99, vec![1])])]);
            let padding_only = hardware_data(256, &[&[0; 8]]);
            let mut bad_later_message = good.clone();
            bad_later_message.extend_from_slice(&padding_only);
            let mut bad_later_set = good.clone();
            append_hardware_set(&mut bad_later_set, &padding_only);
            let mut bad_trailer = good;
            bad_trailer.extend_from_slice(&[1, 2, 3]);
            for input in [bad_later_message, bad_later_set, bad_trailer, padding_only] {
                assert!(actor.handle_record(&input).is_err());
                assert_eq!(actor.last_observation_time, seed);
            }
            let unknown = data_message(0, &[(999, vec![(123, vec![1])])]);
            assert!(actor.handle_record(&unknown).unwrap().is_empty());
            assert_eq!(actor.last_observation_time, seed);
            let mut continuation = hardware_data(257, &[&7u64.to_be_bytes()]);
            append_hardware_set(&mut continuation, &unknown);
            let before = utc_nanos();
            let batch = actor.handle_record(&continuation).unwrap();
            let time = batch.iter().next().unwrap().observation_time;
            match seed {
                Some(expected) => assert_eq!(time, expected),
                None => assert!((before..=utc_nanos()).contains(&time)),
            }
            assert_eq!(actor.last_observation_time, seed);
        }
    }

    #[tokio::test]
    async fn slow_sink_preflush_keeps_inherited_time_without_future_message_leakage() {
        let mut actor = timestamp_actor();
        actor
            .handle_record(&data_message(0, &[(256, vec![(42, vec![1])])]))
            .unwrap();
        let (tx, mut rx) = channel(1);
        tx.send(Arc::new(SAIStatsBatch::default())).await.unwrap();
        actor.add_recipient(tx);
        let mut batch = SAIStatsBatch::default();
        batch.push_record(
            7,
            (0..TARGET_COUNTERS_PER_BATCH).map(|_| SAIStat::new("x", 1, 1, 1)),
        );
        let mut input = hardware_data(257, &[&7u64.to_be_bytes()]);
        input.extend_from_slice(&data_message(0, &[(256, vec![(99, vec![1])])]));
        {
            let processing = actor.process_record_input(&input, &mut batch);
            tokio::pin!(processing);
            tokio::select! {
                _ = &mut processing => panic!("preflush must wait for the full sink"),
                _ = tokio::time::sleep(Duration::from_millis(20)) => {}
            }
            assert!(rx.recv().await.unwrap().is_empty());
            processing.await;
        }
        assert_eq!(
            rx.recv().await.unwrap().counter_count(),
            TARGET_COUNTERS_PER_BATCH
        );
        assert_eq!(
            batch.iter().map(|r| r.observation_time).collect::<Vec<_>>(),
            vec![42, 99]
        );
        assert_eq!(actor.last_observation_time, Some(99));
    }

    #[test]
    fn sn5640_split_timestamp_and_placeholder_layout_decodes_both_templates() {
        // Minimized from the SN5640 capture: standard322/4 +325/4,
        // enterprise325 is a counter, repeated zero-PEN slots, continuation
        // without timestamp. Placeholder payload is deliberately nonzero.
        let mut first = hardware_template(
            256,
            &[
                (322, 4, None),
                (325, 4, None),
                (325, 8, Some(0x0015_0001)),
                (0, 8, Some(0)),
                (0, 4, Some(0)),
                (0, 8, Some(0)),
                (326, 4, Some(0x0015_0029)),
            ],
        );
        let second = hardware_template(
            257,
            &[
                (0, 8, Some(0)),
                (0, 4, Some(0)),
                (0, 8, Some(0)),
                (325, 4, Some(0x0015_0029)),
            ],
        );
        Arc::make_mut(first.templates.as_mut().unwrap())
            .extend_from_slice(second.templates.as_ref().unwrap());
        let mut actor = actor();
        actor.handle_template(first).unwrap();
        assert_eq!(actor.installed.len(), 2);
        let one = &actor.installed[&TemplateKey {
            observation_domain_id: 0,
            template_id: 256,
        }];
        assert_eq!(one.record_len, 40);
        assert_eq!(
            one.counters.iter().map(|c| c.offset).collect::<Vec<_>>(),
            vec![8, 36]
        );
        assert_eq!(
            one.observation_time,
            ObservationTime::Split {
                seconds_offset: 0,
                nanos_offset: 4
            }
        );
        let two = &actor.installed[&TemplateKey {
            observation_domain_id: 0,
            template_id: 257,
        }];
        assert_eq!(two.record_len, 24);
        assert_eq!(two.observation_time, ObservationTime::Missing);
        let mut record = Vec::new();
        record.extend_from_slice(&1_788_655_919u32.to_be_bytes());
        record.extend_from_slice(&123_456_789u32.to_be_bytes());
        record.extend_from_slice(&0xfedc_ba98_7654_3210u64.to_be_bytes());
        record.extend_from_slice(&[0xaa; 20]);
        record.extend_from_slice(&0xf123_4567u32.to_be_bytes());
        let mut next_record = record.clone();
        next_record[4..8].copy_from_slice(&123_456_790u32.to_be_bytes());
        let mut continuation = vec![0xbb; 20];
        continuation.extend_from_slice(&0x8765_4321u32.to_be_bytes());
        let mut data = hardware_data(256, &[&record, &next_record]);
        data.extend_from_slice(&hardware_data(257, &[&continuation]));
        let output = actor.handle_record(&data).unwrap();
        let records = output.iter().collect::<Vec<_>>();
        assert_eq!(output.counter_count(), 5); // No unknown_0 placeholder metrics.
        assert_eq!(records[0].observation_time, 1_788_655_919_123_456_789);
        assert_eq!(records[1].observation_time, 1_788_655_919_123_456_790);
        assert_eq!(records[2].observation_time, 1_788_655_919_123_456_790);
        assert_eq!(records[0].stats.get(0).unwrap().counter, 0xfedc_ba98_7654_3210);
        assert_eq!(records[0].stats.get(0).unwrap().object_name.as_ref(), "Ethernet325");
        assert_eq!(
            (records[0].stats.get(0).unwrap().type_id, records[0].stats.get(0).unwrap().stat_id),
            (21, 1)
        );
        assert_eq!(records[0].stats.get(1).unwrap().counter, 0xf123_4567);
        assert_eq!(records[2].stats.get(0).unwrap().counter, 0x8765_4321);
    }

    #[test]
    fn sn5640_capture_sized_concatenated_templates_decode_all_real_counters() {
        // Same field counts, widths, placeholder counts and message sizes as
        // the captured SN5640 Queue snapshot; object IDs are normalized here.
        let mut snapshot = None;
        let mut input = Vec::new();
        for (id, objects, placeholder8, placeholder4, wire_len, record_len) in [
            (256, 472u16, 4704, 1568, 65312, 57128),
            (257, 400u16, 3864, 1288, 54040, 47264),
        ] {
            let mut fields = Vec::new();
            let mut record = Vec::new();
            if id == 256 {
                fields.extend([(322, 4, None), (325, 4, None)]);
                record.extend_from_slice(&100u32.to_be_bytes());
                record.extend_from_slice(&200u32.to_be_bytes());
            }
            for object in 1..=objects {
                for (stat, width) in [(1u32, 8u16), (34, 8), (42, 8), (41, 4)] {
                    fields.push((object, width, Some(0x0015_0000 | stat)));
                    let value = u64::from(object) * 100 + u64::from(stat);
                    record.extend_from_slice(&value.to_be_bytes()[8 - usize::from(width)..]);
                }
            }
            for _ in 0..placeholder8 {
                fields.push((0, 8, Some(0)));
                record.extend_from_slice(&[0xfe; 8]);
            }
            for _ in 0..placeholder4 {
                fields.push((0, 4, Some(0)));
                record.extend_from_slice(&[0xfd; 4]);
            }
            let update = hardware_template(id, &fields);
            assert_eq!(update.templates.as_ref().unwrap().len(), wire_len);
            assert_eq!(record.len(), record_len);
            if let Some(previous) = &mut snapshot {
                let previous: &mut IPFixTemplatesMessage = previous;
                Arc::make_mut(previous.templates.as_mut().unwrap())
                    .extend_from_slice(update.templates.as_ref().unwrap());
            } else {
                snapshot = Some(update);
            }
            input.extend_from_slice(&hardware_data(id, &[&record]));
        }
        let mut actor = actor();
        actor.handle_template(snapshot.unwrap()).unwrap();
        let batch = actor.handle_record(&input).unwrap();
        assert_eq!(batch.counter_count(), 3488);
        assert_eq!(batch.record_count(), 2);
        for (index, record) in batch.iter().enumerate() {
            assert_eq!(record.stats.len(), if index == 0 { 1888 } else { 1600 });
            assert_eq!(record.observation_time, 100_000_000_200);
            for (counter_index, stat) in record.stats.iter().enumerate() {
                let object = (counter_index / 4 + 1) as u64;
                let expected_stat = [1, 34, 42, 41][counter_index % 4];
                assert_eq!((stat.type_id, stat.stat_id), (21, expected_stat));
                assert_eq!(stat.counter, object * 100 + u64::from(expected_stat));
                assert_eq!(stat.object_name.as_ref(), format!("Ethernet{object}"));
            }
        }
    }

    #[test]
    fn scale_2048_ports_eight_queues_sixty_stats_compiles_and_decodes() {
        const OBJECTS: usize = 2048 * 8;
        const OBJECTS_PER_TEMPLATE: usize = 128;
        const STATS: usize = 60;
        const COUNTERS_PER_TEMPLATE: usize = OBJECTS_PER_TEMPLATE * STATS;
        const TEMPLATES: usize = OBJECTS / OBJECTS_PER_TEMPLATE;
        const MESSAGE_BYTES: usize = 61_472;
        const RECORD_BYTES: usize = 61_448;

        // Build one wire message at a time, not a million-element field-spec vector.
        let mut snapshot = Vec::with_capacity(TEMPLATES * MESSAGE_BYTES);
        let mut fields = Vec::with_capacity(COUNTERS_PER_TEMPLATE + 2);
        for index in 0..TEMPLATES {
            fields.clear();
            fields.extend([(322, 4, None), (325, 4, None)]);
            for object in index * OBJECTS_PER_TEMPLATE + 1..=(index + 1) * OBJECTS_PER_TEMPLATE {
                for stat in 1..=STATS {
                    fields.push((
                        u16::try_from(object).unwrap(),
                        8,
                        Some(0x0015_0000 | u32::try_from(stat).unwrap()),
                    ));
                }
            }
            let update = hardware_template(256 + u16::try_from(index).unwrap(), &fields);
            let bytes = update.templates.as_ref().unwrap();
            assert_eq!(bytes.len(), MESSAGE_BYTES);
            assert_eq!(
                usize::from(NetworkEndian::read_u16(&bytes[2..4])),
                bytes.len()
            );
            assert_eq!(NetworkEndian::read_u16(&bytes[22..24]), 7682);
            snapshot.extend_from_slice(bytes);
        }
        drop(fields);
        assert_eq!(snapshot.len(), 7_868_416);
        assert!(snapshot.len() > 4 * 1024 * 1024);
        // Four hardware placeholder slots per counter still fit the config budget.
        assert_eq!(5 * snapshot.len(), 39_342_080);
        assert!(5 * snapshot.len() <= MAX_TEMPLATE_CONFIG_BYTES);
        let update = IPFixTemplatesMessage::new(
            "scale".into(),
            Arc::new(snapshot),
            Some(
                (0..OBJECTS)
                    .map(|i| format!("Ethernet{}|{}", i / 8, i % 8))
                    .collect(),
            ),
            Some((1..=OBJECTS).map(|id| u16::try_from(id).unwrap()).collect()),
        );
        let mut actor = actor();
        // Exercise admission, whole-candidate compilation and installation once.
        actor.handle_template(update).unwrap();
        assert_eq!(actor.installed.len(), TEMPLATES);
        assert_eq!(actor.sessions["scale"].active.templates.len(), TEMPLATES);
        assert_eq!(
            actor
                .installed
                .values()
                .map(|t| t.counters.len())
                .sum::<usize>(),
            983_040
        );
        for index in 0..TEMPLATES {
            let template = &actor.installed[&TemplateKey {
                observation_domain_id: 0,
                template_id: 256 + u16::try_from(index).unwrap(),
            }];
            assert_eq!(template.record_len, RECORD_BYTES);
            assert_eq!(template.counters.len(), COUNTERS_PER_TEMPLATE);
            assert_eq!(
                template.observation_time,
                ObservationTime::Split {
                    seconds_offset: 0,
                    nanos_offset: 4,
                }
            );
            for counter_index in [0, COUNTERS_PER_TEMPLATE - 1] {
                let counter = &template.counters[counter_index];
                let object = index * OBJECTS_PER_TEMPLATE + counter_index / STATS;
                assert_eq!(
                    counter.object_name.as_ref(),
                    format!("Ethernet{}|{}", object / 8, object % 8)
                );
                assert_eq!(
                    (counter.type_id, counter.stat_id),
                    (21, (counter_index % STATS + 1) as u32)
                );
                assert_eq!(counter.offset, 8 + counter_index * 8);
                assert_eq!(counter.len, 8);
            }
        }

        // Decode only the boundary templates; do not allocate a million samples.
        for (index, last_name) in [(0, "Ethernet15|7"), (TEMPLATES - 1, "Ethernet2047|7")] {
            let mut record = Vec::with_capacity(RECORD_BYTES);
            record.extend_from_slice(&1_788_655_919u32.to_be_bytes());
            record.extend_from_slice(&123_456_789u32.to_be_bytes());
            for counter in 0..COUNTERS_PER_TEMPLATE {
                let value = (index * COUNTERS_PER_TEMPLATE + counter + 1) as u64;
                record.extend_from_slice(&value.to_be_bytes());
            }
            assert_eq!(record.len(), RECORD_BYTES);
            let data = hardware_data(256 + u16::try_from(index).unwrap(), &[&record]);
            assert_eq!(data.len(), 61_468);
            let batch = actor.handle_record(&data).unwrap();
            assert_eq!(batch.record_count(), 1);
            assert_eq!(batch.counter_count(), COUNTERS_PER_TEMPLATE);
            let decoded = batch.iter().next().unwrap();
            assert_eq!(decoded.observation_time, 1_788_655_919_123_456_789);
            let last = decoded.stats.iter().last().unwrap();
            assert_eq!(last.object_name.as_ref(), last_name);
            assert_eq!((last.type_id, last.stat_id), (21, 60));
            assert_eq!(last.counter, ((index + 1) * COUNTERS_PER_TEMPLATE) as u64);
        }
    }

    #[tokio::test]
    async fn future_queue_scale_with_one_set_per_queue_is_not_rejected() {
        const OBJECTS: usize = 2048 * 8;
        const STATS: usize = 60;
        let mut templates = Vec::with_capacity(OBJECTS * 512);
        let mut input = Vec::with_capacity(OBJECTS * 508);
        let mut fields = Vec::with_capacity(STATS + 2);
        let mut record = Vec::with_capacity(488);
        for index in 0..OBJECTS {
            let id = u16::try_from(index + 256).unwrap();
            fields.clear();
            fields.extend([(322, 4, None), (325, 4, None)]);
            record.clear();
            record.extend_from_slice(&100u32.to_be_bytes());
            record.extend_from_slice(&(index as u32).to_be_bytes());
            for stat in 1..=STATS {
                fields.push(((index + 1) as u16, 8, Some(0x0015_0000 | stat as u32)));
                record.extend_from_slice(&((index * STATS + stat) as u64).to_be_bytes());
            }
            let template = hardware_template(id, &fields);
            templates.extend_from_slice(template.templates.as_ref().unwrap());
            input.extend_from_slice(&hardware_data(id, &[&record]));
        }
        assert_eq!(templates.len(), 8_388_608);
        assert_eq!(input.len(), 8_323_072);
        let mut actor = actor();
        actor
            .handle_template(IPFixTemplatesMessage::new(
                "queue-scale".into(),
                Arc::new(templates),
                Some(
                    (0..OBJECTS)
                        .map(|index| format!("Ethernet{}|{}", index / 8, index % 8))
                        .collect(),
                ),
                Some((1..=OBJECTS).map(|id| id as u16).collect()),
            ))
            .unwrap();
        assert_eq!(actor.installed.len(), OBJECTS);
        let (tx, mut rx) = channel(1);
        actor.add_recipient(tx);
        let producer = async {
            let mut batch = SAIStatsBatch::default();
            actor.process_record_input(&input, &mut batch).await;
            actor.send_batch(batch).await;
            drop(actor);
        };
        let consumer = async {
            let mut records = 0usize;
            let mut counters = 0usize;
            while let Some(batch) = rx.recv().await {
                assert!(batch.counter_count() <= TARGET_COUNTERS_PER_BATCH);
                for record in batch.iter() {
                    assert_eq!(record.observation_time, 100_000_000_000 + records as u64);
                    assert_eq!(record.stats.len(), STATS);
                    for (index, stat) in record.stats.iter().enumerate() {
                        assert_eq!(
                            stat.object_name.as_ref(),
                            format!("Ethernet{}|{}", records / 8, records % 8)
                        );
                        assert_eq!((stat.type_id, stat.stat_id), (21, index as u32 + 1));
                        assert_eq!(stat.counter, (counters + index + 1) as u64);
                    }
                    records += 1;
                    counters += STATS;
                }
            }
            assert_eq!(records, OBJECTS);
            assert_eq!(counters, 983_040);
        };
        tokio::time::timeout(Duration::from_secs(30), async {
            tokio::join!(producer, consumer);
        })
        .await
        .unwrap();
    }

    #[test]
    fn set_count_limit_accepts_boundary_and_rejects_next_set_atomically() {
        let mut actor = actor();
        actor
            .handle_template(snapshot("boundary", &[(0, 300, 1)]))
            .unwrap();
        let mut input = Vec::new();
        // Stay within each IPFIX message's independent 16-bit length bound.
        let per_message = (u16::MAX as usize - IPFIX_HEADER_LEN) / 20;
        let mut remaining = MAX_DATA_SETS_PER_RECORD_INPUT;
        while remaining > 0 {
            let count = remaining.min(per_message);
            let sets = vec![(300, vec![(1, vec![10])]); count];
            input.extend_from_slice(&data_message(0, &sets));
            remaining -= count;
        }
        let output = actor.handle_record(&input).unwrap();
        assert_eq!(output.record_count(), MAX_DATA_SETS_PER_RECORD_INPUT);
        assert_eq!(output.counter_count(), MAX_DATA_SETS_PER_RECORD_INPUT);
        let prior_time = actor.last_observation_time;
        input.extend_from_slice(&data_message(0, &[(300, vec![(2, vec![20])])]));
        assert!(actor
            .handle_record(&input)
            .unwrap_err()
            .to_string()
            .contains("data Sets"));
        assert_eq!(actor.last_observation_time, prior_time);
    }

    #[test]
    fn scale_placeholder_layout_decodes_and_projects_above_32_mib() {
        const COUNTERS: usize = 1536;
        const PADDED_TEMPLATES: usize = 640;
        let mut fields = Vec::with_capacity(COUNTERS * 5 + 2);
        let mut record = Vec::with_capacity(61_448);
        fields.extend([(322, 4, None), (325, 4, None)]);
        record.extend_from_slice(&100u32.to_be_bytes());
        record.extend_from_slice(&200u32.to_be_bytes());
        for counter in 0..COUNTERS {
            // Interleave nonzero placeholder payload before every real counter.
            fields.extend([(0, 8, Some(0)); 4]);
            record.extend_from_slice(&[0xfe; 32]);
            fields.push((
                (counter / 60 + 1) as u16,
                8,
                Some(0x0015_0000 | (counter % 60 + 1) as u32),
            ));
            record.extend_from_slice(&(counter as u64 + 1).to_be_bytes());
        }
        let update = hardware_template(256, &fields);
        let message_bytes = update.templates.as_ref().unwrap().len();
        assert_eq!(message_bytes, 61_472);
        assert_eq!(PADDED_TEMPLATES * COUNTERS, 2048 * 8 * 60);
        // Size projection only: parse one representative padded template, not 5M fields.
        let projected_bytes = PADDED_TEMPLATES * message_bytes;
        assert_eq!(projected_bytes, 39_342_080);
        assert!(projected_bytes > 32 * 1024 * 1024);
        assert!(projected_bytes <= MAX_TEMPLATE_CONFIG_BYTES);

        let mut actor = actor();
        actor.handle_template(update).unwrap();
        assert_eq!(actor.installed.len(), 1);
        let template = &actor.installed[&TemplateKey {
            observation_domain_id: 0,
            template_id: 256,
        }];
        assert_eq!(template.record_len, 61_448);
        assert_eq!(template.counters.len(), COUNTERS);
        for (index, counter) in template.counters.iter().enumerate() {
            assert_eq!(counter.offset, 40 + index * 40);
        }
        assert_eq!(record.len(), template.record_len);
        let batch = actor
            .handle_record(&hardware_data(256, &[&record]))
            .unwrap();
        assert_eq!(batch.record_count(), 1);
        assert_eq!(batch.counter_count(), COUNTERS);
        let decoded = batch.iter().next().unwrap();
        assert_eq!(decoded.observation_time, 100_000_000_200);
        for (index, stat) in decoded.stats.iter().enumerate() {
            assert_eq!(
                stat.object_name.as_ref(),
                format!("Ethernet{}", index / 60 + 1)
            );
            assert_eq!((stat.type_id, stat.stat_id), (21, (index % 60 + 1) as u32));
            assert_eq!(stat.counter, index as u64 + 1);
        }
    }

    #[test]
    fn timestamp_priority_offsets_and_incomplete_fallback_match_legacy() {
        for fields in [
            vec![(322, 4, None), (325, 4, None)],
            vec![(325, 4, None), (322, 4, None)],
            vec![(322, 4, None), (325, 4, None), (325, 8, None)],
            vec![(325, 8, None), (325, 4, None), (322, 4, None)],
            vec![(322, 4, None)],
            vec![(325, 4, None)],
            vec![],
        ] {
            let mut specs = vec![(322, 3, Some(0x0001_0001))];
            specs.extend(fields.iter().copied());
            specs.push((325, 6, Some(0x0001_0002)));
            let mut record = vec![0xff; 3];
            for &(ie, len, _) in &fields {
                match (ie, len) {
                    (322, 4) => record.extend_from_slice(&u32::MAX.to_be_bytes()),
                    (325, 4) => record.extend_from_slice(&u32::MAX.to_be_bytes()),
                    (325, 8) => record.extend_from_slice(&42u64.to_be_bytes()),
                    _ => unreachable!(),
                }
            }
            record.extend_from_slice(&[0xff; 6]);
            let mut actor = actor();
            actor
                .handle_template(hardware_template(300, &specs))
                .unwrap();
            let before = utc_nanos();
            let batch = actor
                .handle_record(&hardware_data(300, &[&record]))
                .unwrap();
            let after = utc_nanos();
            let output = batch.iter().next().unwrap();
            if fields.iter().any(|&(id, len, _)| id == 325 && len == 8) {
                assert_eq!(output.observation_time, 42);
            } else if fields.len() == 2 {
                assert_eq!(
                    output.observation_time,
                    u64::from(u32::MAX) * 1_000_000_000 + u64::from(u32::MAX)
                );
            } else {
                assert!((before..=after).contains(&output.observation_time));
            }
            assert_eq!(output.stats.get(0).unwrap().counter, 0xff_ffff);
            assert_eq!(output.stats.get(1).unwrap().counter, 0xffff_ffff_ffff);
        }
    }

    #[test]
    fn hardware_layout_keeps_strict_malformed_field_validation() {
        let counter = (1, 8, Some(0x0001_0001));
        for fields in [
            vec![(322, 8, None), counter],
            vec![(325, 3, None), counter],
            vec![(322, 4, None), (322, 4, None), counter],
            vec![(325, 4, None), (325, 4, None), counter],
            vec![(325, 8, None), (325, 8, None), counter],
            vec![(0, 0, Some(0)), counter],
            vec![(0, 9, Some(0)), counter],
            vec![(0, u16::MAX, Some(0)), counter],
            vec![(1, 8, Some(0)), counter],
            vec![counter, counter],
            vec![(0, 8, Some(0))],
            vec![(322, 4, None), (325, 4, None)],
            vec![],
        ] {
            let mut update = hardware_template(300, &fields);
            // Exercise template validation rather than empty-metadata rejection.
            update.object_names = Some(vec!["Ethernet1".into()]);
            update.object_ids = Some(vec![1]);
            assert!(
                IpfixActor::compile_generation(&update).is_err(),
                "{fields:?}"
            );
        }
        let valid = hardware_template(
            300,
            &[(322, 4, None), (325, 4, None), (0, 8, Some(0)), counter],
        );
        let bytes = valid.templates.as_ref().unwrap();
        for end in 20..bytes.len() {
            let mut truncated = valid.clone();
            let mut data = bytes[..end].to_vec();
            data[2..4].copy_from_slice(&(end as u16).to_be_bytes());
            data[18..20].copy_from_slice(&((end - 16) as u16).to_be_bytes());
            truncated.templates = Some(Arc::new(data));
            assert!(
                IpfixActor::compile_generation(&truncated).is_err(),
                "end={end}"
            );
        }
    }

    #[test]
    fn continuation_without_time_accepts_one_counter_and_repeated_placeholders() {
        for width in 1u16..=8 {
            let specs = [
                (0, width, Some(0)),
                (1, width, Some(0x0001_0001)),
                (0, width, Some(0)),
            ];
            let update = hardware_template(300, &specs);
            let mut actor = actor();
            actor.handle_template(update).unwrap();
            let bytes = vec![0xff; 3 * usize::from(width)];
            let output = actor.handle_record(&hardware_data(300, &[&bytes])).unwrap();
            assert_eq!(output.counter_count(), 1);
            assert_eq!(
                output.iter().next().unwrap().stats.get(0).unwrap().counter,
                u64::MAX >> (64 - width * 8)
            );
            actor
                .handle_template(IPFixTemplatesMessage::delete("hardware".into()))
                .unwrap();
            actor
                .handle_template(hardware_template(300, &[(1, width, Some(0x0001_0001))]))
                .unwrap();
            let batch = actor
                .handle_record(&hardware_data(300, &[&vec![0xff; usize::from(width)]]))
                .unwrap();
            assert_eq!(batch.counter_count(), 1);
        }
    }

    #[test]
    fn hardware_timestamp_change_respects_snapshot_validation_and_cutover() {
        let mut actor = actor();
        let old = hardware_template(300, &[(325, 8, None), (1, 8, Some(0x0001_0001))]);
        let candidate = hardware_template(
            301,
            &[
                (322, 4, None),
                (325, 4, None),
                (0, 4, Some(0)),
                (1, 8, Some(0x0001_0001)),
            ],
        );
        actor.handle_template(old.clone()).unwrap();
        // Width/layout changes on an in-use ID remain conflicts, not silent reinterpretation.
        let mut collision = candidate.clone();
        Arc::make_mut(collision.templates.as_mut().unwrap())[20..22]
            .copy_from_slice(&300u16.to_be_bytes());
        assert!(actor.handle_template(collision).is_err());
        actor.handle_template(candidate).unwrap();
        let mut bytes = vec![];
        bytes.extend_from_slice(&2u32.to_be_bytes());
        bytes.extend_from_slice(&3u32.to_be_bytes());
        bytes.extend_from_slice(&[0xff; 4]);
        bytes.extend_from_slice(&100u64.to_be_bytes());
        let good = hardware_data(301, &[&bytes]);
        let mut malformed = good.clone();
        malformed.extend_from_slice(&[1, 2, 3]);
        assert!(actor.handle_record(&malformed).is_err());
        assert!(actor.sessions["hardware"].pending.is_some());
        let output = actor.handle_record(&good).unwrap();
        assert_eq!(
            output.iter().next().unwrap().observation_time,
            2_000_000_003
        );
        assert!(actor.sessions["hardware"].pending.is_none());
        assert_eq!(keys(&actor), vec![(0, 301)]);
    }

    fn template_message(
        owner: &str,
        domain: u32,
        id: u16,
        fields: &[(u16, u32)],
    ) -> IPFixTemplatesMessage {
        let len = IPFIX_HEADER_LEN + 12 + fields.len() * 8;
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&IPFIX_VERSION.to_be_bytes());
        bytes.extend_from_slice(&(len as u16).to_be_bytes());
        bytes.extend_from_slice(&[0; 8]);
        bytes.extend_from_slice(&domain.to_be_bytes());
        bytes.extend_from_slice(&TEMPLATE_SET_ID.to_be_bytes());
        bytes.extend_from_slice(&((len - IPFIX_HEADER_LEN) as u16).to_be_bytes());
        bytes.extend_from_slice(&id.to_be_bytes());
        bytes.extend_from_slice(&((fields.len() + 1) as u16).to_be_bytes());
        bytes.extend_from_slice(&OBSERVATION_TIME_NANOSECONDS.to_be_bytes());
        bytes.extend_from_slice(&OBSERVATION_TIME_LEN.to_be_bytes());
        for (label, enterprise) in fields {
            bytes.extend_from_slice(&(0x8000 | label).to_be_bytes());
            bytes.extend_from_slice(&8u16.to_be_bytes());
            bytes.extend_from_slice(&enterprise.to_be_bytes());
        }
        let objects: HashMap<_, _> = fields
            .iter()
            .map(|(id, _)| (*id, format!("Ethernet{id}")))
            .collect();
        let (ids, names) = objects.into_iter().unzip();
        IPFixTemplatesMessage::new(owner.into(), Arc::new(bytes), Some(names), Some(ids))
    }

    fn snapshot(owner: &str, templates: &[(u32, u16, u32)]) -> IPFixTemplatesMessage {
        let mut message = template_message(
            owner,
            templates[0].0,
            templates[0].1,
            &[(1, templates[0].2)],
        );
        for (domain, id, stat) in &templates[1..] {
            let next = template_message(owner, *domain, *id, &[(1, *stat)]);
            Arc::make_mut(message.templates.as_mut().unwrap())
                .extend_from_slice(next.templates.as_ref().unwrap());
        }
        message
    }

    fn data_message(domain: u32, sets: &[(u16, Vec<(u64, Vec<u64>)>)]) -> Vec<u8> {
        let len = IPFIX_HEADER_LEN
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
        assert!(len <= u16::MAX as usize);
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&IPFIX_VERSION.to_be_bytes());
        bytes.extend_from_slice(&(len as u16).to_be_bytes());
        bytes.extend_from_slice(&[0; 8]);
        bytes.extend_from_slice(&domain.to_be_bytes());
        for (id, records) in sets {
            let len = SET_HEADER_LEN
                + records
                    .iter()
                    .map(|(_, values)| 8 + values.len() * 8)
                    .sum::<usize>();
            bytes.extend_from_slice(&id.to_be_bytes());
            bytes.extend_from_slice(&(len as u16).to_be_bytes());
            for (time, values) in records {
                bytes.extend_from_slice(&time.to_be_bytes());
                for value in values {
                    bytes.extend_from_slice(&value.to_be_bytes());
                }
            }
        }
        bytes
    }

    fn actor() -> IpfixActor {
        let (_, templates) = channel(4);
        let (_, records) = channel(4);
        IpfixActor::new(templates, records)
    }

    fn keys(actor: &IpfixActor) -> Vec<(u32, u16)> {
        let mut keys: Vec<_> = actor
            .installed
            .keys()
            .map(|key| (key.observation_domain_id, key.template_id))
            .collect();
        keys.sort_unstable();
        keys
    }

    #[test]
    fn changed_stats_promote_whole_snapshot_on_first_new_key() {
        let mut actor = actor();
        actor
            .handle_template(snapshot("peer", &[(1, 300, 1)]))
            .unwrap();
        actor
            .handle_template(snapshot("peer", &[(1, 400, 2)]))
            .unwrap();
        let peer = actor.sessions["peer"].clone();
        actor
            .handle_template(snapshot("s", &[(0, 300, 1), (0, 301, 2)]))
            .unwrap();
        actor
            .handle_template(snapshot("s", &[(0, 400, 3), (0, 401, 4)]))
            .unwrap();
        assert_eq!(
            keys(&actor),
            vec![(0, 300), (0, 301), (0, 400), (0, 401), (1, 300), (1, 400)]
        );
        let batch = actor
            .handle_record(&data_message(
                0,
                &[
                    (300, vec![(1, vec![10])]),
                    (400, vec![(2, vec![20])]),
                    (300, vec![(3, vec![30])]),
                    (301, vec![(4, vec![40])]),
                ],
            ))
            .unwrap();
        assert_eq!(
            batch
                .iter()
                .map(|record| record.observation_time)
                .collect::<Vec<_>>(),
            vec![1, 2]
        );
        assert_eq!(batch.iter().nth(1).unwrap().stats.get(0).unwrap().stat_id, 3);
        assert_eq!(keys(&actor), vec![(0, 400), (0, 401), (1, 300), (1, 400)]);
        assert_eq!(actor.sessions["peer"], peer);
        assert!(actor.sessions["s"].pending.is_none());
        actor
            .handle_record(&data_message(0, &[(401, vec![(5, vec![50])])]))
            .unwrap();
        assert_eq!(keys(&actor), vec![(0, 400), (0, 401), (1, 300), (1, 400)]);
        assert!(actor.sessions["s"].pending.is_none());
    }

    #[test]
    fn snapshots_coexist_across_additions_removals_and_domains() {
        for (old, new) in [
            (vec![(0, 300, 1)], vec![(0, 400, 2)]),
            (vec![(0, 300, 1)], vec![(1, 400, 1)]),
            (vec![(0, 300, 1), (0, 301, 1)], vec![(0, 400, 1)]),
            (vec![(0, 300, 1)], vec![(0, 400, 1), (0, 401, 1)]),
            (
                vec![(0, 300, 1), (0, 301, 1)],
                vec![(0, 301, 1), (0, 400, 1)],
            ),
        ] {
            let mut actor = actor();
            actor.handle_template(snapshot("s", &old)).unwrap();
            actor.handle_template(snapshot("s", &new)).unwrap();
            let active = actor.sessions["s"].active.clone();
            assert_eq!(
                active,
                IpfixActor::compile_generation(&snapshot("s", &old)).unwrap()
            );
            let mut expected = new
                .iter()
                .map(|(domain, id, _)| (*domain, *id))
                .collect::<Vec<_>>();
            expected.sort_unstable();
            let mut coexist = expected.clone();
            coexist.extend(old.iter().map(|(domain, id, _)| (*domain, *id)));
            coexist.sort_unstable();
            coexist.dedup();
            assert_eq!(keys(&actor), coexist);
            for (domain, id, _) in &old {
                assert_eq!(
                    actor
                        .handle_record(&data_message(*domain, &[(*id, vec![(1, vec![1])])]))
                        .unwrap()
                        .record_count(),
                    1
                );
                assert_eq!(actor.sessions["s"].active, active);
                assert!(actor.sessions["s"].pending.is_some());
            }
            let (domain, id, _) = new
                .iter()
                .find(|(domain, id, _)| {
                    !old.iter()
                        .any(|(old_domain, old_id, _)| (old_domain, old_id) == (domain, id))
                })
                .unwrap();
            actor
                .handle_record(&data_message(*domain, &[(*id, vec![(2, vec![2])])]))
                .unwrap();
            assert_eq!(keys(&actor), expected);
            assert!(actor.sessions["s"].pending.is_none());
        }
    }

    #[test]
    fn counter_additions_removals_and_reordering_do_not_prevent_handover() {
        let original = template_message("s", 0, 300, &[(1, 0x0001_0001), (2, 0x0001_0002)]);
        for fields in [
            vec![(1, 0x0001_0001)],
            vec![(1, 0x0001_0001), (2, 0x0001_0002), (3, 0x0001_0003)],
            vec![(2, 0x0001_0002), (1, 0x0001_0001)],
            vec![(3, 0x0001_0001), (2, 0x0001_0002)],
            vec![(1, 0x0002_0001), (2, 0x0001_0002)],
            vec![(1, 0x0001_0003), (2, 0x0001_0002)],
        ] {
            let mut actor = actor();
            actor.handle_template(original.clone()).unwrap();
            actor
                .handle_template(template_message("s", 0, 400, &fields))
                .unwrap();
            assert_eq!(keys(&actor), vec![(0, 300), (0, 400)]);
            assert_eq!(
                actor
                    .handle_record(&data_message(0, &[(300, vec![(1, vec![10, 20])])]))
                    .unwrap()
                    .counter_count(),
                2
            );
            let batch = actor
                .handle_record(&data_message(
                    0,
                    &[(400, vec![(2, vec![30; fields.len()])])],
                ))
                .unwrap();
            let record = batch.iter().next().unwrap();
            assert_eq!(record.stats.len(), fields.len());
            for (counter, (object, enterprise)) in record.stats.iter().zip(&fields) {
                assert_eq!(counter.object_name.as_ref(), format!("Ethernet{object}"));
                assert_eq!(
                    (counter.type_id, counter.stat_id),
                    decode_sai_ids(*enterprise)
                );
                assert_eq!(counter.counter, 30);
            }
            assert_eq!(keys(&actor), vec![(0, 400)]);
            assert!(actor.sessions["s"].pending.is_none());
        }
    }

    #[test]
    fn width_change_with_new_id_can_cut_over() {
        let mut actor = actor();
        actor
            .handle_template(template_message("s", 0, 300, &[(1, 1)]))
            .unwrap();
        let mut update = template_message("s", 0, 400, &[(1, 1)]);
        Arc::make_mut(update.templates.as_mut().unwrap())[30..32]
            .copy_from_slice(&1u16.to_be_bytes());
        actor.handle_template(update).unwrap();
        let mut data = data_message(0, &[(400, vec![(9, vec![0])])]);
        data.truncate(29);
        data[2..4].copy_from_slice(&29u16.to_be_bytes());
        data[18..20].copy_from_slice(&13u16.to_be_bytes());
        data[28] = 255;
        let batch = actor.handle_record(&data).unwrap();
        assert_eq!(batch.iter().next().unwrap().stats.get(0).unwrap().counter, 255);
        assert_eq!(keys(&actor), vec![(0, 400)]);
    }

    #[test]
    fn malformed_whole_input_never_emits_or_promotes() {
        let mut actor = actor();
        actor
            .handle_template(snapshot("s", &[(0, 300, 1)]))
            .unwrap();
        actor
            .handle_template(snapshot("s", &[(0, 400, 1)]))
            .unwrap();
        let valid = data_message(0, &[(400, vec![(1, vec![1])])]);
        let mut bad_trailer = valid.clone();
        bad_trailer.extend_from_slice(&[0, 10, 0]);
        let bad_later_set = data_message(0, &[(400, vec![(1, vec![1])]), (300, vec![(2, vec![])])]);
        let mut bad_later_message = valid;
        bad_later_message.extend_from_slice(&data_message(0, &[(300, vec![(2, vec![])])]));
        let padding_only = data_message(0, &[(400, vec![(0, vec![])])]);
        for input in [bad_trailer, bad_later_set, bad_later_message, padding_only] {
            assert!(actor.handle_record(&input).is_err());
            assert_eq!(actor.last_observation_time, None);
            assert_eq!(keys(&actor), vec![(0, 300), (0, 400)]);
            assert!(actor.sessions["s"].pending.is_some());
        }
    }

    #[tokio::test(start_paused = true)]
    async fn invalid_input_warning_budget_reports_exact_suppressed_count() {
        let mut actor = actor();
        assert_eq!(actor.record_invalid_input_warning(), Some(0));
        let deadline = actor.next_invalid_warning;
        for _ in 0..1_000 {
            assert_eq!(actor.record_invalid_input_warning(), None);
        }
        assert_eq!(actor.suppressed_invalid_warnings, 1_000);
        assert_eq!(actor.next_invalid_warning, deadline);
        tokio::time::advance(DROP_WARNING_INTERVAL - Duration::from_millis(1)).await;
        assert_eq!(actor.record_invalid_input_warning(), None);
        tokio::time::advance(Duration::from_millis(1)).await;
        assert_eq!(actor.record_invalid_input_warning(), Some(1_001));
        assert_eq!(actor.suppressed_invalid_warnings, 0);
        assert_eq!(actor.record_invalid_input_warning(), None);
        tokio::time::advance(3 * DROP_WARNING_INTERVAL).await;
        assert_eq!(actor.record_invalid_input_warning(), Some(1));
        assert_eq!(actor.suppressed_invalid_warnings, 0);
    }

    #[tokio::test(start_paused = true)]
    async fn invalid_input_flood_does_not_change_decoding_or_other_warning_budget() {
        let mut actor = actor();
        actor
            .handle_template(snapshot("s", &[(0, 300, 1)]))
            .unwrap();
        actor
            .handle_template(snapshot("s", &[(0, 301, 2)]))
            .unwrap();
        let mut batch = SAIStatsBatch::default();
        let mut invalid_version = data_message(0, &[(300, vec![(1, vec![1])])]);
        invalid_version[0..2].copy_from_slice(&9u16.to_be_bytes());
        let mut invalid_tail = data_message(0, &[(301, vec![(2, vec![2])])]);
        invalid_tail.extend_from_slice(&[1, 2, 3]);
        let layout_error = data_message(0, &[(300, vec![(3, vec![])])]);
        let bad_inputs = [vec![], invalid_version, invalid_tail, layout_error];
        let unknown_deadline = actor.next_drop_warning;
        for input in bad_inputs.iter().cycle().take(1_000) {
            actor.process_record_input(input, &mut batch).await;
        }
        assert!(batch.is_empty());
        assert_eq!(actor.suppressed_invalid_warnings, 999);
        assert_eq!(actor.next_drop_warning, unknown_deadline);
        assert_eq!(actor.dropped_sets, 0);
        assert_eq!(actor.last_observation_time, None);
        assert!(actor.sessions["s"].pending.is_some());
        let invalid_deadline = actor.next_invalid_warning;

        // Unknown-template warnings remain independent; valid data and template
        // cutover are not delayed by exhausted invalid-input logging budget.
        let good = data_message(0, &[(400, vec![(4, vec![4])]), (301, vec![(5, vec![50])])]);
        actor.process_record_input(&good, &mut batch).await;
        assert_eq!(batch.record_count(), 1);
        assert_eq!(batch.iter().next().unwrap().observation_time, 5);
        assert_eq!(batch.iter().next().unwrap().stats.get(0).unwrap().counter, 50);
        assert!(actor.sessions["s"].pending.is_none());
        assert_eq!(actor.dropped_sets, 1);
        assert!(actor.next_drop_warning > unknown_deadline);
        assert_eq!(actor.next_invalid_warning, invalid_deadline);
        assert_eq!(actor.suppressed_invalid_warnings, 999);

        tokio::time::advance(DROP_WARNING_INTERVAL).await;
        actor.process_record_input(&[], &mut batch).await;
        assert_eq!(actor.suppressed_invalid_warnings, 0);
        assert!(actor.next_invalid_warning > invalid_deadline);
        assert_eq!(batch.record_count(), 1);
    }

    #[tokio::test(start_paused = true)]
    async fn invalid_warning_state_is_actor_local_and_saturating() {
        let mut first = actor();
        let mut second = actor();
        assert_eq!(first.record_invalid_input_warning(), Some(0));
        first.suppressed_invalid_warnings = u64::MAX;
        assert_eq!(first.record_invalid_input_warning(), None);
        assert_eq!(first.suppressed_invalid_warnings, u64::MAX);
        assert_eq!(second.record_invalid_input_warning(), Some(0));
        tokio::time::advance(DROP_WARNING_INTERVAL).await;
        assert_eq!(first.record_invalid_input_warning(), Some(u64::MAX));
        assert_eq!(first.suppressed_invalid_warnings, 0);
    }

    #[tokio::test(start_paused = true)]
    async fn unknown_sets_drop_without_blocking_and_warnings_are_rate_limited() {
        let mut actor = actor();
        actor
            .handle_template(snapshot("known", &[(0, 300, 1)]))
            .unwrap();
        let input = data_message(0, &[(400, vec![(1, vec![10])]), (300, vec![(2, vec![20])])]);
        let batch = actor.handle_record(&input).unwrap();
        assert_eq!(batch.record_count(), 1);
        assert_eq!(batch.iter().next().unwrap().observation_time, 2);
        assert_eq!(actor.dropped_sets, 1);
        let warning = actor.next_drop_warning;
        actor.handle_record(&input).unwrap();
        assert_eq!(actor.dropped_sets, 2);
        assert_eq!(actor.next_drop_warning, warning);
        tokio::time::advance(DROP_WARNING_INTERVAL).await;
        actor.handle_record(&input).unwrap();
        assert_eq!(actor.dropped_sets, 3);
        assert!(actor.next_drop_warning > warning);
        actor
            .handle_template(snapshot("late", &[(0, 400, 2)]))
            .unwrap();
        let batch = actor
            .handle_record(&data_message(0, &[(400, vec![(3, vec![30])])]))
            .unwrap();
        assert_eq!(batch.record_count(), 1);
        assert_eq!(batch.iter().next().unwrap().observation_time, 3);
    }

    #[test]
    fn cancellation_supersession_and_identical_refresh_are_bounded() {
        let mut actor = actor();
        let active = snapshot("s", &[(0, 300, 1), (0, 301, 2)]);
        actor.handle_template(active.clone()).unwrap();
        let original = actor.sessions["s"].clone();
        actor
            .handle_template(snapshot("s", &[(0, 301, 2), (0, 300, 1)]))
            .unwrap();
        assert_eq!(actor.sessions["s"], original);
        actor
            .handle_template(snapshot("s", &[(0, 400, 1), (0, 401, 2)]))
            .unwrap();
        let before = keys(&actor);
        let pending = actor.sessions["s"].clone();
        actor
            .handle_template(snapshot("s", &[(0, 400, 1), (0, 401, 2)]))
            .unwrap();
        assert_eq!(keys(&actor), before);
        assert_eq!(actor.sessions["s"], pending);
        actor
            .handle_template(snapshot("s", &[(0, 400, 1), (0, 402, 2)]))
            .unwrap();
        assert_eq!(keys(&actor), vec![(0, 300), (0, 301), (0, 400), (0, 402)]);
        actor.handle_template(active).unwrap();
        assert_eq!(keys(&actor), vec![(0, 300), (0, 301)]);
        assert_eq!(
            actor
                .handle_record(&data_message(0, &[(400, vec![(2, vec![2])])]))
                .unwrap()
                .record_count(),
            0
        );
        actor
            .handle_record(&data_message(0, &[(300, vec![(3, vec![3])])]))
            .unwrap();
        assert_eq!(keys(&actor), vec![(0, 300), (0, 301)]);
        assert!(actor.sessions["s"].pending.is_none());
    }

    #[test]
    fn promoted_snapshot_remains_active_during_subsequent_handover() {
        let mut actor = actor();
        actor
            .handle_template(snapshot("s", &[(0, 300, 1), (0, 301, 2)]))
            .unwrap();
        actor
            .handle_template(snapshot("s", &[(0, 400, 1), (0, 401, 2)]))
            .unwrap();
        actor
            .handle_record(&data_message(0, &[(400, vec![(1, vec![1])])]))
            .unwrap();
        actor
            .handle_template(snapshot("s", &[(0, 500, 1), (0, 401, 2)]))
            .unwrap();
        let batch = actor
            .handle_record(&data_message(
                0,
                &[(400, vec![(2, vec![2])]), (301, vec![(3, vec![3])])],
            ))
            .unwrap();
        assert_eq!(batch.record_count(), 1);
        assert_eq!(keys(&actor), vec![(0, 400), (0, 401), (0, 500)]);
        actor
            .handle_record(&data_message(0, &[(500, vec![(4, vec![4])])]))
            .unwrap();
        assert_eq!(keys(&actor), vec![(0, 401), (0, 500)]);
        actor
            .handle_record(&data_message(0, &[(401, vec![(5, vec![5])])]))
            .unwrap();
        assert_eq!(keys(&actor), vec![(0, 401), (0, 500)]);
    }

    #[test]
    fn shared_keys_do_not_promote_but_additions_promote_the_whole_snapshot() {
        let mut actor = actor();
        actor
            .handle_template(snapshot("s", &[(0, 300, 1), (0, 301, 2)]))
            .unwrap();
        actor
            .handle_template(snapshot("s", &[(0, 300, 1), (0, 401, 2), (0, 402, 3)]))
            .unwrap();
        let c = TemplateKey {
            observation_domain_id: 0,
            template_id: 402,
        };
        let shared = TemplateKey {
            observation_domain_id: 0,
            template_id: 300,
        };
        assert!(Arc::ptr_eq(
            &actor.sessions["s"].active.templates[&shared],
            &actor.sessions["s"].pending.as_ref().unwrap().templates[&shared]
        ));
        actor
            .handle_record(&data_message(0, &[(300, vec![(1, vec![1])])]))
            .unwrap();
        assert!(actor.sessions["s"].pending.is_some());
        assert_eq!(keys(&actor), vec![(0, 300), (0, 301), (0, 401), (0, 402)]);
        let mut malformed = data_message(0, &[(402, vec![(1, vec![1])])]);
        malformed.extend_from_slice(&[1]);
        assert!(actor.handle_record(&malformed).is_err());
        assert!(!actor.sessions["s"].active.templates.contains_key(&c));
        actor
            .handle_record(&data_message(0, &[(402, vec![(2, vec![2])])]))
            .unwrap();
        assert!(actor.sessions["s"].active.templates.contains_key(&c));
        assert_eq!(keys(&actor), vec![(0, 300), (0, 401), (0, 402)]);
        assert!(actor.sessions["s"].pending.is_none());

        let next = snapshot("s", &[(0, 300, 1), (0, 401, 2), (0, 502, 3)]);
        actor.handle_template(next.clone()).unwrap();
        actor.handle_template(next).unwrap();
        assert_eq!(keys(&actor), vec![(0, 300), (0, 401), (0, 402), (0, 502)]);
        let batch = actor
            .handle_record(&data_message(
                0,
                &[
                    (300, vec![(3, vec![3])]),
                    (301, vec![(4, vec![4])]),
                    (402, vec![(5, vec![5])]),
                ],
            ))
            .unwrap();
        assert_eq!(batch.record_count(), 2);
        assert!(actor.sessions["s"].pending.is_some());
        actor
            .handle_record(&data_message(0, &[(502, vec![(6, vec![6])])]))
            .unwrap();
        assert_eq!(keys(&actor), vec![(0, 300), (0, 401), (0, 502)]);
        assert!(actor.sessions["s"].pending.is_none());
        actor
            .handle_record(&data_message(0, &[(401, vec![(7, vec![7])])]))
            .unwrap();
        assert_eq!(keys(&actor), vec![(0, 300), (0, 401), (0, 502)]);
        assert!(actor.sessions["s"].pending.is_none());
    }

    #[test]
    fn removal_only_snapshot_waits_for_a_new_key_or_cancellation() {
        let mut actor = actor();
        actor
            .handle_template(snapshot("s", &[(0, 300, 1), (0, 301, 2)]))
            .unwrap();
        actor
            .handle_template(snapshot("s", &[(0, 300, 1)]))
            .unwrap();
        actor
            .handle_record(&data_message(0, &[(300, vec![(1, vec![1])])]))
            .unwrap();
        assert_eq!(actor.sessions["s"].active.templates.len(), 2);
        assert!(actor.sessions["s"].pending.is_some());
        assert_eq!(keys(&actor), vec![(0, 300), (0, 301)]);
        actor
            .handle_template(snapshot("s", &[(0, 300, 1), (0, 500, 4)]))
            .unwrap();
        assert_eq!(keys(&actor), vec![(0, 300), (0, 301), (0, 500)]);
        assert_eq!(actor.sessions["s"].active.templates.len(), 2);
        actor
            .handle_record(&data_message(0, &[(500, vec![(2, vec![2])])]))
            .unwrap();
        assert_eq!(actor.sessions["s"].active.templates.len(), 2);
        assert_eq!(keys(&actor), vec![(0, 300), (0, 500)]);
        assert!(actor.sessions["s"].pending.is_none());
    }

    #[test]
    fn repeated_snapshots_retain_only_active_and_latest_pending() {
        let mut actor = actor();
        actor
            .handle_template(snapshot("s", &[(0, 300, 1)]))
            .unwrap();
        let mut active_id = 300;
        for id in 400..1500 {
            actor
                .handle_template(snapshot("s", &[(0, id, u32::from(id))]))
                .unwrap();
            assert_eq!(keys(&actor), vec![(0, active_id), (0, id)]);
            assert_eq!(actor.sessions["s"].active.templates.len(), 1);
            if id % 2 == 0 {
                actor
                    .handle_record(&data_message(0, &[(id, vec![(1, vec![1])])]))
                    .unwrap();
                assert_eq!(actor.sessions["s"].active.templates.len(), 1);
                assert!(actor.sessions["s"].pending.is_none());
                active_id = id;
            }
        }
        actor
            .handle_template(snapshot("s", &[(0, 1500, 1)]))
            .unwrap();
        actor
            .handle_record(&data_message(0, &[(1500, vec![(1, vec![1])])]))
            .unwrap();
        assert_eq!(keys(&actor), vec![(0, 1500)]);
        assert!(actor.sessions["s"].pending.is_none());
    }

    #[test]
    fn malformed_updates_remove_only_incoming_owner_and_valid_updates_recover() {
        for malformed_suffix in [false, true] {
            let mut actor = actor();
            actor
                .handle_template(snapshot("a", &[(0, 300, 1)]))
                .unwrap();
            actor
                .handle_template(snapshot("b", &[(0, 301, 2)]))
                .unwrap();
            actor
                .handle_template(snapshot("a", &[(0, 400, 3)]))
                .unwrap();
            let mut bad = snapshot("a", &[(0, 500, 4)]);
            if malformed_suffix {
                Arc::make_mut(bad.templates.as_mut().unwrap()).extend_from_slice(&[0, 10, 0]);
            } else {
                bad.templates = Some(Arc::new(vec![0, 10, 0, 0]));
            }
            assert!(actor.handle_template(bad).is_err());
            assert_eq!(keys(&actor), vec![(0, 301)]);
            assert!(!actor.sessions.contains_key("a"));
            actor
                .handle_template(snapshot("a", &[(0, 300, 3)]))
                .unwrap();
            assert_eq!(keys(&actor), vec![(0, 300), (0, 301)]);
            let batch = actor
                .handle_record(&data_message(0, &[(301, vec![(1, vec![1])])]))
                .unwrap();
            assert_eq!(batch.iter().next().unwrap().stats.get(0).unwrap().stat_id, 2);
        }
    }

    #[test]
    fn collisions_preserve_all_owners_active_pending_and_installed_state() {
        let mut actor = actor();
        for (owner, active, pending) in [("a", 300, 400), ("b", 301, 401)] {
            actor
                .handle_template(snapshot(owner, &[(7, active, 1)]))
                .unwrap();
            actor
                .handle_template(snapshot(owner, &[(7, pending, 2)]))
                .unwrap();
        }
        let sessions = actor.sessions.clone();
        let installed = actor.installed.clone();
        for (owner, id, stat, incumbent) in [
            ("a", 300, 3, "a"),
            ("a", 400, 3, "a"),
            ("a", 301, 1, "b"),
            ("a", 401, 3, "b"),
            ("new", 301, 1, "b"),
        ] {
            let error = actor
                .handle_template(snapshot(owner, &[(7, 500, 4), (7, id, stat)]))
                .unwrap_err()
                .to_string();
            assert!(error.contains("collision"));
            assert!(error.contains(&format!("incoming owner {owner:?}")));
            assert!(error.contains(&format!("existing owner {incumbent:?}")));
            assert!(error.contains(&format!("template_id: {id}")));
            assert!(error.contains("observation_domain_id: 7"));
            assert_eq!(actor.sessions, sessions);
            assert_eq!(actor.installed, installed);
            for (key, template) in &installed {
                assert!(Arc::ptr_eq(&actor.installed[key], template));
            }
        }
        let batch = actor
            .handle_record(&data_message(
                7,
                &[(300, vec![(1, vec![10])]), (301, vec![(2, vec![20])])],
            ))
            .unwrap();
        assert_eq!(batch.record_count(), 2);
        assert_eq!(actor.sessions, sessions);
    }

    #[test]
    fn retired_and_superseded_keys_allow_different_owner_and_schema_reuse() {
        let mut actor = actor();
        actor
            .handle_template(snapshot("a", &[(0, 300, 1)]))
            .unwrap();
        actor
            .handle_template(snapshot("a", &[(0, 400, 2)]))
            .unwrap();
        actor
            .handle_template(snapshot("a", &[(0, 500, 3)]))
            .unwrap();
        actor
            .handle_template(snapshot("b", &[(0, 400, 4)]))
            .unwrap();
        actor
            .handle_record(&data_message(0, &[(500, vec![(1, vec![1])])]))
            .unwrap();
        actor
            .handle_template(snapshot("c", &[(0, 300, 5)]))
            .unwrap();
        let batch = actor
            .handle_record(&data_message(
                0,
                &[(300, vec![(2, vec![2])]), (400, vec![(3, vec![3])])],
            ))
            .unwrap();
        assert_eq!(
            batch.iter().map(|r| r.stats.get(0).unwrap().stat_id).collect::<Vec<_>>(),
            vec![5, 4]
        );
        assert_eq!(keys(&actor), vec![(0, 300), (0, 400), (0, 500)]);
    }

    #[test]
    fn delete_and_deactivate_remove_only_owner_and_allow_same_id_late_reuse() {
        for delete in [false, true] {
            let mut actor = actor();
            actor
                .handle_template(snapshot("a", &[(0, 300, 1)]))
                .unwrap();
            actor
                .handle_template(snapshot("a", &[(0, 400, 1)]))
                .unwrap();
            actor
                .handle_template(snapshot("b", &[(0, 301, 2)]))
                .unwrap();
            let update = if delete {
                IPFixTemplatesMessage::delete("a".into())
            } else {
                IPFixTemplatesMessage::deactivate("a".into())
            };
            actor.handle_template(update).unwrap();
            assert_eq!(keys(&actor), vec![(0, 301)]);
            assert!(actor
                .handle_record(&data_message(0, &[(300, vec![(1, vec![1])])]))
                .unwrap()
                .is_empty());
            actor
                .handle_template(snapshot("a", &[(0, 300, 3)]))
                .unwrap();
            let batch = actor
                .handle_record(&data_message(0, &[(300, vec![(2, vec![2])])]))
                .unwrap();
        assert_eq!(batch.iter().next().unwrap().stats.get(0).unwrap().stat_id, 3);
        }
    }

    #[test]
    fn same_id_is_scoped_by_domain_and_unknown_deletes_leave_no_state() {
        let mut actor = actor();
        actor
            .handle_template(snapshot("a", &[(0, 300, 1)]))
            .unwrap();
        actor
            .handle_template(snapshot("b", &[(1, 300, 2)]))
            .unwrap();
        actor
            .handle_template(IPFixTemplatesMessage::delete("unknown".into()))
            .unwrap();
        assert_eq!(actor.sessions.len(), 2);
        let mut input = data_message(0, &[(300, vec![(1, vec![1])])]);
        input.extend_from_slice(&data_message(1, &[(300, vec![(2, vec![2])])]));
        assert_eq!(
            actor
                .handle_record(&input)
                .unwrap()
                .iter()
                .map(|r| r.stats.get(0).unwrap().stat_id)
                .collect::<Vec<_>>(),
            vec![1, 2]
        );
    }

    #[test]
    fn widths_one_through_eight_and_timestamp_offsets_decode_exactly() {
        for width in 1u16..=8 {
            for timestamp_first in [false, true] {
                let mut update = template_message("s", 0, 300, &[(1, 0x9234_8567)]);
                let bytes = Arc::make_mut(update.templates.as_mut().unwrap());
                bytes[30..32].copy_from_slice(&width.to_be_bytes());
                if !timestamp_first {
                    bytes[24..36].rotate_left(4);
                }
                let mut actor = actor();
                actor.handle_template(update).unwrap();
                for high_only in [false, true] {
                    let mut payload = vec![0xff; width as usize];
                    if high_only {
                        payload.fill(0);
                        payload[0] = 0x80;
                    }
                    let expected = if high_only {
                        1u64 << (width * 8 - 1)
                    } else {
                        u64::MAX >> (64 - width * 8)
                    };
                    if timestamp_first {
                        payload.splice(0..0, 42u64.to_be_bytes());
                    } else {
                        payload.extend_from_slice(&42u64.to_be_bytes());
                    }
                    let mut data = data_message(0, &[(300, vec![(0, vec![0])])]);
                    data.truncate(20);
                    data.extend_from_slice(&payload);
                    let len = data.len() as u16;
                    data[2..4].copy_from_slice(&len.to_be_bytes());
                    data[18..20].copy_from_slice(&(len - 16).to_be_bytes());
                    let batch = actor.handle_record(&data).unwrap();
                    let record = batch.iter().next().unwrap();
                    assert_eq!(record.observation_time, 42);
                    assert_eq!(record.stats.get(0).unwrap().counter, expected);
                    assert_eq!(
                        (record.stats.get(0).unwrap().type_id, record.stats.get(0).unwrap().stat_id),
                        decode_sai_ids(0x9234_8567)
                    );
                }
            }
        }
    }

    #[test]
    fn mixed_width_counters_keep_template_order() {
        let mut update = template_message("s", 0, 300, &[(2, 0x1234_0567), (1, 0x0001_0002)]);
        let bytes = Arc::make_mut(update.templates.as_mut().unwrap());
        let original = bytes.clone();
        bytes[24..32].copy_from_slice(&original[28..36]);
        bytes[26..28].copy_from_slice(&3u16.to_be_bytes());
        bytes[32..40].copy_from_slice(&original[36..44]);
        bytes[34..36].copy_from_slice(&6u16.to_be_bytes());
        bytes[40..44].copy_from_slice(&original[24..28]);
        let generation = IpfixActor::compile_generation(&update).unwrap();
        let template = generation.templates.values().next().unwrap();
        assert_eq!(template.counters[0].offset, 0);
        assert_eq!(template.counters[1].offset, 3);
        assert_eq!(
            template.observation_time,
            ObservationTime::RawNanoseconds { offset: 9 }
        );
        assert_eq!(template.record_len, 17);
        assert_eq!(template.counters[0].object_name.as_ref(), "Ethernet2");
        assert_eq!(
            (template.counters[0].type_id, template.counters[0].stat_id),
            (0x1234, 0x567)
        );
    }

    #[test]
    fn framing_padding_and_invalid_widths_are_checked() {
        for len in 0u16..IPFIX_HEADER_LEN as u16 {
            let mut input = [0; IPFIX_HEADER_LEN];
            input[0..2].copy_from_slice(&IPFIX_VERSION.to_be_bytes());
            input[2..4].copy_from_slice(&len.to_be_bytes());
            assert!(IpfixMessages::new(&input).next().unwrap().is_err());
        }
        for width in [0u16, 9, u16::MAX] {
            let mut update = template_message("s", 0, 300, &[(1, 1)]);
            Arc::make_mut(update.templates.as_mut().unwrap())[30..32]
                .copy_from_slice(&width.to_be_bytes());
            assert!(IpfixActor::compile_generation(&update).is_err());
        }
        let mut actor = actor();
        let mut update = template_message("s", 0, 300, &[(1, 1)]);
        let bytes = Arc::make_mut(update.templates.as_mut().unwrap());
        bytes.extend_from_slice(&[0; 4]);
        bytes[2..4].copy_from_slice(&40u16.to_be_bytes());
        bytes[18..20].copy_from_slice(&24u16.to_be_bytes());
        actor.handle_template(update).unwrap();
        let mut data = data_message(0, &[(300, vec![(42, vec![100])])]);
        data.extend_from_slice(&[0; 7]);
        data[2..4].copy_from_slice(&43u16.to_be_bytes());
        data[18..20].copy_from_slice(&27u16.to_be_bytes());
        assert_eq!(actor.handle_record(&data).unwrap().record_count(), 1);
        data[42] = 1;
        assert!(actor.handle_record(&data).is_err());
        data[18..20].copy_from_slice(&4u16.to_be_bytes());
        assert!(actor.handle_record(&data).is_err());
    }

    #[test]
    fn template_and_data_input_limits_are_atomic() {
        let mut update = snapshot("s", &[(0, 300, 1)]);
        update.templates = Some(Arc::new(vec![0; MAX_TEMPLATE_CONFIG_BYTES + 1]));
        assert!(IpfixActor::compile_generation(&update).is_err());
        update = snapshot("s", &[(0, 300, 1)]);
        update.object_names = Some(vec!["x".repeat(MAX_OBJECT_METADATA_BYTES + 1)]);
        assert!(IpfixActor::compile_generation(&update).is_err());
        update.object_names = Some(vec!["x".into(); MAX_OBJECTS_PER_UPDATE + 1]);
        assert!(IpfixActor::compile_generation(&update).is_err());
        let mut actor = actor();
        actor
            .handle_template(snapshot("s", &[(0, 300, 1)]))
            .unwrap();
        actor
            .handle_template(snapshot("s", &[(0, 400, 1)]))
            .unwrap();
        let mut input = data_message(0, &[(400, vec![(1, vec![1])])]);
        for _ in 0..MAX_DATA_SETS_PER_RECORD_INPUT {
            input.extend_from_slice(&data_message(0, &[(500, vec![(0, vec![])])]));
        }
        assert!(actor.handle_record(&input).is_err());
        assert_eq!(keys(&actor), vec![(0, 300), (0, 400)]);
        assert_eq!(actor.dropped_sets, 0);
    }

    #[test]
    fn template_counts_are_not_subject_to_artificial_quotas() {
        let mut actor = actor();
        for domain in 0..2 {
            let templates = (300..3300).map(|id| (domain, id, 1)).collect::<Vec<_>>();
            actor
                .handle_template(snapshot(&format!("s{domain}"), &templates))
                .unwrap();
        }
        assert_eq!(actor.installed.len(), 6000);
        let replacement = (3300..6300).map(|id| (0, id, 2)).collect::<Vec<_>>();
        actor.handle_template(snapshot("s0", &replacement)).unwrap();
        assert_eq!(actor.installed.len(), 9000);
        actor
            .handle_record(&data_message(0, &[(3300, vec![(1, vec![1])])]))
            .unwrap();
        assert_eq!(actor.installed.len(), 6000);
        assert!(actor.sessions["s0"].pending.is_none());
        assert_eq!(actor.sessions["s1"].active.templates.len(), 3000);
    }

    #[test]
    fn aggregate_template_storage_has_no_artificial_byte_quota() {
        let mut actor = actor();
        for domain in 0..65 {
            let mut update = snapshot(&format!("s{domain}"), &[(domain, 300, 1)]);
            update.object_names = Some(vec!["x".repeat(1024 * 1024)]);
            actor.handle_template(update).unwrap();
        }
        assert_eq!(actor.sessions.len(), 65);
        assert_eq!(actor.installed.len(), 65);
        assert_eq!(
            actor
                .handle_record(&data_message(64, &[(300, vec![(1, vec![1])])]))
                .unwrap()
                .record_count(),
            1
        );
    }

    #[test]
    fn structured_truncations_and_arbitrary_inputs_never_panic() {
        let update = template_message("s", 0, 300, &[(1, 1)]);
        let bytes = update.templates.as_ref().unwrap();
        for len in 0..bytes.len() {
            let mut mutated = update.clone();
            let mut bytes = bytes[..len].to_vec();
            if len >= 20 {
                bytes[2..4].copy_from_slice(&(len as u16).to_be_bytes());
                bytes[18..20].copy_from_slice(&((len - 16) as u16).to_be_bytes());
            }
            mutated.templates = Some(Arc::new(bytes));
            assert!(std::panic::catch_unwind(|| IpfixActor::compile_generation(&mutated)).is_ok());
        }
        let mut actor = actor();
        actor.handle_template(update.clone()).unwrap();
        let mut seed = 7u64;
        for len in 0..256 {
            let bytes = (0..len)
                .map(|_| {
                    seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1);
                    (seed >> 32) as u8
                })
                .collect::<Vec<_>>();
            let mut mutated = update.clone();
            mutated.templates = Some(Arc::new(bytes.clone()));
            assert!(std::panic::catch_unwind(|| IpfixActor::compile_generation(&mutated)).is_ok());
            assert!(std::panic::catch_unwind(std::panic::AssertUnwindSafe(
                || actor.handle_record(&bytes)
            ))
            .is_ok());
        }
    }

    #[tokio::test]
    async fn batches_preserve_whole_records_including_oversized_records_and_fanout() {
        let mut actor = actor();
        let (first_tx, mut first_rx) = channel(8);
        let (second_tx, mut second_rx) = channel(8);
        actor.add_recipient(first_tx);
        actor.add_recipient(second_tx);
        let mut batch = SAIStatsBatch::default();
        for (time, count) in [
            (1, 5000),
            (2, 5000),
            (3, TARGET_COUNTERS_PER_BATCH + 1),
            (4, 3),
        ] {
            batch.push_record(time, (0..count).map(|i| SAIStat::new("x", 1, 2, i as u64)));
        }
        actor.send_batch(batch).await;
        for (time, count) in [
            (1, 5000),
            (2, 5000),
            (3, TARGET_COUNTERS_PER_BATCH + 1),
            (4, 3),
        ] {
            let first = first_rx.recv().await.unwrap();
            let second = second_rx.recv().await.unwrap();
            assert!(Arc::ptr_eq(&first, &second));
            assert_eq!(first.record_count(), 1);
            assert_eq!(first.counter_count(), count);
            assert_eq!(first.iter().next().unwrap().observation_time, time);
        }
    }

    #[tokio::test]
    async fn process_input_groups_small_messages_and_flushes_at_soft_target() {
        let mut actor = actor();
        let fields = (1..=4000).map(|id| (id, 1)).collect::<Vec<_>>();
        actor
            .handle_template(template_message("s", 0, 300, &fields))
            .unwrap();
        let (tx, mut rx) = channel(4);
        actor.add_recipient(tx);
        let mut input = Vec::new();
        for time in 1..=3 {
            input.extend_from_slice(&data_message(0, &[(300, vec![(time, vec![time; 4000])])]));
        }
        let mut batch = SAIStatsBatch::default();
        actor.process_record_input(&input, &mut batch).await;
        actor.send_batch(batch).await;
        let first = rx.recv().await.unwrap();
        let second = rx.recv().await.unwrap();
        assert_eq!(
            (first.counter_count(), second.counter_count()),
            (8000, 4000)
        );
        assert_eq!(
            first.iter().map(|r| r.observation_time).collect::<Vec<_>>(),
            vec![1, 2]
        );
        assert_eq!(second.iter().next().unwrap().observation_time, 3);
    }

    #[tokio::test]
    async fn malformed_later_message_never_sends_valid_prefix() {
        let mut actor = actor();
        actor
            .handle_template(snapshot("s", &[(0, 300, 1)]))
            .unwrap();
        actor
            .handle_template(snapshot("s", &[(0, 400, 1)]))
            .unwrap();
        let (tx, mut rx) = channel(1);
        actor.add_recipient(tx);
        let mut input = data_message(0, &[(400, vec![(1, vec![1])])]);
        input.extend_from_slice(&[1, 2, 3]);
        let mut batch = SAIStatsBatch::default();
        actor.process_record_input(&input, &mut batch).await;
        assert!(batch.is_empty());
        assert_eq!(actor.last_observation_time, None);
        assert!(rx.try_recv().is_err());
        assert_eq!(keys(&actor), vec![(0, 300), (0, 400)]);
    }

    #[tokio::test]
    async fn closed_recipient_does_not_stop_healthy_delivery_or_later_inputs() {
        for queued in [false, true] {
            let (_template_tx, template_rx) = channel(1);
            let (record_tx, record_rx) = channel(2);
            let (healthy_tx, mut healthy_rx) = channel(2);
            let (closed_tx, closed_rx) = channel(1);
            drop(closed_rx);
            let mut actor = IpfixActor::new(template_rx, record_rx);
            let fields = (1..=4000).map(|id| (id, 1)).collect::<Vec<_>>();
            actor
                .handle_template(template_message("s", 0, 300, &fields))
                .unwrap();
            actor.add_recipient(closed_tx);
            actor.add_recipient(healthy_tx);
            if queued {
                record_tx.send(Arc::new(vec![1, 2, 3])).await.unwrap();
            }
            let mut input = data_message(0, &[(300, vec![(1, vec![10; 4000])])]);
            input.extend_from_slice(&data_message(
                0,
                &[(300, vec![(2, vec![20; 4000]), (3, vec![30; 4000])])],
            ));
            record_tx.send(Arc::new(input)).await.unwrap();
            let task = tokio::spawn(IpfixActor::run(actor));
            let first = tokio::time::timeout(Duration::from_secs(1), healthy_rx.recv())
                .await
                .unwrap()
                .unwrap();
            let second = tokio::time::timeout(Duration::from_secs(1), healthy_rx.recv())
                .await
                .unwrap()
                .unwrap();
            assert_eq!(
                first
                    .iter()
                    .map(|record| record.observation_time)
                    .collect::<Vec<_>>(),
                vec![1]
            );
            assert_eq!(
                second
                    .iter()
                    .map(|record| record.observation_time)
                    .collect::<Vec<_>>(),
                vec![2, 3]
            );
            assert!(!task.is_finished());
            record_tx
                .send(Arc::new(data_message(
                    0,
                    &[(300, vec![(4, vec![40; 4000])])],
                )))
                .await
                .unwrap();
            let later = tokio::time::timeout(Duration::from_secs(1), healthy_rx.recv())
                .await
                .unwrap()
                .unwrap();
            assert_eq!(later.iter().next().unwrap().observation_time, 4);
            drop(record_tx);
            tokio::time::timeout(Duration::from_secs(1), task)
                .await
                .unwrap()
                .unwrap();
        }
    }
}
