use std::{
    collections::VecDeque,
    error::Error,
    fmt::{Display, Formatter},
    sync::Arc,
    time::{Duration, Instant},
};

use ahash::{HashMap, HashMapExt, HashSet, HashSetExt};
use byteorder::{ByteOrder, NetworkEndian};
use log::{debug, error, warn};
use tokio::{
    select,
    sync::mpsc::{Receiver, Sender},
};

use super::super::message::{
    buffer::SocketBufferMessage,
    ipfix::IPFixTemplatesMessage,
    saistats::{decode_sai_ids, SAIStat, SAIStatsBatch, SAIStatsBatchMessage},
};
use crate::utilities::{record_comm_stats, ChannelLabel};

const IPFIX_VERSION: u16 = 10;
const IPFIX_HEADER_LEN: usize = 16;
const SET_HEADER_LEN: usize = 4;
const TEMPLATE_SET_ID: u16 = 2;
const MIN_DATA_SET_ID: u16 = 256;
const OBSERVATION_TIME_NANOSECONDS: u16 = 325;
const HFT_FIELD_LEN: u16 = 8;
const MAX_UNKNOWN_SETS: usize = 256;
const MAX_UNKNOWN_SET_BYTES: usize = 4 * 1024 * 1024;
const UNKNOWN_SET_TTL: Duration = Duration::from_secs(5);
const MAX_RETIRED_TEMPLATES: usize = 4096;
const MAX_TEMPLATE_CONFIG_BYTES: usize = 4 * 1024 * 1024;
const MAX_TEMPLATES_PER_UPDATE: usize = 1024;
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

#[derive(Debug, Clone, PartialEq, Eq)]
struct TemplateGeneration {
    owner: Arc<str>,
    templates: HashMap<TemplateKey, Arc<CompiledTemplate>>,
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
}

#[derive(Debug, Clone)]
struct BufferedSet {
    key: TemplateKey,
    bytes: Arc<[u8]>,
    received_at: Instant,
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
        while self
            .sets
            .front()
            .is_some_and(|set| now.duration_since(set.received_at) >= UNKNOWN_SET_TTL)
        {
            let expired = self.sets.pop_front().expect("front checked above");
            self.bytes = self.bytes.saturating_sub(expired.bytes.len());
            self.dropped = self.dropped.saturating_add(1);
        }
        self.dropped - dropped_before
    }

    fn push(&mut self, set: BufferedSet) -> u64 {
        let dropped_before = self.dropped;
        let len = set.bytes.len();
        if len > MAX_UNKNOWN_SET_BYTES {
            self.dropped = self.dropped.saturating_add(1);
            return self.dropped - dropped_before;
        }
        while self.sets.len() >= MAX_UNKNOWN_SETS
            || self.bytes.saturating_add(len) > MAX_UNKNOWN_SET_BYTES
        {
            let Some(oldest) = self.sets.pop_front() else {
                break;
            };
            self.bytes = self.bytes.saturating_sub(oldest.bytes.len());
            self.dropped = self.dropped.saturating_add(1);
        }
        self.bytes += len;
        self.sets.push_back(set);
        self.dropped - dropped_before
    }

    fn pop_front(&mut self) -> Option<BufferedSet> {
        let set = self.sets.pop_front()?;
        self.bytes = self.bytes.saturating_sub(set.bytes.len());
        Some(set)
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
}

#[derive(Debug)]
struct RetiredTemplates {
    keys: HashSet<TemplateKey>,
}

impl Default for RetiredTemplates {
    fn default() -> Self {
        Self {
            keys: HashSet::new(),
        }
    }
}

impl RetiredTemplates {
    fn retire(&mut self, template: &CompiledTemplate) {
        self.keys.insert(template.key);
    }

    fn contains(&self, key: &TemplateKey) -> bool {
        self.keys.contains(key)
    }

