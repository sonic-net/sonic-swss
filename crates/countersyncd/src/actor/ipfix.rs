use std::{
    error::Error,
    fmt::{Display, Formatter},
    sync::Arc,
    time::Duration,
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
const DROP_WARNING_INTERVAL: Duration = Duration::from_secs(5);
const MAX_TEMPLATE_CONFIG_BYTES: usize = 4 * 1024 * 1024;
const MAX_DATA_SETS_PER_RECORD_INPUT: usize = 4096;
const MAX_RECORD_INPUTS_PER_BATCH: usize = 64;
const MAX_RECORD_INPUT_BYTES_PER_BATCH: usize = 4 * 1024 * 1024;
// A batching target, not a limit on a template or logical record.
const TARGET_COUNTERS_PER_BATCH: usize = 8192;
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

    pub(crate) fn validate_templates(update: &IPFixTemplatesMessage) -> Result<(), IpfixError> {
        Self::compile_generation(update).map(|_| ())
    }

    fn compile_generation(
        templates: &IPFixTemplatesMessage,
    ) -> Result<TemplateGeneration, IpfixError> {
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

    fn handle_template(&mut self, update: IPFixTemplatesMessage) -> Result<(), IpfixError> {
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
            let template = self.installed.get(&key).cloned();
            let layout = template
                .as_deref()
                .map(|template| validate_data_set(template, set))
                .transpose()?;
            if let Some(layout) = layout {
                counter_count = counter_count
                    .checked_add(layout.counter_count)
                    .ok_or("decoded counter count overflow")?;
            }
            sets.push(ValidatedDataSet {
                key,
                bytes: set,
                template,
                layout,
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
            if let (Some(template), Some(layout)) = (set.template, set.layout) {
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
        let input = match self.validate_record_input(records) {
            Ok(input) => input,
            Err(err) => {
                warn!("Dropping invalid HFT IPFIX message: {err}");
                return Ok(());
            }
        };
        for validated in input.messages {
            if !batch.is_empty()
                && batch
                    .counter_count()
                    .saturating_add(validated.counter_count)
                    > TARGET_COUNTERS_PER_BATCH
            {
                self.send_batch(std::mem::take(batch)).await?;
            }
            self.process_data_message(validated, batch);
            if batch.counter_count() >= TARGET_COUNTERS_PER_BATCH {
                self.send_batch(std::mem::take(batch)).await?;
            }
        }
        Ok(())
    }

    async fn send_batch(&self, batch: SAIStatsBatch) -> Result<(), IpfixError> {
        if batch.is_empty() || self.saistats_recipients.is_empty() {
            return Ok(());
        }
        if batch.counter_count() <= TARGET_COUNTERS_PER_BATCH || batch.record_count() == 1 {
            let closed = self.send_chunk(batch).await;
            return if closed == 0 {
                Ok(())
            } else {
                Err(format!("{closed} SAI stats recipient(s) closed").into())
            };
        }
        let mut closed = 0usize;
        for batch in batch.into_record_batches(TARGET_COUNTERS_PER_BATCH) {
            closed = closed.saturating_add(self.send_chunk(batch).await);
        }
        if closed > 0 {
            return Err(format!("{closed} SAI stats recipient send(s) closed").into());
        }
        Ok(())
    }

    async fn send_chunk(&self, batch: SAIStatsBatch) -> usize {
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

    pub async fn run(mut actor: IpfixActor) -> Result<(), IpfixError> {
        loop {
            select! {
                template = actor.template_recipient.recv() => match template {
                    Some(template) => {
                        record_comm_stats(ChannelLabel::SwssToIpfixTemplates, actor.template_recipient.len());
                        if let Err(err) = actor.handle_template(template) {
                            error!("HFT template update rejected: {err}");
                        }
                    }
                    None => return Err("IPFIX template input channel closed".into()),
                },
                record = actor.record_recipient.recv() => match record {
                    Some(record) => {
                        record_comm_stats(ChannelLabel::DataNetlinkToIpfixRecords, actor.record_recipient.len());
                        let mut batch = SAIStatsBatch::default();
                        let mut input_count = 1usize;
                        let mut input_bytes = record.len();
                        actor.process_record_input(&record, &mut batch).await?;
                        while input_count < MAX_RECORD_INPUTS_PER_BATCH
                            && input_bytes < MAX_RECORD_INPUT_BYTES_PER_BATCH
                            && actor.template_recipient.is_empty()
                        {
                            let Ok(next) = actor.record_recipient.try_recv() else { break; };
                            record_comm_stats(ChannelLabel::DataNetlinkToIpfixRecords, actor.record_recipient.len());
                            input_count += 1;
                            input_bytes = input_bytes.saturating_add(next.len());
                            actor.process_record_input(&next, &mut batch).await?;
                        }
                        actor.send_batch(batch).await?;
                    }
                    None => return Err("IPFIX record input channel closed".into()),
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
        if field_count < 2 || 4 + (field_count - 1) * 8 > set.len() - offset {
            return Err(format!("template {template_id} has a truncated field list").into());
        }
        let mut counters = Vec::with_capacity(field_count.saturating_sub(1));
        let mut observation_time_offset = None;
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
                    return Err(format!("template {template_id} observation time has length {field_len}; expected {OBSERVATION_TIME_LEN}").into());
                }
                if !field_keys.insert((field_id, None)) {
                    return Err(format!(
                        "template {template_id} contains duplicate observation time fields"
                    )
                    .into());
                }
                observation_time_offset = Some(record_len);
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
        if observation_time_offset.is_none() || counters.is_empty() {
            return Err(format!("template {template_id} requires exactly one observation time and at least one counter").into());
        }
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
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use tokio::sync::mpsc::channel;

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
        assert_eq!(template.observation_time_offset, 9);
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
        actor.send_batch(batch).await.unwrap();
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
        actor
            .process_record_input(&input, &mut batch)
            .await
            .unwrap();
        actor.send_batch(batch).await.unwrap();
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
        actor
            .process_record_input(&input, &mut batch)
            .await
            .unwrap();
        assert!(batch.is_empty());
        assert!(rx.try_recv().is_err());
        assert_eq!(keys(&actor), vec![(0, 300), (0, 400)]);
    }

    #[tokio::test]
    async fn delivery_error_propagates_from_initial_and_queued_inputs() {
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
            let error = tokio::time::timeout(Duration::from_secs(1), IpfixActor::run(actor))
                .await
                .unwrap()
                .unwrap_err();
            assert!(error.to_string().contains("recipient"));
            assert_eq!(
                healthy_rx
                    .recv()
                    .await
                    .unwrap()
                    .iter()
                    .next()
                    .unwrap()
                    .observation_time,
                1
            );
        }
    }
}
