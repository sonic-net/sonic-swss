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
        IPFixOwnerUpdate, IPFixTemplateOperation, IPFixTemplatesMessage, MAX_OBJECTS_PER_UPDATE,
        MAX_OBJECT_METADATA_BYTES, MAX_TEMPLATE_CONFIG_BYTES,
    },
    saistats::{decode_sai_ids, SAIStat, SAIStatsBatch, SAIStatsBatchMessage},
};
use crate::utilities::{record_comm_stats, ChannelLabel};

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
const MAX_DATA_SETS_PER_RECORD_INPUT: usize = 4096;
const MAX_RECORD_INPUTS_PER_BATCH: usize = 64;
const MAX_RECORD_INPUT_BYTES_PER_BATCH: usize = 4 * 1024 * 1024;
// A batching target, not a limit on a template or logical record.
const TARGET_COUNTERS_PER_BATCH: usize = 8192;

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
    ProcessingTime,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct CompiledTemplate {
    key: TemplateKey,
    owner: Arc<str>,
    observation_time: ObservationTime,
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

struct ReconciliationPlan {
    sessions: HashMap<Arc<str>, SessionTemplates>,
    installed: HashMap<TemplateKey, Arc<CompiledTemplate>>,
    rejections: Vec<String>,
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
pub struct IpfixActor {
    saistats_recipients: Vec<Sender<SAIStatsBatchMessage>>,
    template_recipient: Receiver<IPFixTemplatesMessage>,
    record_recipient: Receiver<SocketBufferMessage>,
    sessions: HashMap<Arc<str>, SessionTemplates>,
    installed: HashMap<TemplateKey, Arc<CompiledTemplate>>,
    dropped_sets: u64,
    next_drop_warning: Instant,
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
            dropped_sets: 0,
            next_drop_warning: Instant::now(),
        }
    }

    pub fn add_recipient(&mut self, recipient: Sender<SAIStatsBatchMessage>) {
        self.saistats_recipients.push(recipient);
    }

    fn compile_generation(templates: &IPFixOwnerUpdate) -> Result<TemplateGeneration, IpfixError> {
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
        Ok(TemplateGeneration {
            templates: compiled,
        })
    }

    fn remove_session(&mut self, owner: &str) {
        self.sessions.remove(owner);
        self.installed
            .retain(|_, template| template.owner.as_ref() != owner);
    }

    fn handle_templates(&mut self, update: IPFixTemplatesMessage) -> Result<(), IpfixError> {
        match update {
            IPFixTemplatesMessage::Owner(update) => self.handle_template(update),
            IPFixTemplatesMessage::Reconcile(snapshots) => self.reconcile_templates(snapshots),
        }
    }

    fn handle_template(&mut self, update: IPFixOwnerUpdate) -> Result<(), IpfixError> {
        if matches!(
            update.operation,
            IPFixTemplateOperation::Delete | IPFixTemplateOperation::Deactivate
        ) {
            self.remove_session(&update.key);
            return Ok(());
        }

        let mut generation = match Self::compile_generation(&update) {
            Ok(generation) => generation,
            Err(err) => {
                // Malformed configuration locally deactivates its owner.
                self.remove_session(&update.key);
                return Err(err);
            }
        };
        // Check the entire candidate before changing any installed/session state.
        // Conflicts, unlike compile errors, must preserve both owners' snapshots.
        for (key, template) in &mut generation.templates {
            if let Some(installed) = self.installed.get(key) {
                if installed.as_ref() != template.as_ref() {
                    return Err(format!(
                        "template collision at {key:?}: incoming owner {:?}, existing owner {:?}; different schema or owner",
                        update.key, installed.owner
                    )
                    .into());
                }
                // Shared keys have one decoder/allocation across both generations.
                *template = Arc::clone(installed);
            }
        }
        let owner = Arc::clone(
            &generation
                .templates
                .values()
                .next()
                .expect("nonempty generation")
                .owner,
        );
        let session = match self.sessions.get(update.key.as_str()) {
            Some(previous) => SessionTemplates {
                active: previous.active.clone(),
                pending: (generation != previous.active).then_some(generation),
            },
            None => SessionTemplates {
                active: generation,
                pending: None,
            },
        };

        // Keep active plus the latest pending snapshot, never historical pending keys.
        self.installed
            .retain(|_, template| template.owner.as_ref() != update.key);
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

    fn reconcile_templates(&mut self, snapshots: Vec<IPFixOwnerUpdate>) -> Result<(), IpfixError> {
        let mut owners = HashSet::new();
        let mut candidates = HashMap::new();
        // Validate the envelope before touching live state. Missing, disabled or
        // malformed owners are removals; valid but conflicting owners may retain
        // their previous complete active/pending state.
        for update in snapshots {
            if !owners.insert(update.key.clone()) {
                return Err(format!("duplicate reconciliation owner {:?}", update.key).into());
            }
            if update.operation != IPFixTemplateOperation::Update {
                continue;
            }
            match Self::compile_generation(&update) {
                Ok(generation) => {
                    candidates.insert(Arc::<str>::from(update.key), generation);
                }
                Err(err) => error!(
                    "Removing invalid reconciled HFT session {}: {err}",
                    update.key
                ),
            }
        }
        let plan = plan_reconciliation(&self.sessions, &self.installed, candidates)?;
        for reason in &plan.rejections {
            error!("{reason}");
        }
        // No await: data sees either registry, never a reset/reinstall gap.
        self.sessions = plan.sessions;
        self.installed = plan.installed;
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
        message: ValidatedDataMessage<'_>,
        batch: &mut SAIStatsBatch,
    ) {
        let dropped_before = self.dropped_sets;
        for set in message.sets {
            if let Some((template, layout)) = set.decoder {
                // A preceding Set in this input may have retired this descriptor.
                if self
                    .installed
                    .get(&set.key)
                    .is_some_and(|installed| Arc::ptr_eq(installed, &template))
                {
                    self.promote_pending_for(&template);
                    self.decode_set(&template, set.bytes, layout, batch);
                    continue;
                }
            }
            self.dropped_sets = self.dropped_sets.saturating_add(1);
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
        batch: &mut SAIStatsBatch,
    ) {
        let payload = &set[SET_HEADER_LEN..];
        batch.reserve(layout.record_count, layout.counter_count);
        for record in payload[..layout.record_bytes].chunks_exact(template.record_len) {
            let observation_time = match template.observation_time {
                ObservationTime::RawNanoseconds { offset } => NetworkEndian::read_u64(
                    &record[offset..offset + usize::from(OBSERVATION_TIME_LEN)],
                ),
                ObservationTime::Split {
                    seconds_offset,
                    nanos_offset,
                } => {
                    let seconds = NetworkEndian::read_u32(
                        &record[seconds_offset
                            ..seconds_offset + usize::from(SPLIT_OBSERVATION_TIME_LEN)],
                    );
                    let nanos = NetworkEndian::read_u32(
                        &record
                            [nanos_offset..nanos_offset + usize::from(SPLIT_OBSERVATION_TIME_LEN)],
                    );
                    u64::from(seconds) * NANOS_PER_SECOND + u64::from(nanos)
                }
                ObservationTime::ProcessingTime => SystemTime::now()
                    .duration_since(SystemTime::UNIX_EPOCH)
                    .expect("System time should be after Unix epoch")
                    .as_nanos() as u64,
            };
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

    async fn process_record_input(&mut self, records: &[u8], batch: &mut SAIStatsBatch) {
        let input = match self.validate_record_input(records) {
            Ok(input) => input,
            Err(err) => {
                warn!("Dropping invalid HFT IPFIX message: {err}");
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
                        if let Err(err) = actor.handle_templates(template) {
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

fn plan_reconciliation(
    current_sessions: &HashMap<Arc<str>, SessionTemplates>,
    installed: &HashMap<TemplateKey, Arc<CompiledTemplate>>,
    candidates: HashMap<Arc<str>, TemplateGeneration>,
) -> Result<ReconciliationPlan, IpfixError> {
    // An unchanged pending row carries no new cutover signal. Preserve its
    // active decoder as well; otherwise recovery would release an in-use ID.
    let preserve_pending = candidates
        .iter()
        .filter_map(|(owner, generation)| {
            current_sessions
                .get(owner.as_ref())
                .and_then(|session| session.pending.as_ref())
                .filter(|pending| *pending == generation)
                .map(|_| Arc::clone(owner))
        })
        .collect::<HashSet<_>>();
    let mut claims: HashMap<TemplateKey, Vec<Arc<str>>> = HashMap::new();
    for (owner, generation) in &candidates {
        for key in generation.templates.keys() {
            claims.entry(*key).or_default().push(Arc::clone(owner));
        }
    }
    let mut rejected = HashSet::new();
    let mut rejections = Vec::new();
    let mut work = Vec::new();
    for (key, claimants) in &claims {
        let incumbent = installed
            .get(key)
            .map(|template| &template.owner)
            .filter(|owner| {
                preserve_pending.contains(*owner)
                    || candidates
                        .get(owner.as_ref())
                        .is_some_and(|generation| generation.templates.contains_key(key))
            });
        for owner in claimants {
            let conflict = match incumbent {
                Some(incumbent) => owner != incumbent,
                None => claimants.len() > 1,
            };
            if conflict && rejected.insert(Arc::clone(owner)) {
                rejections.push(format!("Rejecting reconciled HFT session {owner}: collision at {key:?}, incumbent {incumbent:?}, claimants {claimants:?}"));
                work.push(Arc::clone(owner));
            }
        }
    }
    // Rejection preserves the entire previous snapshot, including keys the
    // rejected candidate omitted. Propagate those reservations monotonically;
    // never manufacture a winner by removing a rejected owner's claims.
    while let Some(owner) = work.pop() {
        let Some(previous) = current_sessions.get(owner.as_ref()) else {
            continue;
        };
        for generation in std::iter::once(&previous.active).chain(previous.pending.iter()) {
            for key in generation.templates.keys() {
                for claimant in claims.get(key).into_iter().flatten() {
                    if claimant != &owner && rejected.insert(Arc::clone(claimant)) {
                        rejections.push(format!("Rejecting reconciled HFT session {claimant}: collision at {key:?} with retained owner {owner}"));
                        work.push(Arc::clone(claimant));
                    }
                }
            }
        }
    }
    let mut sessions = HashMap::new();
    let mut installed = HashMap::new();
    for (owner, generation) in candidates {
        let session = if rejected.contains(&owner) || preserve_pending.contains(&owner) {
            let Some(previous) = current_sessions.get(owner.as_ref()) else {
                continue;
            };
            previous.clone()
        } else {
            SessionTemplates {
                active: generation,
                pending: None,
            }
        };
        for generation in std::iter::once(&session.active).chain(session.pending.iter()) {
            for (key, template) in &generation.templates {
                if installed
                    .get(key)
                    .is_some_and(|existing: &Arc<CompiledTemplate>| existing.owner != owner)
                {
                    return Err(format!("conflicting reconciliation result at {key:?}").into());
                }
                installed.insert(*key, Arc::clone(template));
            }
        }
        sessions.insert(owner, session);
    }
    Ok(ReconciliationPlan {
        sessions,
        installed,
        rejections,
    })
}

fn validate_template_update_limits(templates: &IPFixOwnerUpdate) -> Result<(), IpfixError> {
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
                let object_name = object_names.get(&field_id).ok_or_else(|| {
                    format!("template {template_id} references unmapped object ID {field_id}")
                })?;
                let (type_id, stat_id) = decode_sai_ids(enterprise_number);
                if !field_keys.insert((field_id, enterprise_number)) {
                    return Err(format!(
                        "template {template_id} contains a duplicate counter field"
                    )
                    .into());
                }
                counters.push(CompiledCounter {
                    offset: field_offset,
                    len: u8::try_from(field_len).expect("counter length is at most 8"),
                    object_name: Arc::clone(object_name),
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
        if counters.is_empty() {
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
            ObservationTime::ProcessingTime
        };
        let key = TemplateKey {
            observation_domain_id: domain,
            template_id,
        };
        let template = Arc::new(CompiledTemplate {
            key,
            owner: Arc::clone(owner),
            observation_time,
            counters: counters.into(),
            record_len,
        });
        if output.insert(key, template).is_some() {
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

    // Field specifiers preserve the E bit independently of the IE number.
    fn hardware_template(id: u16, fields: &[(u16, u16, Option<u32>)]) -> IPFixOwnerUpdate {
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
        IPFixOwnerUpdate::new(
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
        assert_eq!(two.observation_time, ObservationTime::ProcessingTime);
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
        let before = utc_nanos();
        let output = actor.handle_record(&data).unwrap();
        let after = utc_nanos();
        let records = output.iter().collect::<Vec<_>>();
        assert_eq!(output.counter_count(), 5); // No unknown_0 placeholder metrics.
        assert_eq!(records[0].observation_time, 1_788_655_919_123_456_789);
        assert_eq!(records[1].observation_time, 1_788_655_919_123_456_790);
        assert!((before..=after).contains(&records[2].observation_time));
        assert_eq!(records[0].stats[0].counter, 0xfedc_ba98_7654_3210);
        assert_eq!(records[0].stats[0].object_name.as_ref(), "Ethernet325");
        assert_eq!(
            (records[0].stats[0].type_id, records[0].stats[0].stat_id),
            (21, 1)
        );
        assert_eq!(records[0].stats[1].counter, 0xf123_4567);
        assert_eq!(records[2].stats[0].counter, 0x8765_4321);
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
                let previous: &mut IPFixOwnerUpdate = previous;
                Arc::make_mut(previous.templates.as_mut().unwrap())
                    .extend_from_slice(update.templates.as_ref().unwrap());
            } else {
                snapshot = Some(update);
            }
            input.extend_from_slice(&hardware_data(id, &[&record]));
        }
        let mut actor = actor();
        actor.handle_template(snapshot.unwrap()).unwrap();
        let before = utc_nanos();
        let batch = actor.handle_record(&input).unwrap();
        let after = utc_nanos();
        assert_eq!(batch.counter_count(), 3488);
        assert_eq!(batch.record_count(), 2);
        for (index, record) in batch.iter().enumerate() {
            assert_eq!(record.stats.len(), if index == 0 { 1888 } else { 1600 });
            if index == 0 {
                assert_eq!(record.observation_time, 100_000_000_200);
            } else {
                assert!((before..=after).contains(&record.observation_time));
            }
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
            assert_eq!(output.stats[0].counter, 0xff_ffff);
            assert_eq!(output.stats[1].counter, 0xffff_ffff_ffff);
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
                output.iter().next().unwrap().stats[0].counter,
                u64::MAX >> (64 - width * 8)
            );
            actor
                .handle_template(IPFixOwnerUpdate::delete("hardware".into()))
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
    ) -> IPFixOwnerUpdate {
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
        IPFixOwnerUpdate::new(owner.into(), Arc::new(bytes), Some(names), Some(ids))
    }

    fn snapshot(owner: &str, templates: &[(u32, u16, u32)]) -> IPFixOwnerUpdate {
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
        assert_eq!(batch.iter().nth(1).unwrap().stats[0].stat_id, 3);
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
    fn reconciliation_ownership_is_order_independent_and_snapshot_atomic() {
        type Spec = (&'static str, Vec<(u32, u16, u32)>);
        let cases: Vec<(Vec<Spec>, Vec<Spec>, Vec<Spec>)> = vec![
            (
                vec![("A", vec![(0, 300, 1)])],
                vec![("A", vec![(0, 300, 1)]), ("B", vec![(0, 300, 2)])],
                vec![("A", vec![(0, 300, 1)])],
            ),
            (
                vec![("A", vec![(0, 300, 1)])],
                vec![("A", vec![(0, 300, 2)]), ("B", vec![(0, 300, 3)])],
                vec![("A", vec![(0, 300, 2)])],
            ),
            (
                vec![("A", vec![(0, 300, 1)])],
                vec![("B", vec![(0, 300, 2)])],
                vec![("B", vec![(0, 300, 2)])],
            ),
            (
                vec![("A", vec![(0, 300, 1)])],
                vec![("B", vec![(0, 300, 2)]), ("C", vec![(0, 300, 3)])],
                vec![],
            ),
            (
                vec![],
                vec![
                    ("A", vec![(0, 300, 1), (0, 400, 1)]),
                    ("B", vec![(0, 400, 2), (0, 500, 2)]),
                ],
                vec![],
            ),
            (
                vec![("A", vec![(0, 300, 1)]), ("B", vec![(0, 400, 2)])],
                vec![
                    ("A", vec![(0, 400, 1)]),
                    ("B", vec![(0, 400, 2)]),
                    ("C", vec![(0, 300, 3)]),
                ],
                vec![("A", vec![(0, 300, 1)]), ("B", vec![(0, 400, 2)])],
            ),
            (
                vec![
                    ("A", vec![(0, 300, 1)]),
                    ("B", vec![(0, 400, 2)]),
                    ("C", vec![(0, 500, 3)]),
                ],
                vec![
                    ("A", vec![(0, 400, 1)]),
                    ("B", vec![(0, 500, 2)]),
                    ("C", vec![(0, 500, 3)]),
                    ("D", vec![(0, 300, 4)]),
                ],
                vec![
                    ("A", vec![(0, 300, 1)]),
                    ("B", vec![(0, 400, 2)]),
                    ("C", vec![(0, 500, 3)]),
                ],
            ),
            (
                vec![("A", vec![(0, 300, 1)]), ("B", vec![(0, 400, 2)])],
                vec![("A", vec![(0, 400, 1)]), ("B", vec![(0, 300, 2)])],
                vec![("A", vec![(0, 400, 1)]), ("B", vec![(0, 300, 2)])],
            ),
            (
                vec![("A", vec![(0, 300, 1)])],
                vec![("A", vec![(0, 300, 1)]), ("B", vec![(1, 300, 2)])],
                vec![("A", vec![(0, 300, 1)]), ("B", vec![(1, 300, 2)])],
            ),
            (
                vec![("X", vec![(0, 700, 4)])],
                vec![
                    ("X", vec![(0, 700, 4)]),
                    ("A", vec![(0, 600, 1), (0, 700, 1)]),
                    ("B", vec![(0, 600, 2)]),
                ],
                vec![("X", vec![(0, 700, 4)])],
            ),
        ];
        for (initial, candidates, expected) in cases {
            for reverse in [false, true] {
                for rotation in 0..candidates.len().max(1) {
                    let mut actor = actor();
                    for (owner, templates) in &initial {
                        actor.handle_template(snapshot(owner, templates)).unwrap();
                    }
                    let mut updates = candidates
                        .iter()
                        .map(|(owner, templates)| snapshot(owner, templates))
                        .collect::<Vec<_>>();
                    if !updates.is_empty() {
                        updates.rotate_left(rotation);
                    }
                    if reverse {
                        updates.reverse();
                    }
                    let compiled = updates
                        .iter()
                        .map(|update| {
                            (
                                Arc::<str>::from(update.key.as_str()),
                                IpfixActor::compile_generation(update).unwrap(),
                            )
                        })
                        .collect();
                    let before_sessions = actor.sessions.clone();
                    let before_installed = actor.installed.clone();
                    let plan =
                        plan_reconciliation(&actor.sessions, &actor.installed, compiled).unwrap();
                    assert_eq!(actor.sessions, before_sessions);
                    assert_eq!(actor.installed, before_installed);
                    let input = IPFixTemplatesMessage::Reconcile(updates);
                    actor.handle_templates(input.clone()).unwrap();
                    assert_eq!(actor.sessions, plan.sessions);
                    assert_eq!(actor.installed, plan.installed);
                    let expected_map = expected
                        .iter()
                        .flat_map(|(owner, templates)| {
                            templates.iter().map(move |&(domain, id, stat)| {
                                ((domain, id), (owner.to_string(), stat))
                            })
                        })
                        .collect::<std::collections::BTreeMap<_, _>>();
                    let actual = actor
                        .installed
                        .iter()
                        .map(|(key, t)| {
                            (
                                (key.observation_domain_id, key.template_id),
                                (t.owner.to_string(), u32::from(t.counters[0].stat_id)),
                            )
                        })
                        .collect::<std::collections::BTreeMap<_, _>>();
                    assert_eq!(
                        actual, expected_map,
                        "candidates={candidates:?}, reverse={reverse},rotation={rotation}"
                    );
                    let before = actor.sessions.clone();
                    actor.handle_templates(input).unwrap();
                    assert_eq!(actor.sessions, before, "recovery must be idempotent");
                }
            }
        }
    }

    #[test]
    fn unchanged_pending_reconciliation_preserves_incumbent_until_real_cutover() {
        for reverse in [false, true] {
            for cancel in [false, true] {
                let mut actor = actor();
                let active = snapshot("A", &[(0, 300, 1)]);
                let pending = snapshot("A", &[(0, 400, 2)]);
                actor.handle_template(active.clone()).unwrap();
                actor.handle_template(pending.clone()).unwrap();
                assert!(actor
                    .handle_template(snapshot("B", &[(0, 300, 3)]))
                    .is_err());
                let previous = actor.sessions["A"].clone();
                let mut rows = vec![pending, snapshot("B", &[(0, 300, 3)])];
                if reverse {
                    rows.reverse();
                }
                for _ in 0..2 {
                    actor
                        .handle_templates(IPFixTemplatesMessage::Reconcile(rows.clone()))
                        .unwrap();
                    assert_eq!(actor.sessions["A"], previous);
                    assert!(!actor.sessions.contains_key("B"));
                    let batch = actor
                        .handle_record(&data_message(0, &[(300, vec![(1, vec![10])])]))
                        .unwrap();
                    assert_eq!(batch.iter().next().unwrap().stats[0].stat_id, 1);
                }
                if cancel {
                    actor.handle_template(active).unwrap();
                    assert_eq!(keys(&actor), vec![(0, 300)]);
                    assert!(actor.sessions["A"].pending.is_none());
                } else {
                    actor
                        .handle_record(&data_message(0, &[(400, vec![(2, vec![20])])]))
                        .unwrap();
                    assert_eq!(keys(&actor), vec![(0, 400)]);
                    assert!(actor.sessions["A"].pending.is_none());
                }
            }
        }
    }

    #[test]
    fn rejected_reconciliation_retains_pending_keys_and_complete_metadata() {
        for reverse in [false, true] {
            let mut actor = actor();
            actor
                .handle_template(snapshot("A", &[(0, 300, 1)]))
                .unwrap();
            actor
                .handle_template(snapshot("A", &[(0, 400, 2)]))
                .unwrap();
            actor
                .handle_template(snapshot("X", &[(0, 700, 3)]))
                .unwrap();
            let previous = actor.sessions["A"].clone();
            let mut updates = vec![
                snapshot("A", &[(0, 300, 4), (0, 500, 4), (0, 700, 4)]),
                snapshot("X", &[(0, 700, 3)]),
                snapshot("B", &[(0, 400, 5)]),
            ];
            if reverse {
                updates.reverse();
            }
            actor
                .handle_templates(IPFixTemplatesMessage::Reconcile(updates))
                .unwrap();
            assert_eq!(actor.sessions["A"], previous);
            assert!(!actor.sessions.contains_key("B"));
            assert_eq!(keys(&actor), vec![(0, 300), (0, 400), (0, 700)]);
            assert_eq!(
                actor.installed[&TemplateKey {
                    observation_domain_id: 0,
                    template_id: 300
                }]
                    .counters[0]
                    .stat_id,
                1
            );
        }
    }

    #[test]
    fn authoritative_reconciliation_releases_removed_or_invalid_owners() {
        for removal in 0..4 {
            let mut actor = actor();
            actor
                .handle_template(snapshot("A", &[(0, 300, 1)]))
                .unwrap();
            actor
                .handle_template(snapshot("A", &[(0, 400, 2)]))
                .unwrap();
            let mut rows = vec![snapshot("B", &[(0, 300, 3), (0, 400, 3)])];
            match removal {
                0 => {}
                1 => rows.push(IPFixOwnerUpdate::delete("A".into())),
                2 => rows.push(IPFixOwnerUpdate::deactivate("A".into())),
                _ => {
                    let mut invalid = snapshot("A", &[(0, 300, 1)]);
                    invalid.templates = Some(Arc::new(vec![1, 2, 3]));
                    rows.push(invalid);
                }
            }
            actor
                .handle_templates(IPFixTemplatesMessage::Reconcile(rows))
                .unwrap();
            assert!(!actor.sessions.contains_key("A"));
            assert!(actor
                .installed
                .values()
                .all(|template| template.owner.as_ref() == "B"));
        }
    }

    #[test]
    fn duplicate_reconciliation_owner_never_partially_commits() {
        let mut actor = actor();
        actor
            .handle_template(snapshot("A", &[(0, 300, 1)]))
            .unwrap();
        let before = actor.sessions.clone();
        let rows = vec![snapshot("B", &[(0, 400, 2)]), snapshot("B", &[(0, 500, 3)])];
        assert!(actor
            .handle_templates(IPFixTemplatesMessage::Reconcile(rows))
            .is_err());
        assert_eq!(actor.sessions, before);
        assert_eq!(keys(&actor), vec![(0, 300)]);
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
        assert_eq!(batch.iter().next().unwrap().stats[0].counter, 255);
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
            assert_eq!(keys(&actor), vec![(0, 300), (0, 400)]);
            assert!(actor.sessions["s"].pending.is_some());
        }
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
            assert_eq!(batch.iter().next().unwrap().stats[0].stat_id, 2);
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
            batch.iter().map(|r| r.stats[0].stat_id).collect::<Vec<_>>(),
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
                IPFixTemplatesMessage::Owner(IPFixOwnerUpdate::delete("a".into()))
            } else {
                IPFixTemplatesMessage::Owner(IPFixOwnerUpdate::deactivate("a".into()))
            };
            actor.handle_templates(update).unwrap();
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
            assert_eq!(batch.iter().next().unwrap().stats[0].stat_id, 3);
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
            .handle_templates(IPFixTemplatesMessage::Owner(IPFixOwnerUpdate::delete(
                "unknown".into(),
            )))
            .unwrap();
        assert_eq!(actor.sessions.len(), 2);
        let mut input = data_message(0, &[(300, vec![(1, vec![1])])]);
        input.extend_from_slice(&data_message(1, &[(300, vec![(2, vec![2])])]));
        assert_eq!(
            actor
                .handle_record(&input)
                .unwrap()
                .iter()
                .map(|r| r.stats[0].stat_id)
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
                    assert_eq!(record.stats[0].counter, expected);
                    assert_eq!(
                        (record.stats[0].type_id, record.stats[0].stat_id),
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