    fn validate_reactivation(&self, template: &CompiledTemplate) -> Result<(), IpfixError> {
        if self.contains(&template.key) {
            return Err(format!(
                "template ({}, {}) cannot reuse a retired ID without an exporter generation boundary",
                template.key.observation_domain_id, template.key.template_id
            )
            .into());
        }
        Ok(())
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
    retired: RetiredTemplates,
    deferred_sets: DeferredSetBuffer,
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
            retired: RetiredTemplates::default(),
            deferred_sets: DeferredSetBuffer::default(),
        }
    }

    pub fn add_recipient(&mut self, recipient: Sender<SAIStatsBatchMessage>) {
        self.saistats_recipients.push(recipient);
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

        Ok(TemplateGeneration {
            owner,
            templates: compiled,
        })
    }

    fn handle_template(
        &mut self,
        templates: IPFixTemplatesMessage,
    ) -> Result<SAIStatsBatch, IpfixError> {
        if templates.is_delete {
            self.handle_template_deletion(&templates.key);
            return Ok(SAIStatsBatch::default());
        }

        let generation = match Self::compile_generation(&templates) {
            Ok(generation) => generation,
            Err(err) => {
                error!(
                    "Rejecting invalid HFT template update for {} and retaining the active generation: {}",
                    templates.key, err
                );
                return Err(err);
            }
        };
        let owner = Arc::clone(&generation.owner);

        if let Some(conflict) = generation.templates.keys().find_map(|key| {
            self.installed
                .get(key)
                .filter(|existing| existing.owner != owner)
        }) {
            return Err(format!(
                "template ({}, {}) is already owned by session {}",
                conflict.key.observation_domain_id, conflict.key.template_id, conflict.owner
            )
            .into());
        }

        let is_first_install = !self.sessions.contains_key(owner.as_ref());
        if is_first_install {
            validate_generation_reactivation(&generation, &self.retired)?;
            validate_registry_capacity(&generation, &self.installed, &self.retired)?;
            install_generation(&generation, &mut self.installed);
            self.sessions.insert(
                owner,
                SessionTemplates {
                    active: Some(generation),
                    pending: None,
                },
            );
            return self.finish_template_update();
        }

        let session = self
            .sessions
            .get_mut(owner.as_ref())
            .expect("session existence checked above");
        if session
            .active
            .as_ref()
            .is_some_and(|active| active.equivalent(&generation))
        {
            if let Some(pending) = session.pending.take() {
                retire_canceled_pending_generation(
                    &pending,
                    session.active.as_ref(),
                    &mut self.installed,
                    &mut self.retired,
                );
                debug!("Canceled pending HFT template generation for {owner}");
            }
            debug!("Refreshed unchanged HFT template generation for {owner}");
        } else if session
            .pending
            .as_ref()
            .is_some_and(|pending| pending.equivalent(&generation))
        {
            debug!("Refreshed unchanged pending HFT template generation for {owner}");
        } else {
            let active = session.active.as_ref().expect("checked above");
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
                    "session {owner} update has no new template ID to mark a lossless cutover"
                )
                .into());
            }
            validate_pending_replacement(
                &generation,
                session.pending.as_ref(),
                &self.installed,
                &self.retired,
            )?;
            if let Some(pending) = session.pending.take() {
                retire_replaced_pending_generation(
                    &pending,
                    &generation,
                    session.active.as_ref(),
                    &mut self.installed,
                    &mut self.retired,
                );
            }
            install_generation(&generation, &mut self.installed);
            session.pending = Some(generation);
        }

        self.finish_template_update()
    }

    fn finish_template_update(&mut self) -> Result<SAIStatsBatch, IpfixError> {
        let mut batch = SAIStatsBatch::default();
        self.drain_deferred_sets(&mut batch)?;
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
        let should_promote = session
            .pending
            .as_ref()
            .is_some_and(|pending| pending.templates.contains_key(&key))
            && session
                .active
                .as_ref()
                .is_some_and(|active| !active.templates.contains_key(&key));
        if !should_promote {
            return;
        }

        let pending = session.pending.take().expect("checked above");
        if let Some(active) = session.active.replace(pending.clone()) {
            for old_key in active.templates.keys() {
                if !pending.templates.contains_key(old_key) {
                    if let Some(old_template) = self.installed.remove(old_key) {
                        self.retired.retire(&old_template);
                    }
                }
            }
        }
        debug!("Promoted pending HFT template generation for {owner}");
    }

    fn handle_template_deletion(&mut self, key: &str) {
        let Some((owner, session)) = self.sessions.remove_entry(key) else {
            return;
        };
        let mut removed = HashSet::new();
        for generation in [session.active, session.pending].into_iter().flatten() {
            for template_key in generation.templates.keys() {
                if self
                    .installed
                    .get(template_key)
                    .is_some_and(|template| template.owner == owner)
                {
                    if let Some(template) = self.installed.remove(template_key) {
                        self.retired.retire(&template);
                    }
                    removed.insert(*template_key);
                }
            }
        }
        self.deferred_sets.remove_keys(&removed);
    }

    #[cfg(test)]
    fn handle_record(&mut self, records: &[u8]) -> Result<SAIStatsBatch, IpfixError> {
        let mut batch = SAIStatsBatch::default();
        if records.is_empty() {
            return Err("empty IPFIX payload".into());
        }
        for message in IpfixMessages::new(records) {
            let message = message?;
            self.validate_data_message(message)?;
            self.process_data_message(message, &mut batch)?;
        }
        Ok(batch)
    }

    fn validate_data_message(&self, message: &[u8]) -> Result<(), IpfixError> {
        let domain = NetworkEndian::read_u32(&message[12..16]);
        let mut offset = IPFIX_HEADER_LEN;
        let mut set_count = 0usize;
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
            if let Some(template) = self.installed.get(&key) {
                validate_data_set(template, set)?;
            }
            set_count += 1;
        }
        if set_count == 0 {
            return Err("IPFIX data message contains no sets".into());
        }
        Ok(())
    }

    fn process_data_message(
        &mut self,
        message: &[u8],
        batch: &mut SAIStatsBatch,
    ) -> Result<(), IpfixError> {
        let domain = NetworkEndian::read_u32(&message[12..16]);
        let mut offset = IPFIX_HEADER_LEN;
        while offset < message.len() {
            let (set_id, set) = next_set(message, &mut offset)?;
            if set.is_empty() {
                break;
            }
            let key = TemplateKey {
                observation_domain_id: domain,
                template_id: set_id,
            };
            if !self.deferred_sets.sets.is_empty() {
                let dropped = self.deferred_sets.push(BufferedSet {
                    key,
                    bytes: Arc::from(set),
                    received_at: Instant::now(),
                });
                self.log_deferred_drops(dropped);
            } else if let Some(template) = self.installed.get(&key) {
                validate_data_set(template, set)?;
                self.promote_pending_for(key);
                if self.installed.contains_key(&key) {
                    self.decode_set(key, set, batch)?;
                }
            } else if !self.retired.contains(&key) {
                let dropped = self.deferred_sets.push(BufferedSet {
                    key,
                    bytes: Arc::from(set),
                    received_at: Instant::now(),
                });
                self.log_deferred_drops(dropped);
            }
        }
        self.drain_deferred_sets(batch)?;
        Ok(())
    }

    fn drain_deferred_sets(&mut self, batch: &mut SAIStatsBatch) -> Result<(), IpfixError> {
        let expired = self.deferred_sets.expire(Instant::now());
        self.log_deferred_drops(expired);

        loop {
            let Some(front) = self.deferred_sets.sets.front() else {
                return Ok(());
            };
            if self.retired.contains(&front.key) {
                self.deferred_sets.pop_front();
                continue;
            }
            let Some(template) = self.installed.get(&front.key) else {
                return Ok(());
            };
            if let Err(err) = validate_data_set(template, &front.bytes) {
                let invalid = self.deferred_sets.pop_front().expect("front checked above");
                warn!(
                    "Dropping invalid deferred HFT data set ({}, {}): {}",
                    invalid.key.observation_domain_id, invalid.key.template_id, err
                );
                continue;
            }
            let counters = data_set_counter_count(template, &front.bytes)?;
            if !batch.is_empty()
                && batch.counter_count().saturating_add(counters) > MAX_COUNTERS_PER_BATCH
            {
                return Ok(());
            }

            let ready = self.deferred_sets.pop_front().expect("front checked above");
            self.promote_pending_for(ready.key);
            if self.installed.contains_key(&ready.key) {
                self.decode_set(ready.key, &ready.bytes, batch)?;
            }
        }
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
        key: TemplateKey,
        set: &[u8],
        batch: &mut SAIStatsBatch,
    ) -> Result<(), IpfixError> {
        let template = self
            .installed
            .get(&key)
            .ok_or_else(|| format!("missing installed template {key:?}"))?;
        if set.len() < SET_HEADER_LEN {
            return Err("IPFIX set is shorter than its header".into());
        }
        validate_data_set(template, set)?;
        let payload = &set[SET_HEADER_LEN..];
        let record_bytes = payload.len() / template.record_len * template.record_len;
        let record_count = record_bytes / template.record_len;
        batch.reserve(record_count, record_count * template.counters.len());

        for record in payload[..record_bytes].chunks_exact(template.record_len) {
            let time_offset = template.observation_time_offset;
            let observation_time = NetworkEndian::read_u64(&record[time_offset..time_offset + 8]);
            batch.push_record(
                observation_time,
                template.counters.iter().map(|counter| SAIStat {
                    object_name: Arc::clone(&counter.object_name),
                    type_id: counter.type_id,
                    stat_id: counter.stat_id,
                    counter: NetworkEndian::read_u64(
                        &record[counter.offset..counter.offset + HFT_FIELD_LEN as usize],
                    ),
                }),
            );
        }
        Ok(())
    }

    async fn process_record_input(
        &mut self,
        records: &[u8],
        batch: &mut SAIStatsBatch,
    ) -> Result<(), IpfixError> {
        if records.is_empty() {
            return Err("empty IPFIX payload".into());
        }
        for message in IpfixMessages::new(records) {
            let message = message?;
            self.validate_data_message(message)?;
            let counters = self.data_message_counter_count(message)?;
            if !batch.is_empty()
                && batch.counter_count().saturating_add(counters) > MAX_COUNTERS_PER_BATCH
            {
                self.send_batch(std::mem::take(batch)).await?;
            }
            self.process_data_message(message, batch)?;
            if batch.counter_count() >= MAX_COUNTERS_PER_BATCH {
                self.send_batch(std::mem::take(batch)).await?;
            }
        }
        Ok(())
    }

    fn data_message_counter_count(&self, message: &[u8]) -> Result<usize, IpfixError> {
        let domain = NetworkEndian::read_u32(&message[12..16]);
        let mut offset = IPFIX_HEADER_LEN;
        let mut counters = 0usize;
        while offset < message.len() {
            let (set_id, set) = next_set(message, &mut offset)?;
            if set.is_empty() {
                break;
            }
            let key = TemplateKey {
                observation_domain_id: domain,
                template_id: set_id,
            };
            if self.deferred_sets.sets.is_empty() {
                if let Some(template) = self.installed.get(&key) {
                    counters = counters
                        .checked_add(data_set_counter_count(template, set)?)
                        .ok_or("decoded counter count overflow")?;
                }
            }
        }
        Ok(counters)
    }

    async fn send_batch(&self, batch: SAIStatsBatch) -> Result<(), IpfixError> {
        if batch.is_empty() || self.saistats_recipients.is_empty() {
            return Ok(());
        }
        let batch = Arc::new(batch);
        let mut blocked = Vec::new();
        for recipient in &self.saistats_recipients {
            match recipient.try_reserve() {
                Ok(permit) => permit.send(Arc::clone(&batch)),
                Err(tokio::sync::mpsc::error::TrySendError::Full(_)) => blocked.push(recipient),
                Err(tokio::sync::mpsc::error::TrySendError::Closed(_)) => {
                    return Err("SAI stats recipient closed".into())
                }
            }
        }
        let mut closed = 0usize;
        for recipient in blocked {
            if recipient.send(Arc::clone(&batch)).await.is_err() {
                closed += 1;
            }
        }
        if closed > 0 {
            return Err(format!("{closed} SAI stats recipient(s) closed").into());
        }
        Ok(())
    }

    async fn send_batch_and_drain_deferred(
        &mut self,
        batch: SAIStatsBatch,
    ) -> Result<(), IpfixError> {
        self.send_batch(batch).await?;
        loop {
            let mut ready = SAIStatsBatch::default();
            self.drain_deferred_sets(&mut ready)?;
            if ready.is_empty() {
                return Ok(());
            }
            self.send_batch(ready).await?;
        }
    }

    pub async fn run(mut actor: IpfixActor) -> Result<(), IpfixError> {
        loop {
            select! {
                template = actor.template_recipient.recv() => match template {
                    Some(template) => {
                        record_comm_stats(
                            ChannelLabel::SwssToIpfixTemplates,
                            actor.template_recipient.len(),
                        );
                        match actor.handle_template(template) {
                            Ok(batch) => actor.send_batch_and_drain_deferred(batch).await?,
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

fn retire_canceled_pending_generation(
    pending: &TemplateGeneration,
    active: Option<&TemplateGeneration>,
    installed: &mut HashMap<TemplateKey, Arc<CompiledTemplate>>,
    retired: &mut RetiredTemplates,
) {
    for key in pending.templates.keys() {
        if !active.is_some_and(|generation| generation.templates.contains_key(key)) {
            if let Some(template) = installed.remove(key) {
                retired.retire(&template);
            }
        }
    }
}

fn retire_replaced_pending_generation(
    pending: &TemplateGeneration,
    replacement: &TemplateGeneration,
    active: Option<&TemplateGeneration>,
    installed: &mut HashMap<TemplateKey, Arc<CompiledTemplate>>,
    retired: &mut RetiredTemplates,
) {
    for key in pending.templates.keys() {
        if !active.is_some_and(|generation| generation.templates.contains_key(key))
            && !replacement.templates.contains_key(key)
        {
            if let Some(template) = installed.remove(key) {
                retired.retire(&template);
            }
        }
    }
}

fn validate_pending_replacement(
    replacement: &TemplateGeneration,
    pending: Option<&TemplateGeneration>,
    installed: &HashMap<TemplateKey, Arc<CompiledTemplate>>,
    retired: &RetiredTemplates,
) -> Result<(), IpfixError> {
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
        } else if retired.contains(&template.key) {
            retired.validate_reactivation(template)?;
        }
    }

    let fresh_keys = replacement
        .templates
        .keys()
        .filter(|key| !installed.contains_key(key) && !retired.contains(key))
        .count();
    if retired.keys.len() + installed.len() + fresh_keys > MAX_RETIRED_TEMPLATES {
        return Err(format!(
            "template key registry limit {} exceeded",
            MAX_RETIRED_TEMPLATES
        )
        .into());
    }
    Ok(())
}

fn validate_generation_reactivation(
    generation: &TemplateGeneration,
    retired: &RetiredTemplates,
) -> Result<(), IpfixError> {
    for template in generation.templates.values() {
        retired.validate_reactivation(template)?;
    }
    Ok(())
}

fn validate_registry_capacity(
    generation: &TemplateGeneration,
    installed: &HashMap<TemplateKey, Arc<CompiledTemplate>>,
    retired: &RetiredTemplates,
) -> Result<(), IpfixError> {
    let fresh_keys = generation
        .templates
        .keys()
        .filter(|key| !installed.contains_key(key) && !retired.contains(key))
        .count();
    if retired.keys.len() + installed.len() + fresh_keys > MAX_RETIRED_TEMPLATES {
        return Err(format!(
            "template key registry limit {} exceeded",
            MAX_RETIRED_TEMPLATES
        )
        .into());
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

fn validate_data_set(template: &CompiledTemplate, set: &[u8]) -> Result<(), IpfixError> {
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
    if padding.len() > 3 || padding.iter().any(|byte| *byte != 0) {
        return Err(format!(
            "data set {} has {} invalid trailing bytes",
            template.key.template_id,
            padding.len()
        )
        .into());
    }
    Ok(())
}

fn data_set_counter_count(template: &CompiledTemplate, set: &[u8]) -> Result<usize, IpfixError> {
    validate_data_set(template, set)?;
    let record_count = (set.len() - SET_HEADER_LEN) / template.record_len;
    record_count
        .checked_mul(template.counters.len())
        .ok_or_else(|| "decoded counter count overflow".into())
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
        if set.len() - offset < 4 {
            if set[offset..].iter().all(|byte| *byte == 0) {
                break;
            }
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
        for field_index in 0..field_count {
            if set.len() - offset < 4 {
                return Err(format!("template {template_id} has a truncated field").into());
            }
            let raw_id = NetworkEndian::read_u16(&set[offset..offset + 2]);
            let field_len = NetworkEndian::read_u16(&set[offset + 2..offset + 4]);
            offset += 4;
            if field_len != HFT_FIELD_LEN {
                return Err(format!(
                    "template {template_id} field {} has length {field_len}; HFT requires 8",
                    raw_id & 0x7fff
                )
                .into());
            }
            let enterprise = raw_id & 0x8000 != 0;
            let field_id = raw_id & 0x7fff;
            if enterprise {
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
                    offset: field_index * HFT_FIELD_LEN as usize,
                    object_name: Arc::clone(object_name),
                    type_id,
                    stat_id,
                });
            } else if field_id == OBSERVATION_TIME_NANOSECONDS {
                if !field_keys.insert((field_id, None)) {
                    return Err(format!(
                        "template {template_id} contains duplicate observation time fields"
                    )
                    .into());
                }
                observation_time_offset = Some(field_index * HFT_FIELD_LEN as usize);
                observation_fields += 1;
            } else {
                return Err(format!(
                    "template {template_id} contains unsupported standard IE {field_id}"
                )
                .into());
            }
        }
        if observation_fields != 1 || counters.is_empty() {
            return Err(format!(
                "template {template_id} requires exactly one observation time and at least one counter"
            )
            .into());
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
            record_len: field_count * HFT_FIELD_LEN as usize,
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
        message.extend_from_slice(&HFT_FIELD_LEN.to_be_bytes());
        for (label, enterprise) in fields {
            message.extend_from_slice(&(0x8000 | label).to_be_bytes());
            message.extend_from_slice(&HFT_FIELD_LEN.to_be_bytes());
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
    fn malformed_update_retains_active_generation() {
        let mut actor = actor();
        let valid = template_message("session", 0, 300, &[(1, 0x0001_0002)]);
        actor.handle_template(valid.clone()).unwrap();
        let mut invalid = valid;
        invalid.templates = Some(Arc::new(vec![0, 10, 0, 0]));
        assert!(actor.handle_template(invalid).is_err());
        assert_eq!(actor.installed.len(), 1);
        assert_eq!(
            actor.sessions["session"]
                .active
                .as_ref()
                .unwrap()
                .templates
                .len(),
            1
        );
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
    fn rejects_ambiguous_same_id_schema_change() {
        let mut actor = actor();
        actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0002)]))
            .unwrap();
        assert!(actor
            .handle_template(template_message("session", 0, 300, &[(1, 0x0001_0003)]))
            .is_err());
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
        assert!(actor.retired.contains(&TemplateKey {
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
    fn rejected_supersession_retains_pending_generation() {
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
        assert!(actor.sessions["session"]
            .pending
            .as_ref()
            .unwrap()
            .templates
            .contains_key(&TemplateKey {
                observation_domain_id: 0,
                template_id: 301,
            }));
        assert!(actor.installed.contains_key(&TemplateKey {
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
        assert!(!actor.retired.contains(&TemplateKey {
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
        let pending = actor.sessions["session"].pending.as_ref().unwrap();
        assert_eq!(
            pending.templates[&TemplateKey {
                observation_domain_id: 0,
                template_id: 301,
            }]
                .counters[0]
                .stat_id,
            3
        );
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
        assert!(actor.retired.contains(&TemplateKey {
            observation_domain_id: 0,
            template_id: 300,
        }));
        assert!(actor.retired.contains(&TemplateKey {
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
    fn delete_tombstone_drops_late_data_and_rejects_changed_schema_reuse() {
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
                    &[(1, 0x0001_0002)],
                ))
                .is_err());
        }
        assert!(actor.sessions.is_empty());
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
    fn rejects_non_hft_fields_and_bad_object_ids() {
        let mut message = template_message("session", 0, 300, &[(1, 0x0001_0002)]);
        message.object_ids = Some(vec![0]);
        assert!(IpfixActor::compile_generation(&message).is_err());

        let bytes = Arc::make_mut(message.templates.as_mut().unwrap());
        bytes[24..26].copy_from_slice(&322u16.to_be_bytes());
        message.object_ids = Some(vec![1]);
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
        assert!(error.to_string().contains("SAI stats recipient closed"));
        assert_eq!(healthy_rx.recv().await.unwrap().record_count(), 1);
    }
}
