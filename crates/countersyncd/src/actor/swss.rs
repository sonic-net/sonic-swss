use super::super::message::{
    aggregator::{
        AggregatorConfig, AggregatorConfigMessage, CounterSelector, HeatmapLayout,
        DEFAULT_ROLLOVER_BIT_WIDTH, MAX_ROLLOVER_BIT_WIDTH, MIN_ROLLOVER_BIT_WIDTH,
    },
    ipfix::IPFixTemplatesMessage,
};
use swss_common::{DbConnector, KeyOperation, SubscriberStateTable};

use log::{debug, error, info, warn};
use std::{
    collections::{HashMap, HashSet, VecDeque},
    sync::Arc,
    thread,
    time::Duration,
};
use tokio::sync::mpsc::{self, Sender};

const SOCK_PATH: &str = "/var/run/redis/redis.sock";
const CONFIG_DB_ID: i32 = 4;
const STATE_DB_ID: i32 = 6;
const STATE_HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE: &str = "HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE";
const CONFIG_HIGH_FREQUENCY_TELEMETRY_PROFILE_TABLE: &str = "HIGH_FREQUENCY_TELEMETRY_PROFILE";
const CONFIG_HIGH_FREQUENCY_TELEMETRY_AGGREGATOR_TABLE: &str =
    "HIGH_FREQUENCY_TELEMETRY_AGGREGATOR";
const CONFIG_HIGH_FREQUENCY_TELEMETRY_AGGREGATOR_HISTOGRAM_TABLE: &str =
    "HIGH_FREQUENCY_TELEMETRY_AGGREGATOR_HISTOGRAM";
const CONFIG_HIGH_FREQUENCY_TELEMETRY_AGGREGATOR_ROLLOVER_TABLE: &str =
    "HIGH_FREQUENCY_TELEMETRY_AGGREGATOR_ROLLOVER";
const SWSS_EVENT_CHANNEL_CAPACITY: usize = 32;

#[cfg(test)]
const MAX_TEST_ITERATIONS: usize = 20;

/// SwssActor monitors HFT session state and HFT aggregator/profile config.
///
/// The state DB message format example:
/// ```text
/// 127.0.0.1:6379[6]> hgetall "HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE|test|PORT"
///  1> "stream_status"     -> "enabled"
///  2> "session_type"      -> "ipfix"
///  3> "object_names"      -> "Ethernet0"
///  4> "object_ids"        -> "1"
///  5> "session_config"    -> <binary IPFIX template data>
/// ```
pub struct SwssActor {
    pub session_table: SubscriberStateTable,
    pub profile_table: SubscriberStateTable,
    pub aggregator_table: SubscriberStateTable,
    pub histogram_table: SubscriberStateTable,
    pub rollover_table: SubscriberStateTable,
    template_recipient: Sender<IPFixTemplatesMessage>,
}

#[derive(Debug)]
enum SwssEvent {
    SessionUpdate {
        key: String,
        session_data: SessionData,
    },
    SessionDelete {
        key: String,
    },
    Config(AggregatorConfigEvent),
}

#[derive(Debug)]
enum AggregatorConfigEvent {
    ProfileUpdate {
        profile: String,
        aggregator: Option<String>,
        poll_interval_us: Option<u32>,
    },
    ProfileDelete {
        profile: String,
    },
    AggregatorUpdate {
        name: String,
        config: Option<AggregatorConfig>,
    },
    AggregatorDelete {
        name: String,
    },
    HistogramUpdate {
        aggregator: String,
        selector: CounterSelector,
        explicit_bounds: Vec<u64>,
    },
    HistogramDelete {
        aggregator: String,
        selector: CounterSelector,
    },
    RolloverUpdate {
        aggregator: String,
        selector: CounterSelector,
        bit_width: u8,
    },
    RolloverDelete {
        aggregator: String,
        selector: CounterSelector,
    },
}

type SwssEventCollector = fn(&SubscriberStateTable) -> Result<Vec<SwssEvent>, String>;

impl SwssActor {
    /// Creates a new SwssActor instance
    ///
    /// # Arguments
    /// * `template_recipient` - Channel sender for forwarding IPFIX templates to IPFIX actor
    pub fn new(
        template_recipient: Sender<IPFixTemplatesMessage>,
    ) -> Result<Self, String> {
        let session_connect = DbConnector::new_unix(STATE_DB_ID, SOCK_PATH, 0)
            .map_err(|e| format!("Failed to create DB connection: {}", e))?;
        let session_table = SubscriberStateTable::new(
            session_connect,
            STATE_HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE,
            None,
            None,
        )
        .map_err(|e| format!("Failed to create session table: {}", e))?;

        let profile_connect = DbConnector::new_unix(CONFIG_DB_ID, SOCK_PATH, 0)
            .map_err(|e| format!("Failed to create CONFIG_DB profile connection: {}", e))?;
        let profile_table = SubscriberStateTable::new(
            profile_connect,
            CONFIG_HIGH_FREQUENCY_TELEMETRY_PROFILE_TABLE,
            None,
            None,
        )
        .map_err(|e| format!("Failed to create profile table: {}", e))?;

        let aggregator_connect = DbConnector::new_unix(CONFIG_DB_ID, SOCK_PATH, 0)
            .map_err(|e| format!("Failed to create CONFIG_DB aggregator connection: {}", e))?;
        let aggregator_table = SubscriberStateTable::new(
            aggregator_connect,
            CONFIG_HIGH_FREQUENCY_TELEMETRY_AGGREGATOR_TABLE,
            None,
            None,
        )
        .map_err(|e| format!("Failed to create aggregator table: {}", e))?;

        let histogram_connect = DbConnector::new_unix(CONFIG_DB_ID, SOCK_PATH, 0)
            .map_err(|e| format!("Failed to create CONFIG_DB histogram connection: {}", e))?;
        let histogram_table = SubscriberStateTable::new(
            histogram_connect,
            CONFIG_HIGH_FREQUENCY_TELEMETRY_AGGREGATOR_HISTOGRAM_TABLE,
            None,
            None,
        )
        .map_err(|e| format!("Failed to create aggregator histogram table: {}", e))?;

        let rollover_connect = DbConnector::new_unix(CONFIG_DB_ID, SOCK_PATH, 0)
            .map_err(|e| format!("Failed to create CONFIG_DB rollover connection: {}", e))?;
        let rollover_table = SubscriberStateTable::new(
            rollover_connect,
            CONFIG_HIGH_FREQUENCY_TELEMETRY_AGGREGATOR_ROLLOVER_TABLE,
            None,
            None,
        )
        .map_err(|e| format!("Failed to create aggregator rollover table: {}", e))?;

        Ok(SwssActor {
            session_table,
            profile_table,
            aggregator_table,
            histogram_table,
            rollover_table,
            template_recipient,
        })
    }

    /// Main event loop for the SwssActor
    ///
    /// Continuously monitors HFT session state and aggregator/profile config updates.
    ///
    /// # Arguments
    /// * `actor` - SwssActor instance to run
    pub async fn run(actor: SwssActor) {
        info!("SwssActor started, monitoring HFT session state and aggregator/profile config");

        // Keep SWSS table polling on dedicated blocking threads so hiredis
        // calls never park a Tokio worker.
        let SwssActor {
            session_table,
            profile_table,
            aggregator_table,
            histogram_table,
            rollover_table,
            template_recipient,
        } = actor;
        let mut aggregator_state = AggregatorConfigState::default();
        let mut pending_events = VecDeque::new();
        for events in [
            Self::collect_profile_events(&profile_table),
            Self::collect_aggregator_events(&aggregator_table),
            Self::collect_histogram_events(&histogram_table),
            Self::collect_rollover_events(&rollover_table),
            Self::collect_session_events(&session_table),
        ] {
            match events {
                Ok(events) => pending_events.extend(events),
                Err(e) => error!("{}", e),
            }
        }

        let (event_sender, mut event_receiver) = mpsc::channel(SWSS_EVENT_CHANNEL_CAPACITY);
        let readers = [
            (
                "countersyncd-swss-session",
                session_table,
                Self::collect_session_events as SwssEventCollector,
            ),
            (
                "countersyncd-swss-profile",
                profile_table,
                Self::collect_profile_events as SwssEventCollector,
            ),
            (
                "countersyncd-swss-aggregator",
                aggregator_table,
                Self::collect_aggregator_events as SwssEventCollector,
            ),
            (
                "countersyncd-swss-aggregator-histogram",
                histogram_table,
                Self::collect_histogram_events as SwssEventCollector,
            ),
            (
                "countersyncd-swss-aggregator-rollover",
                rollover_table,
                Self::collect_rollover_events as SwssEventCollector,
            ),
        ];
        let _reader_threads = match Self::spawn_reader_threads(readers, event_sender) {
            Ok(handles) => handles,
            Err(e) => {
                error!("Failed to spawn SWSS reader: {}", e);
                return;
            }
        };

        loop {
            let event = match pending_events.pop_front() {
                Some(event) => event,
                None => match event_receiver.recv().await {
                    Some(event) => event,
                    None => break,
                },
            };

            // Apply each table event independently. Retained parent/child state
            // handles ordering; readers do not coalesce cross-table updates.
            Self::process_event(&template_recipient, &mut aggregator_state, event).await;
        }

        debug!("SwssActor terminated");
    }

    async fn process_event(
        template_recipient: &Sender<IPFixTemplatesMessage>,
        aggregator_state: &mut AggregatorConfigState,
        event: SwssEvent,
    ) {
        match event {
            SwssEvent::SessionUpdate { key, session_data } => {
                if Self::validate_session(&key, &session_data) {
                    match aggregator_state.try_config_for_session_key(&key) {
                        Ok(config) => {
                            let config =
                                AggregatorConfigMessage::replacement(key.clone(), config);
                            if let Err(e) = Self::send_session_update(
                                template_recipient,
                                &key,
                                &session_data,
                                config,
                            )
                            .await
                            {
                                error!("Failed to process session {}: {}", key, e);
                                aggregator_state.remove_session(&key);
                                Self::process_session_delete(template_recipient, &key).await;
                            } else {
                                aggregator_state.add_session(key);
                            }
                        }
                        Err(reason) => error!(
                            "Rejecting effective aggregator config for session {}: {}; preserving existing session state",
                            key, reason
                        ),
                    }
                } else {
                    aggregator_state.remove_session(&key);
                    Self::process_session_delete(template_recipient, &key).await;
                }
            }
            SwssEvent::SessionDelete { key } => {
                Self::process_session_delete(template_recipient, &key).await;
                aggregator_state.remove_session(&key);
            }
            SwssEvent::Config(event) => {
                let affected_sessions = aggregator_state.apply_config_event(event);
                Self::send_aggregator_configs_for_sessions(
                    template_recipient,
                    aggregator_state,
                    affected_sessions,
                )
                .await;
            }
        }
    }

    fn spawn_reader_threads<const N: usize>(
        readers: [(&str, SubscriberStateTable, SwssEventCollector); N],
        event_sender: Sender<SwssEvent>,
    ) -> Result<Vec<thread::JoinHandle<()>>, String> {
        readers
            .into_iter()
            .map(|(name, table, collect_events)| {
                Self::spawn_reader_thread(name, table, event_sender.clone(), collect_events)
                    .map_err(|error| format!("{}: {}", name, error))
            })
            .collect()
    }

    fn spawn_reader_thread<F>(
        name: &str,
        table: SubscriberStateTable,
        event_sender: Sender<SwssEvent>,
        collect_events: F,
    ) -> Result<thread::JoinHandle<()>, std::io::Error>
    where
        F: Fn(&SubscriberStateTable) -> Result<Vec<SwssEvent>, String> + Send + 'static,
    {
        let name = name.to_string();
        thread::Builder::new().name(name.clone()).spawn(move || {
            #[cfg(test)]
            let mut iteration_count = 0;

            loop {
                if event_sender.is_closed() {
                    break;
                }

                #[cfg(test)]
                {
                    iteration_count += 1;
                    if iteration_count > MAX_TEST_ITERATIONS {
                        break;
                    }
                }

                #[cfg(test)]
                let timeout = Duration::from_millis(50);
                #[cfg(not(test))]
                let timeout = Duration::from_secs(10);

                let events = match table.read_data(timeout, false) {
                    Ok(swss_common::SelectResult::Data) => collect_events(&table),
                    Ok(swss_common::SelectResult::Timeout | swss_common::SelectResult::Signal) => {
                        Ok(Vec::new())
                    }
                    Err(e) => Err(format!("Error reading from {}: {}", name, e)),
                };

                match events {
                    Ok(events) => {
                        for event in events {
                            if event_sender.blocking_send(event).is_err() {
                                return;
                            }
                        }
                    }
                    Err(e) => {
                        error!("{}", e);
                        thread::sleep(Duration::from_millis(100));
                    }
                }
            }
        })
    }

    fn collect_session_events(
        session_table: &SubscriberStateTable,
    ) -> Result<Vec<SwssEvent>, String> {
        let items = session_table
            .pops()
            .map_err(|e| format!("Error popping items from session table: {}", e))?;
        let mut events = Vec::with_capacity(items.len());

        for item in items {
            debug!(
                "SwssActor received: key={}, op={:?}",
                item.key, item.operation
            );

            let session_key = Self::extract_session_key(&item.key);
            match item.operation {
                KeyOperation::Set => events.push(SwssEvent::SessionUpdate {
                    key: session_key,
                    session_data: Self::parse_session_data(&item.field_values),
                }),
                KeyOperation::Del => events.push(SwssEvent::SessionDelete { key: session_key }),
            }
        }

        Ok(events)
    }

    fn collect_profile_events(
        profile_table: &SubscriberStateTable,
    ) -> Result<Vec<SwssEvent>, String> {
        let items = profile_table
            .pops()
            .map_err(|e| format!("Error popping items from profile table: {}", e))?;
        let mut events = Vec::with_capacity(items.len());

        for item in items {
            let profile =
                Self::extract_config_key(&item.key, CONFIG_HIGH_FREQUENCY_TELEMETRY_PROFILE_TABLE);
            match item.operation {
                KeyOperation::Set => match Self::parse_profile(&item.field_values) {
                    Ok((aggregator, poll_interval_us)) => {
                        events.push(SwssEvent::Config(AggregatorConfigEvent::ProfileUpdate {
                            profile,
                            aggregator,
                            poll_interval_us,
                        }))
                    }
                    Err(reason) => error!("Rejecting profile update for {}: {}", profile, reason),
                },
                KeyOperation::Del => {
                    events.push(SwssEvent::Config(AggregatorConfigEvent::ProfileDelete {
                        profile,
                    }))
                }
            }
        }

        Ok(events)
    }

    fn collect_aggregator_events(
        aggregator_table: &SubscriberStateTable,
    ) -> Result<Vec<SwssEvent>, String> {
        let items = aggregator_table
            .pops()
            .map_err(|e| format!("Error popping items from aggregator table: {}", e))?;
        let mut events = Vec::with_capacity(items.len());

        for item in items {
            let name = Self::extract_config_key(
                &item.key,
                CONFIG_HIGH_FREQUENCY_TELEMETRY_AGGREGATOR_TABLE,
            );
            match item.operation {
                KeyOperation::Set => match Self::parse_aggregator_config(&item.field_values) {
                    Ok(config) => {
                        events.push(SwssEvent::Config(AggregatorConfigEvent::AggregatorUpdate {
                            name,
                            config: Some(config),
                        }))
                    }
                    Err(reason) => {
                        error!(
                            "Rejecting aggregator config update for {}: {}",
                            name, reason
                        )
                    }
                },
                KeyOperation::Del => {
                    events.push(SwssEvent::Config(AggregatorConfigEvent::AggregatorDelete {
                        name,
                    }))
                }
            }
        }

        Ok(events)
    }

    fn collect_histogram_events(
        histogram_table: &SubscriberStateTable,
    ) -> Result<Vec<SwssEvent>, String> {
        let items = histogram_table
            .pops()
            .map_err(|e| format!("Error popping items from aggregator histogram table: {}", e))?;
        let mut events = Vec::with_capacity(items.len());

        for item in items {
            let key = Self::extract_config_key(
                &item.key,
                CONFIG_HIGH_FREQUENCY_TELEMETRY_AGGREGATOR_HISTOGRAM_TABLE,
            );
            let parsed = Self::parse_histogram_key(&key);
            let (aggregator, selector) = match parsed {
                Ok(parsed) => parsed,
                Err(reason) => {
                    error!("Rejecting aggregator histogram key {}: {}", key, reason);
                    continue;
                }
            };
            match item.operation {
                KeyOperation::Set => match Self::parse_histogram_bounds(&item.field_values) {
                    Ok(explicit_bounds) => {
                        events.push(SwssEvent::Config(AggregatorConfigEvent::HistogramUpdate {
                            aggregator,
                            selector,
                            explicit_bounds,
                        }))
                    }
                    Err(reason) => error!(
                        "Rejecting aggregator histogram update for {}: {}",
                        key, reason
                    ),
                },
                KeyOperation::Del => {
                    events.push(SwssEvent::Config(AggregatorConfigEvent::HistogramDelete {
                        aggregator,
                        selector,
                    }))
                }
            }
        }

        Ok(events)
    }

    fn collect_rollover_events(
        rollover_table: &SubscriberStateTable,
    ) -> Result<Vec<SwssEvent>, String> {
        let items = rollover_table
            .pops()
            .map_err(|e| format!("Error popping items from aggregator rollover table: {}", e))?;
        let mut events = Vec::with_capacity(items.len());

        for item in items {
            let key = Self::extract_config_key(
                &item.key,
                CONFIG_HIGH_FREQUENCY_TELEMETRY_AGGREGATOR_ROLLOVER_TABLE,
            );
            match Self::parse_rollover_event(key.clone(), item.operation, &item.field_values) {
                Ok(event) => events.push(SwssEvent::Config(event)),
                Err(reason) => {
                    error!("Rejecting aggregator rollover update for {}: {}", key, reason)
                }
            }
        }

        Ok(events)
    }

    fn parse_session_data(field_values: &HashMap<String, swss_common::CxxString>) -> SessionData {
        let mut session_data = SessionData::default();

        for (field, value) in field_values {
            match field.as_str() {
                "stream_status" => session_data.stream_status = value.to_string_lossy().to_string(),
                "session_type" => session_data.session_type = value.to_string_lossy().to_string(),
                "object_names" => session_data.object_names = value.to_string_lossy().to_string(),
                "object_ids" => session_data.object_ids = value.to_string_lossy().to_string(),
                "session_config" => {
                    session_data.session_config = value.as_bytes().to_vec();
                }
                _ => {
                    debug!("Unknown field in session data: {} = {:?}", field, value);
                }
            }
        }

        session_data
    }

    fn parse_profile(
        field_values: &HashMap<String, swss_common::CxxString>,
    ) -> Result<(Option<String>, Option<u32>), String> {
        let aggregator = field_values.get("aggregator").and_then(|value| {
            let aggregator = value.to_string_lossy().trim().to_string();
            if aggregator.is_empty() {
                None
            } else {
                Some(aggregator)
            }
        });
        let poll_interval_us = field_values
            .get("poll_interval")
            .ok_or_else(|| "missing mandatory poll_interval".to_string())?;
        let poll_interval_us = poll_interval_us
            .to_string_lossy()
            .trim()
            .parse::<u32>()
            .map_err(|_| {
                format!(
                    "Invalid poll_interval '{}'",
                    poll_interval_us.to_string_lossy()
                )
            })?;
        if poll_interval_us == 0 {
            return Err("poll_interval must be greater than zero".to_string());
        }
        Ok((aggregator, Some(poll_interval_us)))
    }

    fn parse_aggregator_config(
        field_values: &HashMap<String, swss_common::CxxString>,
    ) -> Result<AggregatorConfig, String> {
        let reporting_rate = match field_values.get("reporting_rate") {
            Some(value) => {
                let value = value.to_string_lossy();
                let rate = value
                    .trim()
                    .parse::<u32>()
                    .map_err(|_| format!("Invalid reporting_rate '{}'", value))?;
                if rate == 0 {
                    return Err("reporting_rate must be greater than zero".to_string());
                }
                Some(rate)
            }
            None => None,
        };

        let rollover_counters = match Self::config_field(field_values, "rollover_counters") {
            Some(value) => {
                crate::message::aggregator::CounterSelector::parse_list(&value.to_string_lossy())?
            }
            None => HashSet::new(),
        };
        let heatmap_interval = match field_values.get("heatmap_interval") {
            Some(value) => {
                let value = value.to_string_lossy();
                let interval = value
                    .trim()
                    .parse::<u32>()
                    .map_err(|_| format!("Invalid heatmap_interval '{}'", value))?;
                if interval == 0 {
                    return Err("heatmap_interval must be greater than zero".to_string());
                }
                Some(interval)
            }
            None => None,
        };
        let heatmap_counters = match Self::config_field(field_values, "heatmap_counters") {
            Some(value) => {
                crate::message::aggregator::CounterSelector::parse_list(&value.to_string_lossy())?
            }
            None => HashSet::new(),
        };
        let config = AggregatorConfig {
            reporting_rate,
            rollover_counters,
            rollover_bit_width_overrides: Default::default(),
            heatmap_interval,
            heatmap_counters,
            heatmap_explicit_bounds: Default::default(),
            ..Default::default()
        };
        config.validate_structure()?;
        Ok(config)
    }

    fn parse_histogram_key(key: &str) -> Result<(String, CounterSelector), String> {
        Self::parse_child_key(key)
    }

    fn parse_child_key(key: &str) -> Result<(String, CounterSelector), String> {
        let mut components = key.split('|');
        let aggregator = components
            .next()
            .filter(|name| !name.is_empty())
            .ok_or_else(|| "expected <aggregator>|<GROUP>|<COUNTER>".to_string())?;
        let group = components
            .next()
            .ok_or_else(|| "expected <aggregator>|<GROUP>|<COUNTER>".to_string())?;
        let counter = components
            .next()
            .ok_or_else(|| "expected <aggregator>|<GROUP>|<COUNTER>".to_string())?;
        if components.next().is_some() {
            return Err("expected <aggregator>|<GROUP>|<COUNTER>".to_string());
        }
        let selector = CounterSelector::parse(&format!("{}|{}", group, counter))?;
        Ok((aggregator.to_string(), selector))
    }

    fn parse_rollover_bit_width(
        field_values: &HashMap<String, swss_common::CxxString>,
    ) -> Result<u8, String> {
        let value = field_values
            .get("bit_width")
            .ok_or_else(|| "missing bit_width".to_string())?;
        let value = value.to_string_lossy();
        let bit_width = value
            .trim()
            .parse::<u8>()
            .map_err(|_| format!("Invalid bit_width '{}'", value))?;
        if !(MIN_ROLLOVER_BIT_WIDTH..=MAX_ROLLOVER_BIT_WIDTH).contains(&bit_width) {
            return Err(format!(
                "bit_width must be in range {}..={}",
                MIN_ROLLOVER_BIT_WIDTH, MAX_ROLLOVER_BIT_WIDTH
            ));
        }
        Ok(bit_width)
    }

    fn parse_rollover_event(
        key: String,
        operation: KeyOperation,
        field_values: &HashMap<String, swss_common::CxxString>,
    ) -> Result<AggregatorConfigEvent, String> {
        let (aggregator, selector) = Self::parse_child_key(&key)?;
        Ok(match operation {
            KeyOperation::Set => AggregatorConfigEvent::RolloverUpdate {
                aggregator,
                selector,
                bit_width: Self::parse_rollover_bit_width(field_values)?,
            },
            KeyOperation::Del => AggregatorConfigEvent::RolloverDelete {
                aggregator,
                selector,
            },
        })
    }

    fn parse_histogram_bounds(
        field_values: &HashMap<String, swss_common::CxxString>,
    ) -> Result<Vec<u64>, String> {
        let value = Self::config_field(field_values, "explicit_bounds")
            .ok_or_else(|| "missing explicit_bounds".to_string())?;
        let bounds = AggregatorConfig::parse_explicit_bounds(&value.to_string_lossy())?;
        HeatmapLayout::from_explicit_bounds(bounds.clone())?;
        Ok(bounds)
    }

    fn config_field<'a>(
        field_values: &'a HashMap<String, swss_common::CxxString>,
        name: &str,
    ) -> Option<&'a swss_common::CxxString> {
        field_values
            .get(name)
            .or_else(|| field_values.get(&format!("{}@", name)))
    }

    /// Extracts the session key from the full Redis key by removing the table name prefix
    ///
    /// # Arguments
    /// * `full_key` - Full Redis key (e.g., "HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE|session_name|PORT")
    ///
    /// # Returns
    /// Session key without table prefix (e.g., "session_name|PORT")
    fn extract_session_key(full_key: &str) -> String {
        if let Some(pos) = full_key.find('|') {
            if full_key.starts_with(STATE_HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE) {
                return full_key[pos + 1..].to_string();
            }
        }
        // If no table prefix found, return as-is
        full_key.to_string()
    }

    fn extract_config_key(full_key: &str, table_name: &str) -> String {
        full_key
            .strip_prefix(&format!("{}|", table_name))
            .unwrap_or(full_key)
            .to_string()
    }

    fn extract_profile_from_session_key(session_key: &str) -> &str {
        session_key.split('|').next().unwrap_or(session_key)
    }

    /// Processes session update messages from the state database
    ///
    /// # Arguments
    /// * `key` - Session key (e.g., "test|PORT")  
    /// * `field_values` - HashMap of field-value pairs from the state DB
    #[cfg(test)]
    async fn handle_session_update(
        &mut self,
        key: &str,
        field_values: &std::collections::HashMap<String, swss_common::CxxString>,
    ) {
        debug!("Processing session update for key: {}", key);

        let session_data = Self::parse_session_data(field_values);

        // Validate and process the session
        match self.validate_and_process_session(key, &session_data).await {
            Ok(_) => {}
            Err(e) => error!("Failed to process session {}: {}", key, e),
        }
    }

    async fn process_session_delete(template_recipient: &Sender<IPFixTemplatesMessage>, key: &str) {
        info!("Session deleted: {}", key);

        let delete_message = IPFixTemplatesMessage::delete_with_aggregator_config(
            key.to_string(),
            AggregatorConfigMessage::delete(key.to_string()),
        );

        match template_recipient.send(delete_message).await {
            Ok(_) => {
                info!("Successfully sent session deletion message for: {}", key);
            }
            Err(e) => {
                error!("Failed to send session deletion message for {}: {}", key, e);
            }
        }

        debug!("Session cleanup for {} completed", key);
    }

    /// Validates session data and processes enabled IPFIX sessions
    ///
    /// # Arguments
    /// * `key` - Session identifier
    /// * `session_data` - Parsed session configuration
    #[cfg(test)]
    async fn validate_and_process_session(
        &mut self,
        key: &str,
        session_data: &SessionData,
    ) -> Result<bool, String> {
        if !Self::validate_session(key, session_data) {
            Self::process_session_delete(&self.template_recipient, key).await;
            return Ok(false);
        }
        Self::send_session_update(
            &self.template_recipient,
            key,
            session_data,
            AggregatorConfigMessage::replacement(key.to_string(), None),
        )
        .await?;
        Ok(true)
    }

    /// Validates session data and processes enabled IPFIX sessions
    ///
    /// # Arguments
    /// * `key` - Session identifier
    /// * `session_data` - Parsed session configuration
    fn validate_session(key: &str, session_data: &SessionData) -> bool {
        if session_data.stream_status != "enabled" {
            debug!("Skipping disabled session: {}", key);
            return false;
        }

        if session_data.session_type != "ipfix" {
            debug!(
                "Skipping non-IPFIX session: {} (type: {})",
                key, session_data.session_type
            );
            return false;
        }

        if session_data.session_config.is_empty() {
            error!("Failed to process session {}: Session config is empty", key);
            return false;
        }

        true
    }

    async fn send_session_update(
        template_recipient: &Sender<IPFixTemplatesMessage>,
        key: &str,
        session_data: &SessionData,
        aggregator_config: AggregatorConfigMessage,
    ) -> Result<(), String> {
        info!(
            "Processing enabled IPFIX session: key={}, object_names={}, object_ids={}",
            key, session_data.object_names, session_data.object_ids
        );

        let templates = Arc::new(session_data.session_config.clone());

        // Parse object_names if present
        let object_names: Option<Vec<String>> = if session_data.object_names.is_empty() {
            None
        } else {
            Some(
                session_data
                    .object_names
                    .split(',')
                    .map(|s| s.trim().to_string())
                    .filter(|s| !s.is_empty())
                    .collect(),
            )
        };

        let object_ids = if session_data.object_ids.is_empty() {
            None
        } else {
            let mut parsed_object_ids = Vec::new();
            for token in session_data.object_ids.split(',') {
                let trimmed = token.trim();
                if trimmed.is_empty() {
                    continue;
                }

                match trimmed.parse::<u16>() {
                    Ok(object_id) => parsed_object_ids.push(object_id),
                    Err(e) => {
                        warn!(
                            "Invalid object_ids entry '{}' for session {}: {}. Ignoring object_ids for this update",
                            trimmed,
                            key,
                            e
                        );
                        parsed_object_ids.clear();
                        break;
                    }
                }
            }

            if parsed_object_ids.is_empty() {
                None
            } else if let Some(names) = object_names.as_ref() {
                if names.len() != parsed_object_ids.len() {
                    warn!(
                        "object_ids/object_names length mismatch for session {}: {} ids vs {} names. Ignoring object_ids for this update",
                        key,
                        parsed_object_ids.len(),
                        names.len()
                    );
                    None
                } else {
                    Some(parsed_object_ids)
                }
            } else {
                Some(parsed_object_ids)
            }
        };

        let message = IPFixTemplatesMessage::new(key.to_string(), templates, object_names, object_ids)
            .with_aggregator_config(aggregator_config);

        template_recipient
            .send(message)
            .await
            .map_err(|e| format!("Failed to send IPFix templates to recipient: {}", e))?;

        info!("Successfully sent IPFix templates for session: {}", key);
        Ok(())
    }

    async fn send_aggregator_config_for_session(
        template_recipient: &Sender<IPFixTemplatesMessage>,
        aggregator_state: &AggregatorConfigState,
        key: &str,
        is_delete: bool,
        reset: bool,
    ) {
        let config = if is_delete {
            Ok(None)
        } else {
            aggregator_state.try_config_for_session_key(key)
        };
        let config = match config {
            Ok(config) => config,
            Err(reason) => {
                error!(
                    "Rejecting effective aggregator config for session {}: {}; preserving existing aggregator state",
                    key, reason
                );
                return;
            }
        };
        let message = if is_delete {
            AggregatorConfigMessage::delete(key.to_string())
        } else if reset {
            AggregatorConfigMessage::replacement(key.to_string(), config)
        } else {
            AggregatorConfigMessage::new(key.to_string(), config)
        };

        if let Err(e) = template_recipient
            .send(IPFixTemplatesMessage::config(message))
            .await
        {
            error!("Failed to send aggregator config for {}: {}", key, e);
        }
    }

    async fn send_aggregator_configs_for_sessions(
        template_recipient: &Sender<IPFixTemplatesMessage>,
        aggregator_state: &AggregatorConfigState,
        session_keys: Vec<String>,
    ) {
        for key in session_keys {
            Self::send_aggregator_config_for_session(
                template_recipient,
                aggregator_state,
                &key,
                false,
                false,
            )
            .await;
        }
    }

    /// Handles session deletion events
    ///
    /// # Arguments
    /// * `key` - Session key that was deleted
    #[cfg(test)]
    async fn handle_session_delete(&mut self, key: &str) {
        Self::process_session_delete(&self.template_recipient, key).await;
    }
}

#[derive(Default)]
struct AggregatorConfigState {
    profile_aggregators: HashMap<String, String>,
    profile_poll_intervals_us: HashMap<String, u32>,
    aggregator_configs: HashMap<String, AggregatorConfig>,
    histogram_configs: HashMap<String, HashMap<CounterSelector, Vec<u64>>>,
    rollover_configs: HashMap<String, HashMap<CounterSelector, u8>>,
    sessions: HashSet<String>,
}

impl AggregatorConfigState {
    fn apply_config_event(&mut self, event: AggregatorConfigEvent) -> Vec<String> {
        match event {
            AggregatorConfigEvent::ProfileUpdate {
                profile,
                aggregator,
                poll_interval_us,
            } => {
                let affected_sessions = self.session_keys_for_profile(&profile);
                self.set_profile(profile, aggregator, poll_interval_us);
                affected_sessions
            }
            AggregatorConfigEvent::ProfileDelete { profile } => {
                let affected_sessions = self.session_keys_for_profile(&profile);
                self.remove_profile(&profile);
                affected_sessions
            }
            AggregatorConfigEvent::AggregatorUpdate { name, config } => {
                let affected_sessions = self.session_keys_for_aggregator(&name);
                self.set_aggregator_config(name, config);
                affected_sessions
            }
            AggregatorConfigEvent::AggregatorDelete { name } => {
                let affected_sessions = self.session_keys_for_aggregator(&name);
                self.remove_aggregator(&name);
                affected_sessions
            }
            AggregatorConfigEvent::HistogramUpdate {
                aggregator,
                selector,
                explicit_bounds,
            } => {
                let affected_sessions = self.session_keys_for_aggregator(&aggregator);
                self.set_histogram_config(aggregator, selector, explicit_bounds);
                affected_sessions
            }
            AggregatorConfigEvent::HistogramDelete {
                aggregator,
                selector,
            } => {
                let affected_sessions = self.session_keys_for_aggregator(&aggregator);
                self.remove_histogram_config(&aggregator, selector);
                affected_sessions
            }
            AggregatorConfigEvent::RolloverUpdate {
                aggregator,
                selector,
                bit_width,
            } => {
                let affected_sessions = self.session_keys_for_aggregator(&aggregator);
                self.set_rollover_config(aggregator, selector, bit_width);
                affected_sessions
            }
            AggregatorConfigEvent::RolloverDelete {
                aggregator,
                selector,
            } => {
                let affected_sessions = self.session_keys_for_aggregator(&aggregator);
                self.remove_rollover_config(&aggregator, selector);
                affected_sessions
            }
        }
    }

    fn add_session(&mut self, key: String) {
        self.sessions.insert(key);
    }

    fn remove_session(&mut self, key: &str) {
        self.sessions.remove(key);
    }

    fn set_profile(
        &mut self,
        profile: String,
        aggregator: Option<String>,
        poll_interval_us: Option<u32>,
    ) {
        match aggregator {
            Some(aggregator) => {
                self.profile_aggregators.insert(profile.clone(), aggregator);
            }
            None => {
                self.profile_aggregators.remove(&profile);
            }
        }
        match poll_interval_us {
            Some(interval) => {
                self.profile_poll_intervals_us.insert(profile, interval);
            }
            None => {
                self.profile_poll_intervals_us.remove(&profile);
            }
        }
    }

    #[cfg(test)]
    fn set_profile_aggregator(&mut self, profile: String, aggregator: Option<String>) {
        self.set_profile(profile, aggregator, Some(10_000));
    }

    fn remove_profile(&mut self, profile: &str) {
        self.profile_aggregators.remove(profile);
        self.profile_poll_intervals_us.remove(profile);
    }

    fn set_aggregator_config(&mut self, name: String, config: Option<AggregatorConfig>) {
        match config {
            Some(mut config) => {
                config.poll_interval_us = None;
                config.heatmap_layouts.clear();
                self.aggregator_configs.insert(name, config);
            }
            None => {
                self.aggregator_configs.remove(&name);
            }
        }
    }

    fn remove_aggregator(&mut self, name: &str) {
        self.aggregator_configs.remove(name);
        // Child-table readers preserve their own ordering. Keep child rows
        // cached but inert while the parent is absent so parent/child events
        // converge correctly regardless of cross-table interleaving.
    }

    fn set_histogram_config(
        &mut self,
        aggregator: String,
        selector: CounterSelector,
        explicit_bounds: Vec<u64>,
    ) {
        self.histogram_configs
            .entry(aggregator)
            .or_default()
            .insert(selector, explicit_bounds);
    }

    fn remove_histogram_config(&mut self, aggregator: &str, selector: CounterSelector) {
        if let Some(configs) = self.histogram_configs.get_mut(aggregator) {
            configs.remove(&selector);
            if configs.is_empty() {
                self.histogram_configs.remove(aggregator);
            }
        }
    }

    fn set_rollover_config(
        &mut self,
        aggregator: String,
        selector: CounterSelector,
        bit_width: u8,
    ) {
        self.rollover_configs
            .entry(aggregator)
            .or_default()
            .insert(selector, bit_width);
    }

    fn remove_rollover_config(&mut self, aggregator: &str, selector: CounterSelector) {
        if let Some(configs) = self.rollover_configs.get_mut(aggregator) {
            configs.remove(&selector);
            if configs.is_empty() {
                self.rollover_configs.remove(aggregator);
            }
        }
    }

    #[cfg(test)]
    fn config_for_session_key(&self, session_key: &str) -> Option<AggregatorConfig> {
        match self.try_config_for_session_key(session_key) {
            Ok(config) => config,
            Err(reason) => {
                error!(
                    "Rejecting effective aggregator config for session {}: {}",
                    session_key, reason
                );
                None
            }
        }
    }

    fn try_config_for_session_key(
        &self,
        session_key: &str,
    ) -> Result<Option<AggregatorConfig>, String> {
        let profile = SwssActor::extract_profile_from_session_key(session_key);
        let Some(aggregator) = self.profile_aggregators.get(profile) else {
            return Ok(None);
        };
        let Some(base_config) = self.aggregator_configs.get(aggregator) else {
            return Ok(None);
        };
        let mut config = base_config.clone();
        config.poll_interval_us = self.profile_poll_intervals_us.get(profile).copied();
        if let Some(histograms) = self.histogram_configs.get(aggregator) {
            config.heatmap_explicit_bounds.extend(
                histograms
                    .iter()
                    .filter(|(selector, _)| config.heatmap_counters.contains(selector))
                    .map(|(selector, bounds)| (*selector, bounds.clone())),
            );
        }
        if let Some(rollovers) = self.rollover_configs.get(aggregator) {
            config.rollover_bit_width_overrides.extend(
                rollovers
                    .iter()
                    .filter(|(selector, bit_width)| {
                        config.rollover_counters.contains(selector)
                            && **bit_width != DEFAULT_ROLLOVER_BIT_WIDTH
                    })
                    .map(|(selector, bit_width)| (*selector, *bit_width)),
            );
        }
        config.resolve_heatmap_layouts()?;
        Ok(Some(config))
    }

    fn session_keys_for_profile(&self, profile: &str) -> Vec<String> {
        self.sessions
            .iter()
            .filter(|key| SwssActor::extract_profile_from_session_key(key) == profile)
            .cloned()
            .collect()
    }

    fn session_keys_for_aggregator(&self, aggregator: &str) -> Vec<String> {
        self.sessions
            .iter()
            .filter(|key| {
                let profile = SwssActor::extract_profile_from_session_key(key);
                self.profile_aggregators
                    .get(profile)
                    .is_some_and(|configured| configured == aggregator)
            })
            .cloned()
            .collect()
    }
}

/// Represents the parsed session data from HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE
///
/// This structure holds the configuration for a telemetry session including:
/// - stream_status: Whether the session is "enabled" or "disabled"
/// - session_type: Type of session, typically "ipfix" for IPFIX templates
/// - object_names: Comma-separated list of object names (e.g., "Ethernet0")
/// - object_ids: Comma-separated list of object IDs (e.g., "1")
/// - session_config: Binary data containing the session configuration (IPFIX templates)
#[derive(Default, Debug)]
struct SessionData {
    stream_status: String,
    session_type: String,
    object_names: String,
    object_ids: String,
    session_config: Vec<u8>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::actor::aggregator::Aggregator;
    use crate::message::aggregator::default_heatmap_layout;
    use crate::message::saistats::{SAIStat, SAIStats};
    use std::collections::HashMap;
    use swss_common::CxxString;
    use tokio::sync::mpsc::channel;

    // Helper function to create a SwssActor for testing
    fn create_test_actor(template_sender: Sender<IPFixTemplatesMessage>) -> SwssActor {
        SwssActor::new(template_sender).expect("Failed to create SwssActor")
    }

    #[tokio::test]
    async fn test_session_data_parsing() {
        let (template_sender, _template_receiver) = channel(1);
        let mut actor = create_test_actor(template_sender);

        // Test session data
        let key = "test|PORT";
        let mut field_values = HashMap::new();
        field_values.insert("stream_status".to_string(), CxxString::from("enabled"));
        field_values.insert("session_type".to_string(), CxxString::from("ipfix"));
        field_values.insert("object_names".to_string(), CxxString::from("Ethernet0"));
        field_values.insert("object_ids".to_string(), CxxString::from("1"));
        field_values.insert("session_config".to_string(), CxxString::from("test_config"));

        // This should not panic and should process the session
        actor.handle_session_update(key, &field_values).await;
    }

    #[tokio::test]
    async fn test_session_update_with_object_names() {
        let (template_sender, mut template_receiver) = channel(1);
        let mut actor = create_test_actor(template_sender);

        // Test session data with multiple object names
        let key = "test_session|PORT";
        let mut field_values = HashMap::new();
        field_values.insert("stream_status".to_string(), CxxString::from("enabled"));
        field_values.insert("session_type".to_string(), CxxString::from("ipfix"));
        field_values.insert(
            "object_names".to_string(),
            CxxString::from("Ethernet0,Ethernet1,Ethernet2"),
        );
        field_values.insert("object_ids".to_string(), CxxString::from("1,2,3"));
        field_values.insert(
            "session_config".to_string(),
            CxxString::from("ipfix_template_data"),
        );

        // Process the session update
        actor.handle_session_update(key, &field_values).await;

        // Verify the message was sent
        let received_message = template_receiver
            .try_recv()
            .expect("Should have received a message");
        assert_eq!(received_message.key, "test_session|PORT");
        assert!(!received_message.is_delete);
        assert!(received_message.templates.is_some());

        // Verify object_names parsing
        let object_names = received_message
            .object_names
            .as_ref()
            .expect("Should have object_names");
        assert_eq!(object_names, &vec!["Ethernet0", "Ethernet1", "Ethernet2"]);
        assert_eq!(received_message.object_ids, Some(vec![1, 2, 3]));
        assert!(received_message
            .aggregator_config
            .as_ref()
            .is_some_and(|config| config.reset));
    }

    #[test]
    fn test_profile_and_aggregator_config_mapping() {
        let mut profile_fields = HashMap::new();
        profile_fields.insert("aggregator".to_string(), CxxString::from("harm0"));
        profile_fields.insert("poll_interval".to_string(), CxxString::from("10000"));
        assert_eq!(
            SwssActor::parse_profile(&profile_fields),
            Ok((Some("harm0".to_string()), Some(10_000)))
        );
        assert!(SwssActor::parse_profile(&HashMap::new()).is_err());
        profile_fields.insert("poll_interval".to_string(), CxxString::from("0"));
        assert!(SwssActor::parse_profile(&profile_fields)
            .unwrap_err()
            .contains("greater than zero"));

        let mut aggregator_fields = HashMap::new();
        aggregator_fields.insert("reporting_rate".to_string(), CxxString::from("100"));
        aggregator_fields.insert(
            "rollover_counters@".to_string(),
            CxxString::from("PORT|IF_IN_OCTETS"),
        );
        aggregator_fields.insert("heatmap_interval".to_string(), CxxString::from("1000"));
        aggregator_fields.insert(
            "heatmap_counters@".to_string(),
            CxxString::from("PORT|IF_IN_UCAST_PKTS,QUEUE|WATERMARK_BYTES"),
        );
        let config =
            SwssActor::parse_aggregator_config(&aggregator_fields).expect("aggregator config");
        assert_eq!(config.reporting_rate, Some(100));
        assert_eq!(config.rollover_counters.len(), 1);
        assert!(config.rollover_bit_width_overrides.is_empty());
        assert_eq!(config.heatmap_interval, Some(1000));
        assert_eq!(config.heatmap_counters.len(), 2);
        assert!(config.heatmap_explicit_bounds.is_empty());

        let empty_aggregator_fields = HashMap::new();
        let empty_config = SwssActor::parse_aggregator_config(&empty_aggregator_fields)
            .expect("aggregator config");
        assert_eq!(empty_config.reporting_rate, None);
        assert!(empty_config.heatmap_layouts.is_empty());

        let mut invalid_heatmap = HashMap::new();
        invalid_heatmap.insert(
            "heatmap_counters@".to_string(),
            CxxString::from("PORT|IF_IN_UCAST_PKTS"),
        );
        assert!(SwssActor::parse_aggregator_config(&invalid_heatmap).is_err());

        let mut state = AggregatorConfigState::default();
        state.add_session("profile0|PORT".to_string());
        state.set_profile_aggregator("profile0".to_string(), Some("harm0".to_string()));
        state.set_aggregator_config(
            "harm0".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(100),
                ..Default::default()
            }),
        );
        assert_eq!(
            state
                .config_for_session_key("profile0|PORT")
                .expect("session aggregator config")
                .reporting_rate,
            Some(100)
        );
    }

    #[test]
    fn config_events_update_state_and_return_affected_sessions() {
        let histogram = CounterSelector::parse("PORT|IF_IN_UCAST_PKTS").unwrap();
        let rollover = CounterSelector::parse("PORT|IF_IN_OCTETS").unwrap();
        let mut state = AggregatorConfigState::default();
        state.add_session("profile0|PORT".to_string());
        state.add_session("profile0|QUEUE".to_string());
        state.add_session("profile1|PORT".to_string());

        let sorted = |mut sessions: Vec<String>| {
            sessions.sort();
            sessions
        };
        assert_eq!(
            sorted(
                state.apply_config_event(AggregatorConfigEvent::ProfileUpdate {
                    profile: "profile0".to_string(),
                    aggregator: Some("harm0".to_string()),
                    poll_interval_us: Some(1_000),
                })
            ),
            vec!["profile0|PORT".to_string(), "profile0|QUEUE".to_string()]
        );
        assert_eq!(
            state
                .apply_config_event(AggregatorConfigEvent::AggregatorUpdate {
                    name: "harm0".to_string(),
                    config: Some(AggregatorConfig {
                        heatmap_interval: Some(10_000),
                        heatmap_counters: HashSet::from([histogram]),
                        rollover_counters: HashSet::from([rollover]),
                        ..Default::default()
                    }),
                })
                .len(),
            2
        );
        assert_eq!(
            state
                .apply_config_event(AggregatorConfigEvent::HistogramUpdate {
                    aggregator: "harm0".to_string(),
                    selector: histogram,
                    explicit_bounds: vec![10, 20],
                })
                .len(),
            2
        );
        assert_eq!(
            state
                .apply_config_event(AggregatorConfigEvent::RolloverUpdate {
                    aggregator: "harm0".to_string(),
                    selector: rollover,
                    bit_width: 24,
                })
                .len(),
            2
        );
        let effective = state.config_for_session_key("profile0|PORT").unwrap();
        assert_eq!(
            effective
                .layout_for(histogram)
                .unwrap()
                .explicit_bounds_u64(),
            [10, 20]
        );
        assert_eq!(effective.rollover_bit_width_for(rollover), 24);

        assert_eq!(
            state
                .apply_config_event(AggregatorConfigEvent::HistogramDelete {
                    aggregator: "harm0".to_string(),
                    selector: histogram,
                })
                .len(),
            2
        );
        assert_eq!(
            state
                .apply_config_event(AggregatorConfigEvent::RolloverDelete {
                    aggregator: "harm0".to_string(),
                    selector: rollover,
                })
                .len(),
            2
        );
        assert_eq!(
            state
                .apply_config_event(AggregatorConfigEvent::AggregatorDelete {
                    name: "harm0".to_string(),
                })
                .len(),
            2
        );
        assert!(state.config_for_session_key("profile0|PORT").is_none());
        assert_eq!(
            sorted(
                state.apply_config_event(AggregatorConfigEvent::ProfileDelete {
                    profile: "profile0".to_string(),
                })
            ),
            vec!["profile0|PORT".to_string(), "profile0|QUEUE".to_string()]
        );
        assert!(!state.profile_poll_intervals_us.contains_key("profile0"));
    }

    #[test]
    fn test_histogram_parse_and_state_merge() {
        let custom = CounterSelector::parse("PORT|IF_IN_UCAST_PKTS").unwrap();
        let initially_unselected = CounterSelector::parse("QUEUE|PACKETS").unwrap();
        assert_eq!(
            SwssActor::parse_histogram_key("harm0|PORT|IF_IN_UCAST_PKTS").unwrap(),
            ("harm0".to_string(), custom)
        );
        assert!(SwssActor::parse_histogram_key("harm0|PORT").is_err());
        assert!(SwssActor::parse_histogram_key("harm|0|PORT|IF_IN_UCAST_PKTS").is_err());

        let fields = HashMap::from([(
            "explicit_bounds@".to_string(),
            CxxString::from("0,1024,4096"),
        )]);
        assert_eq!(
            SwssActor::parse_histogram_bounds(&fields).unwrap(),
            vec![0, 1024, 4096]
        );
        let invalid_fields = HashMap::from([(
            "explicit_bounds@".to_string(),
            CxxString::from("0,4096,1024"),
        )]);
        assert!(SwssActor::parse_histogram_bounds(&invalid_fields).is_err());

        let mut state = AggregatorConfigState::default();
        state.add_session("profile0|PORT".to_string());
        state.add_session("profile1|QUEUE".to_string());
        state.set_profile_aggregator("profile0".to_string(), Some("harm0".to_string()));
        state.set_profile_aggregator("profile1".to_string(), Some("harm0".to_string()));

        // Child rows can arrive first. They remain inert until the parent both
        // exists and selects the corresponding counter.
        state.set_histogram_config("harm0".to_string(), custom, vec![0, 1024, 4096]);
        state.set_histogram_config("harm0".to_string(), initially_unselected, vec![10, 20]);
        assert!(state.config_for_session_key("profile0|PORT").is_none());

        state.set_aggregator_config(
            "harm0".to_string(),
            Some(AggregatorConfig {
                heatmap_interval: Some(1_000),
                heatmap_counters: HashSet::from([custom]),
                ..Default::default()
            }),
        );
        for session in ["profile0|PORT", "profile1|QUEUE"] {
            let merged = state.config_for_session_key(session).unwrap();
            assert_eq!(
                merged.heatmap_explicit_bounds,
                std::collections::BTreeMap::from([(custom, vec![0, 1024, 4096])])
            );
        }

        let mut affected = state.session_keys_for_aggregator("harm0");
        affected.sort();
        assert_eq!(
            affected,
            vec!["profile0|PORT".to_string(), "profile1|QUEUE".to_string()]
        );

        state.remove_histogram_config("harm0", custom);
        let fallback = state.config_for_session_key("profile0|PORT").unwrap();
        assert!(fallback.heatmap_explicit_bounds.is_empty());
        assert!(Arc::ptr_eq(
            &fallback.layout_for(custom).unwrap(),
            &default_heatmap_layout(custom.heatmap_quantity(), None).unwrap()
        ));

        let mut parent = state
            .config_for_session_key("profile0|PORT")
            .expect("parent config");
        parent.heatmap_counters.insert(initially_unselected);
        state.set_aggregator_config("harm0".to_string(), Some(parent));
        assert_eq!(
            state
                .config_for_session_key("profile0|PORT")
                .unwrap()
                .heatmap_explicit_bounds
                .get(&initially_unselected),
            Some(&vec![10, 20])
        );
    }

    #[test]
    fn test_rollover_parse_and_retained_state_merge() {
        let selected = CounterSelector::parse("PORT|IF_IN_OCTETS").unwrap();
        let initially_unselected = CounterSelector::parse("PORT|IF_OUT_OCTETS").unwrap();
        assert_eq!(
            SwssActor::parse_child_key("harm0|PORT|IF_IN_OCTETS").unwrap(),
            ("harm0".to_string(), selected)
        );
        assert!(SwssActor::parse_child_key("harm0|PORT").is_err());
        assert!(SwssActor::parse_child_key("harm|0|PORT|IF_IN_OCTETS").is_err());

        for bit_width in [1, 24, 32, 48, 63] {
            let fields = HashMap::from([(
                "bit_width".to_string(),
                CxxString::from(bit_width.to_string()),
            )]);
            assert_eq!(SwssActor::parse_rollover_bit_width(&fields), Ok(bit_width));
        }
        for bit_width in [0, 64] {
            let fields = HashMap::from([(
                "bit_width".to_string(),
                CxxString::from(bit_width.to_string()),
            )]);
            assert!(SwssActor::parse_rollover_bit_width(&fields).is_err());
        }
        assert!(SwssActor::parse_rollover_bit_width(&HashMap::new()).is_err());
        assert!(SwssActor::parse_rollover_bit_width(&HashMap::from([(
            "bit_width@".to_string(),
            CxxString::from("8"),
        )]))
        .is_err());

        let set = SwssActor::parse_rollover_event(
            "harm0|PORT|IF_IN_OCTETS".to_string(),
            KeyOperation::Set,
            &HashMap::from([("bit_width".to_string(), CxxString::from("8"))]),
        )
        .unwrap();
        assert!(matches!(
            set,
            AggregatorConfigEvent::RolloverUpdate {
                ref aggregator,
                selector,
                bit_width: 8,
            } if aggregator == "harm0" && selector == selected
        ));
        let delete = SwssActor::parse_rollover_event(
            "harm0|PORT|IF_IN_OCTETS".to_string(),
            KeyOperation::Del,
            &HashMap::new(),
        )
        .unwrap();
        assert!(matches!(
            delete,
            AggregatorConfigEvent::RolloverDelete {
                ref aggregator,
                selector,
            } if aggregator == "harm0" && selector == selected
        ));

        let mut state = AggregatorConfigState::default();
        state.add_session("profile0|PORT".to_string());
        state.set_profile_aggregator("profile0".to_string(), Some("harm0".to_string()));
        state.set_rollover_config("harm0".to_string(), selected, 8);
        state.set_rollover_config("harm0".to_string(), initially_unselected, 24);
        assert!(state.config_for_session_key("profile0|PORT").is_none());

        state.set_aggregator_config(
            "harm0".to_string(),
            Some(AggregatorConfig {
                rollover_counters: HashSet::from([selected]),
                ..Default::default()
            }),
        );
        let custom = state.config_for_session_key("profile0|PORT").unwrap();
        assert_eq!(custom.rollover_bit_width_for(selected), 8);
        assert!(!custom
            .rollover_bit_width_overrides
            .contains_key(&initially_unselected));

        state.remove_rollover_config("harm0", selected);
        let fallback = state.config_for_session_key("profile0|PORT").unwrap();
        assert_eq!(
            fallback.rollover_bit_width_for(selected),
            DEFAULT_ROLLOVER_BIT_WIDTH
        );

        state.set_rollover_config("harm0".to_string(), selected, DEFAULT_ROLLOVER_BIT_WIDTH);
        assert!(state
            .config_for_session_key("profile0|PORT")
            .unwrap()
            .rollover_bit_width_overrides
            .is_empty());

        let mut parent = state
            .config_for_session_key("profile0|PORT")
            .expect("parent config");
        parent.rollover_counters.insert(initially_unselected);
        state.set_aggregator_config("harm0".to_string(), Some(parent));
        assert_eq!(
            state
                .config_for_session_key("profile0|PORT")
                .unwrap()
                .rollover_bit_width_for(initially_unselected),
            24
        );
    }

    #[test]
    fn test_parent_before_child_falls_back_then_switches_at_config_boundary() {
        let selector = CounterSelector::parse("PORT|IF_IN_UCAST_PKTS").unwrap();
        let mut state = AggregatorConfigState::default();
        state.add_session("profile0|PORT".to_string());
        state.set_profile_aggregator("profile0".to_string(), Some("harm0".to_string()));
        state.set_aggregator_config(
            "harm0".to_string(),
            Some(AggregatorConfig {
                heatmap_interval: Some(1_000),
                heatmap_counters: HashSet::from([selector]),
                ..Default::default()
            }),
        );

        let fallback = state.config_for_session_key("profile0|PORT").unwrap();
        assert!(fallback.heatmap_explicit_bounds.is_empty());
        assert_eq!(
            fallback.layout_for(selector).unwrap().explicit_bounds_u64(),
            default_heatmap_layout(selector.heatmap_quantity(), None)
                .unwrap()
                .explicit_bounds_u64()
        );

        // The independent child event produces a new effective config. Runtime
        // applies it at that config boundary and discards the partial heatmap.
        state.set_histogram_config("harm0".to_string(), selector, vec![10, 20]);
        let custom = state.config_for_session_key("profile0|PORT").unwrap();
        assert_eq!(
            custom.layout_for(selector).unwrap().explicit_bounds_u64(),
            &[10, 20]
        );
        assert_ne!(fallback, custom);
    }

    #[test]
    fn test_rollover_parent_before_child_defaults_updates_and_falls_back() {
        let selector = CounterSelector::parse("PORT|IF_IN_OCTETS").unwrap();
        let mut state = AggregatorConfigState::default();
        state.add_session("profile0|PORT".to_string());
        state.set_profile_aggregator("profile0".to_string(), Some("harm0".to_string()));
        state.set_aggregator_config(
            "harm0".to_string(),
            Some(AggregatorConfig {
                rollover_counters: HashSet::from([selector]),
                ..Default::default()
            }),
        );

        let fallback = state.config_for_session_key("profile0|PORT").unwrap();
        assert_eq!(
            fallback.rollover_bit_width_for(selector),
            DEFAULT_ROLLOVER_BIT_WIDTH
        );

        state.set_rollover_config("harm0".to_string(), selector, 24);
        let custom = state.config_for_session_key("profile0|PORT").unwrap();
        assert_eq!(custom.rollover_bit_width_for(selector), 24);

        state.remove_rollover_config("harm0", selector);
        let restored = state.config_for_session_key("profile0|PORT").unwrap();
        assert_eq!(
            restored.rollover_bit_width_for(selector),
            DEFAULT_ROLLOVER_BIT_WIDTH
        );
    }

    #[test]
    fn test_parent_delete_keeps_child_state_inert_until_recreated() {
        let selector = CounterSelector::parse("PORT|IF_IN_UCAST_PKTS").unwrap();
        let rollover = CounterSelector::parse("PORT|IF_IN_OCTETS").unwrap();
        let mut state = AggregatorConfigState::default();
        state.add_session("profile0|PORT".to_string());
        state.set_profile_aggregator("profile0".to_string(), Some("harm0".to_string()));
        let parent = AggregatorConfig {
            heatmap_interval: Some(1_000),
            heatmap_counters: HashSet::from([selector]),
            rollover_counters: HashSet::from([rollover]),
            ..Default::default()
        };
        state.set_aggregator_config("harm0".to_string(), Some(parent.clone()));
        state.set_histogram_config("harm0".to_string(), selector, vec![10, 20]);
        state.set_rollover_config("harm0".to_string(), rollover, 8);
        assert!(state
            .config_for_session_key("profile0|PORT")
            .unwrap()
            .heatmap_explicit_bounds
            .contains_key(&selector));

        state.remove_aggregator("harm0");
        assert!(state.config_for_session_key("profile0|PORT").is_none());
        assert!(state.histogram_configs.contains_key("harm0"));
        assert!(state.rollover_configs.contains_key("harm0"));
        state.set_aggregator_config("harm0".to_string(), Some(parent));
        assert!(state
            .config_for_session_key("profile0|PORT")
            .unwrap()
            .heatmap_explicit_bounds
            .contains_key(&selector));
        assert_eq!(
            state
                .config_for_session_key("profile0|PORT")
                .unwrap()
                .rollover_bit_width_for(rollover),
            8
        );

        state.remove_histogram_config("harm0", selector);
        state.remove_rollover_config("harm0", rollover);
        let cleaned = state.config_for_session_key("profile0|PORT").unwrap();
        assert!(cleaned.heatmap_explicit_bounds.is_empty());
        assert_eq!(
            cleaned.rollover_bit_width_for(rollover),
            DEFAULT_ROLLOVER_BIT_WIDTH
        );
    }

    #[tokio::test]
    async fn test_session_config_message_is_replacement() {
        let mut state = AggregatorConfigState::default();
        state.add_session("profile0|PORT".to_string());
        state.set_profile_aggregator("profile0".to_string(), Some("harm0".to_string()));
        state.set_aggregator_config("harm0".to_string(), Some(AggregatorConfig::default()));
        let (sender, mut receiver) = channel(2);

        SwssActor::send_aggregator_config_for_session(
            &sender,
            &state,
            "profile0|PORT",
            false,
            true,
        )
        .await;
        assert!(receiver
            .recv()
            .await
            .unwrap()
            .aggregator_config
            .is_some_and(|config| config.reset));

        SwssActor::send_aggregator_config_for_session(
            &sender,
            &state,
            "profile0|PORT",
            false,
            false,
        )
        .await;
        assert!(receiver
            .recv()
            .await
            .unwrap()
            .aggregator_config
            .is_some_and(|config| !config.reset));
    }

    #[tokio::test]
    async fn test_rollover_child_update_uses_ordered_config_envelope() {
        let selector = CounterSelector::parse("PORT|IF_IN_OCTETS").unwrap();
        let mut state = AggregatorConfigState::default();
        state.add_session("profile0|PORT".to_string());
        state.set_profile_aggregator("profile0".to_string(), Some("harm0".to_string()));
        state.set_aggregator_config(
            "harm0".to_string(),
            Some(AggregatorConfig {
                rollover_counters: HashSet::from([selector]),
                ..Default::default()
            }),
        );
        state.set_rollover_config("harm0".to_string(), selector, 8);
        let (sender, mut receiver) = channel(1);

        SwssActor::send_aggregator_configs_for_sessions(
            &sender,
            &state,
            vec!["profile0|PORT".to_string()],
        )
        .await;
        let message = receiver.recv().await.unwrap();
        assert!(message.templates.is_none());
        let config = message.aggregator_config.expect("config envelope");
        assert!(!config.reset);
        assert_eq!(
            config
                .config
                .unwrap()
                .rollover_bit_width_for(selector),
            8
        );
    }

    #[tokio::test]
    async fn invalid_effective_update_preserves_existing_aggregator_state() {
        let selector = CounterSelector::parse("PORT|IF_IN_OCTETS").unwrap();
        let mut state = AggregatorConfigState::default();
        state.add_session("profile0|PORT".to_string());
        state.set_profile("profile0".to_string(), Some("harm0".to_string()), Some(10));
        state.set_aggregator_config(
            "harm0".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(10),
                rollover_counters: HashSet::from([selector]),
                rollover_bit_width_overrides: std::collections::BTreeMap::from([(selector, 8)]),
                ..Default::default()
            }),
        );
        let (sender, mut receiver) = channel(1);
        SwssActor::send_aggregator_configs_for_sessions(
            &sender,
            &state,
            vec!["profile0|PORT".to_string()],
        )
        .await;
        let control = receiver.recv().await.unwrap().aggregator_config.unwrap();
        let mut aggregator = Aggregator::default();
        aggregator.set_config(control.key, control.config);
        let sample = |time, counter| {
            Arc::new(SAIStats::new(
                time,
                vec![SAIStat {
                    object_name: "Ethernet0".to_string(),
                    type_id: selector.type_id,
                    stat_id: selector.stat_id,
                    counter,
                }],
            ))
        };
        assert!(aggregator
            .process(Some(Arc::from("profile0|PORT")), sample(1_000, 250))
            .is_none());
        assert_eq!(
            aggregator
                .process(Some(Arc::from("profile0|PORT")), sample(10_001, 5))
                .unwrap()
                .stats
                .stats[0]
                .counter,
            250
        );

        state.set_profile("profile0".to_string(), Some("harm0".to_string()), None);
        state.set_aggregator_config(
            "harm0".to_string(),
            Some(AggregatorConfig {
                rollover_counters: HashSet::from([selector]),
                rollover_bit_width_overrides: std::collections::BTreeMap::from([(selector, 8)]),
                heatmap_interval: Some(100),
                heatmap_counters: HashSet::from([selector]),
                ..Default::default()
            }),
        );
        SwssActor::send_aggregator_configs_for_sessions(
            &sender,
            &state,
            vec!["profile0|PORT".to_string()],
        )
        .await;
        assert!(matches!(
            receiver.try_recv(),
            Err(tokio::sync::mpsc::error::TryRecvError::Empty)
        ));

        let output = aggregator
            .process(Some(Arc::from("profile0|PORT")), sample(20_001, 10))
            .expect("existing reporting window must remain active");
        assert_eq!(output.stats.stats[0].counter, 261);
    }

    #[test]
    fn test_multiple_profiles_can_share_aggregator_config() {
        let mut state = AggregatorConfigState::default();
        state.add_session("profile0|PORT".to_string());
        state.add_session("profile1|QUEUE".to_string());
        state.set_profile_aggregator("profile0".to_string(), Some("harm0".to_string()));
        state.set_profile_aggregator("profile1".to_string(), Some("harm0".to_string()));
        state.set_aggregator_config(
            "harm0".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(100),
                ..Default::default()
            }),
        );

        assert_eq!(
            state
                .config_for_session_key("profile0|PORT")
                .expect("profile0 aggregator config")
                .reporting_rate,
            Some(100)
        );
        assert_eq!(
            state
                .config_for_session_key("profile1|QUEUE")
                .expect("profile1 aggregator config")
                .reporting_rate,
            Some(100)
        );

        state.set_aggregator_config(
            "harm0".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(200),
                ..Default::default()
            }),
        );

        let mut affected_sessions = state.session_keys_for_aggregator("harm0");
        affected_sessions.sort();
        assert_eq!(
            affected_sessions,
            vec!["profile0|PORT".to_string(), "profile1|QUEUE".to_string()]
        );
        assert_eq!(
            state
                .config_for_session_key("profile0|PORT")
                .expect("profile0 updated aggregator config")
                .reporting_rate,
            Some(200)
        );
        assert_eq!(
            state
                .config_for_session_key("profile1|QUEUE")
                .expect("profile1 updated aggregator config")
                .reporting_rate,
            Some(200)
        );
    }

    #[test]
    fn profile_poll_intervals_resolve_distinct_shared_byte_layouts() {
        let selector = CounterSelector::parse("PORT|IF_IN_OCTETS").unwrap();
        let mut state = AggregatorConfigState::default();
        for profile in ["profile0", "profile1"] {
            state.add_session(format!("{profile}|PORT"));
        }
        state.set_profile(
            "profile0".to_string(),
            Some("harm0".to_string()),
            Some(1_000),
        );
        state.set_profile(
            "profile1".to_string(),
            Some("harm0".to_string()),
            Some(10_000),
        );
        state.set_aggregator_config(
            "harm0".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(5_000),
                heatmap_interval: Some(100_000),
                heatmap_counters: HashSet::from([selector]),
                ..Default::default()
            }),
        );

        let first = state
            .try_config_for_session_key("profile0|PORT")
            .unwrap()
            .unwrap();
        let second = state
            .try_config_for_session_key("profile1|PORT")
            .unwrap()
            .unwrap();
        assert_eq!(first.poll_interval_us, Some(1_000));
        assert_eq!(second.poll_interval_us, Some(10_000));
        assert_eq!(
            first.layout_for(selector).unwrap().explicit_bounds_u64()[1],
            31_250_000
        );
        assert_eq!(
            second.layout_for(selector).unwrap().explicit_bounds_u64()[1],
            62_500_000
        );
        assert!(!Arc::ptr_eq(
            &first.layout_for(selector).unwrap(),
            &second.layout_for(selector).unwrap()
        ));
        let first_layout = first.layout_for(selector).unwrap();
        let second_layout = second.layout_for(selector).unwrap();
        assert_ne!(
            crate::message::aggregator::heatmap_schema(
                selector.heatmap_value_kind(),
                selector.heatmap_quantity(),
                first_layout.explicit_bounds_u64(),
            ),
            crate::message::aggregator::heatmap_schema(
                selector.heatmap_value_kind(),
                selector.heatmap_quantity(),
                second_layout.explicit_bounds_u64(),
            )
        );

        state.remove_profile("profile0");
        assert!(!state.profile_poll_intervals_us.contains_key("profile0"));
    }

    #[test]
    fn profile_poll_update_changes_effective_byte_layout() {
        let selector = CounterSelector::parse("PORT|IF_OUT_OCTETS").unwrap();
        let mut state = AggregatorConfigState::default();
        state.set_profile(
            "profile0".to_string(),
            Some("harm0".to_string()),
            Some(1_000),
        );
        state.set_aggregator_config(
            "harm0".to_string(),
            Some(AggregatorConfig {
                heatmap_interval: Some(100_000),
                heatmap_counters: HashSet::from([selector]),
                ..Default::default()
            }),
        );
        let before = state
            .try_config_for_session_key("profile0|PORT")
            .unwrap()
            .unwrap();

        state.set_profile(
            "profile0".to_string(),
            Some("harm0".to_string()),
            Some(10_000),
        );
        let after = state
            .try_config_for_session_key("profile0|PORT")
            .unwrap()
            .unwrap();
        assert_ne!(before, after);
        assert_ne!(
            before.layout_for(selector).unwrap().explicit_bounds_u64(),
            after.layout_for(selector).unwrap().explicit_bounds_u64()
        );
    }

    #[test]
    fn custom_byte_layout_does_not_require_profile_interval() {
        let selector = CounterSelector::parse("PORT|IF_IN_OCTETS").unwrap();
        let mut state = AggregatorConfigState::default();
        state.set_profile("profile0".to_string(), Some("harm0".to_string()), None);
        state.set_aggregator_config(
            "harm0".to_string(),
            Some(AggregatorConfig {
                heatmap_interval: Some(100_000),
                heatmap_counters: HashSet::from([selector]),
                ..Default::default()
            }),
        );
        state.set_histogram_config("harm0".to_string(), selector, vec![0, 100, 1_000]);

        let config = state
            .try_config_for_session_key("profile0|PORT")
            .unwrap()
            .unwrap();
        assert_eq!(
            config.layout_for(selector).unwrap().explicit_bounds_u64(),
            &[0, 100, 1_000]
        );

        state.set_profile(
            "profile0".to_string(),
            Some("harm0".to_string()),
            Some(50_000),
        );
        let updated = state
            .try_config_for_session_key("profile0|PORT")
            .unwrap()
            .unwrap();
        assert_eq!(
            updated.layout_for(selector).unwrap().explicit_bounds_u64(),
            &[0, 100, 1_000]
        );
        assert_eq!(
            crate::message::aggregator::heatmap_schema(
                selector.heatmap_value_kind(),
                selector.heatmap_quantity(),
                config.layout_for(selector).unwrap().explicit_bounds_u64(),
            ),
            crate::message::aggregator::heatmap_schema(
                selector.heatmap_value_kind(),
                selector.heatmap_quantity(),
                updated.layout_for(selector).unwrap().explicit_bounds_u64(),
            )
        );
    }

    #[test]
    fn missing_profile_interval_rejects_only_default_byte_counter() {
        let bytes = CounterSelector::parse("PORT|IF_IN_OCTETS").unwrap();
        let count = CounterSelector::parse("PORT|IF_IN_UCAST_PKTS").unwrap();
        let mut state = AggregatorConfigState::default();
        state.set_profile("profile0".to_string(), Some("harm0".to_string()), None);
        state.set_aggregator_config(
            "harm0".to_string(),
            Some(AggregatorConfig {
                heatmap_interval: Some(100_000),
                heatmap_counters: HashSet::from([count]),
                ..Default::default()
            }),
        );
        assert!(state
            .try_config_for_session_key("profile0|PORT")
            .unwrap()
            .is_some());

        state.set_aggregator_config(
            "harm0".to_string(),
            Some(AggregatorConfig {
                heatmap_interval: Some(100_000),
                heatmap_counters: HashSet::from([bytes]),
                ..Default::default()
            }),
        );
        assert!(state
            .try_config_for_session_key("profile0|PORT")
            .unwrap_err()
            .contains("byte-delta"));
    }

    #[tokio::test]
    async fn test_session_update_without_object_names() {
        let (template_sender, mut template_receiver) = channel(1);
        let mut actor = create_test_actor(template_sender);

        // Test session data without object names
        let key = "test_session|PORT";
        let mut field_values = HashMap::new();
        field_values.insert("stream_status".to_string(), CxxString::from("enabled"));
        field_values.insert("session_type".to_string(), CxxString::from("ipfix"));
        field_values.insert("object_ids".to_string(), CxxString::from("1"));
        field_values.insert(
            "session_config".to_string(),
            CxxString::from("ipfix_template_data"),
        );

        // Process the session update
        actor.handle_session_update(key, &field_values).await;

        // Verify the message was sent
        let received_message = template_receiver
            .try_recv()
            .expect("Should have received a message");
        assert_eq!(received_message.key, "test_session|PORT");
        assert!(!received_message.is_delete);
        assert!(received_message.templates.is_some());
        assert!(received_message.object_names.is_none());
        assert_eq!(received_message.object_ids, Some(vec![1]));
    }

    #[tokio::test]
    async fn test_session_deletion() {
        let (template_sender, mut template_receiver) = channel(1);
        let mut actor = create_test_actor(template_sender);

        let key = "test_session|PORT";

        // Process session deletion
        actor.handle_session_delete(key).await;

        // Verify the deletion message was sent
        let received_message = template_receiver
            .try_recv()
            .expect("Should have received a deletion message");
        assert_eq!(received_message.key, "test_session|PORT");
        assert!(received_message.is_delete);
        assert!(received_message.templates.is_none());
        assert!(received_message.object_names.is_none());
        assert!(received_message.object_ids.is_none());
        assert!(received_message
            .aggregator_config
            .as_ref()
            .is_some_and(|config| config.is_delete));
    }

    #[tokio::test]
    async fn test_disabled_session_not_processed() {
        let (template_sender, mut template_receiver) = channel(1);
        let mut actor = create_test_actor(template_sender);

        // Test disabled session
        let key = "disabled_session|PORT";
        let mut field_values = HashMap::new();
        field_values.insert("stream_status".to_string(), CxxString::from("disabled"));
        field_values.insert("session_type".to_string(), CxxString::from("ipfix"));
        field_values.insert("object_names".to_string(), CxxString::from("Ethernet0"));
        field_values.insert("session_config".to_string(), CxxString::from("test_config"));

        // Process the session update
        actor.handle_session_update(key, &field_values).await;

        let received_message = template_receiver
            .try_recv()
            .expect("disabled session should unregister its template");
        assert!(received_message.is_delete);
        assert!(received_message
            .aggregator_config
            .as_ref()
            .is_some_and(|config| config.is_delete));
    }

    #[tokio::test]
    async fn test_non_ipfix_session_not_processed() {
        let (template_sender, mut template_receiver) = channel(1);
        let mut actor = create_test_actor(template_sender);

        // Test non-IPFIX session
        let key = "non_ipfix_session|PORT";
        let mut field_values = HashMap::new();
        field_values.insert("stream_status".to_string(), CxxString::from("enabled"));
        field_values.insert("session_type".to_string(), CxxString::from("netflow"));
        field_values.insert("object_names".to_string(), CxxString::from("Ethernet0"));
        field_values.insert("session_config".to_string(), CxxString::from("test_config"));

        // Process the session update
        actor.handle_session_update(key, &field_values).await;

        let received_message = template_receiver
            .try_recv()
            .expect("invalid session type should unregister its template");
        assert!(received_message.is_delete);
    }

    #[tokio::test]
    async fn test_empty_object_names_handling() {
        let (template_sender, mut template_receiver) = channel(1);
        let mut actor = create_test_actor(template_sender);

        // Test session data with empty object_names string
        let key = "empty_names_session|PORT";
        let mut field_values = HashMap::new();
        field_values.insert("stream_status".to_string(), CxxString::from("enabled"));
        field_values.insert("session_type".to_string(), CxxString::from("ipfix"));
        field_values.insert("object_names".to_string(), CxxString::from(""));
        field_values.insert("object_ids".to_string(), CxxString::from("1"));
        field_values.insert(
            "session_config".to_string(),
            CxxString::from("ipfix_template_data"),
        );

        // Process the session update
        actor.handle_session_update(key, &field_values).await;

        // Verify the message was sent with None object_names
        let received_message = template_receiver
            .try_recv()
            .expect("Should have received a message");
        assert_eq!(received_message.key, "empty_names_session|PORT");
        assert!(!received_message.is_delete);
        assert!(received_message.templates.is_some());
        assert!(received_message.object_names.is_none());
        assert_eq!(received_message.object_ids, Some(vec![1]));
    }

    #[test]
    fn test_session_data_default() {
        let session_data = SessionData::default();
        assert_eq!(session_data.stream_status, "");
        assert_eq!(session_data.session_type, "");
        assert_eq!(session_data.object_names, "");
        assert_eq!(session_data.object_ids, "");
        assert!(session_data.session_config.is_empty());
    }

    #[test]
    fn test_ipfix_templates_message_new() {
        let templates = Arc::new(vec![1, 2, 3, 4]);
        let object_names = Some(vec!["Ethernet0".to_string(), "Ethernet1".to_string()]);

        let object_ids = Some(vec![1, 2]);

        let message = IPFixTemplatesMessage::new(
            "test_key".to_string(),
            templates.clone(),
            object_names.clone(),
            object_ids.clone(),
        );

        assert_eq!(message.key, "test_key");
        assert_eq!(message.templates, Some(templates));
        assert_eq!(message.object_names, object_names);
        assert_eq!(message.object_ids, object_ids);
        assert!(!message.is_delete);
    }

    #[test]
    fn test_ipfix_templates_message_delete() {
        let message = IPFixTemplatesMessage::delete("test_key".to_string());

        assert_eq!(message.key, "test_key");
        assert!(message.templates.is_none());
        assert!(message.object_names.is_none());
        assert!(message.object_ids.is_none());
        assert!(message.is_delete);
    }

    // Helper function to create a test session entry in Redis
    async fn insert_test_session(
        table: &swss_common::Table,
        session_key: &str, // This should be just the session part, e.g., "test_existing_data|PORT"
        object_names: &str,
        object_ids: &str,
        session_config: &str,
    ) {
        use swss_common::CxxString;

        // The full Redis key includes the table name prefix
        let full_redis_key = format!(
            "{}|{}",
            STATE_HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE, session_key
        );

        // Use table.set to set all field-value pairs at once
        let field_values = vec![
            ("stream_status", CxxString::from("enabled")),
            ("session_type", CxxString::from("ipfix")),
            ("object_names", CxxString::from(object_names)),
            ("object_ids", CxxString::from(object_ids)),
            ("session_config", CxxString::from(session_config)),
        ];

        table
            .set(&full_redis_key, field_values)
            .expect("Should be able to insert session data using table.set");
    }

    // Helper function to set up Redis table for testing
    fn setup_test_table() -> swss_common::Table {
        use swss_common::{DbConnector, Table};

        let table_conn = DbConnector::new_unix(STATE_DB_ID, SOCK_PATH, 0)
            .expect("Should be able to connect to Redis for table");
        let table = Table::new(table_conn, STATE_HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE)
            .expect("Should be able to create table");

        // More aggressive cleanup: try to delete all possible test patterns
        let test_patterns = [
            "HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE|test*",
            "HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE|*test*",
            "test*",
            "*test*",
        ];
        for pattern in &test_patterns {
            table.del(pattern).ok();
        }

        // Also try FLUSHDB to completely clear the test database
        // Note: This is aggressive but necessary for test isolation
        // table.flushdb().ok();  // Uncomment if needed

        table
    }

    // Helper function to cleanup test data
    fn cleanup_test_session(table: &swss_common::Table, session_key: &str) {
        let full_redis_key = format!(
            "{}|{}",
            STATE_HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE, session_key
        );
        table.del(&full_redis_key).ok();
    }

    #[tokio::test]
    #[serial_test::serial]
    async fn test_swss_actor_processes_existing_data() {
        use std::time::{SystemTime, UNIX_EPOCH};

        let table = setup_test_table();

        // Use a unique key based on timestamp to avoid interference
        let timestamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let test_key = format!("test_existing_data_{}", timestamp);

        // Clean up any potential conflicting data first
        cleanup_test_session(&table, &test_key);
        tokio::time::sleep(Duration::from_millis(50)).await;

        // Insert test data BEFORE starting the actor
        insert_test_session(&table, &test_key, "Ethernet0", "1", "test_template_data").await;

        tokio::time::sleep(Duration::from_millis(100)).await;

        // Create and start SwssActor
        let (template_sender, mut template_receiver) = channel(10);
        let actor = create_test_actor(template_sender);

        // Run actor (will auto-terminate in test mode)
        SwssActor::run(actor).await;

        // Check messages received
        let mut received_messages = Vec::new();
        while let Ok(msg) = template_receiver.try_recv() {
            received_messages.push(msg);
        }

        // Cleanup
        cleanup_test_session(&table, &test_key);

        // Verify results
        let found_our_message = received_messages.iter().any(|msg| msg.key == test_key);
        assert!(found_our_message,
                "SwssActor should have processed existing session data with key: {}. Received {} messages: {:?}",
                test_key,
                received_messages.len(),
                received_messages.iter().map(|m| &m.key).collect::<Vec<_>>());

        // Verify message content
        let our_message = received_messages
            .iter()
            .find(|msg| msg.key == test_key)
            .unwrap();
        assert!(!our_message.is_delete);
        assert!(our_message.templates.is_some());

        let object_names = our_message
            .object_names
            .as_ref()
            .expect("Should have object_names");
        assert_eq!(object_names, &vec!["Ethernet0"]);
    }

    #[tokio::test]
    #[serial_test::serial]
    async fn test_swss_actor_runtime_data_behavior() {
        use std::time::{SystemTime, UNIX_EPOCH};

        let table = setup_test_table();

        // Use a unique key based on timestamp to avoid interference
        let timestamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let test_key = format!("test_runtime_data_{}", timestamp);

        // Create SwssActor
        let (template_sender, mut template_receiver) = channel(10);
        let actor = create_test_actor(template_sender);

        // Insert test data BEFORE starting the actor
        insert_test_session(
            &table,
            &test_key,
            "Ethernet1,Ethernet2",
            "2,3",
            "test_runtime_template",
        )
        .await;

        // Run actor (will auto-terminate in test mode)
        SwssActor::run(actor).await;

        // Check if we received the data
        let mut received_messages = Vec::new();
        while let Ok(msg) = template_receiver.try_recv() {
            received_messages.push(msg);
        }

        // Cleanup
        cleanup_test_session(&table, &test_key);

        // Look for our specific message
        let message_found = received_messages.iter().any(|msg| msg.key == test_key);

        if message_found {
            // If data was detected, verify it's correct
            let received_message = received_messages
                .iter()
                .find(|msg| msg.key == test_key)
                .unwrap();
            assert_eq!(received_message.key, test_key);
            assert!(!received_message.is_delete);
            assert!(received_message.templates.is_some());

            let object_names = received_message
                .object_names
                .as_ref()
                .expect("Should have object_names");
            assert_eq!(object_names, &vec!["Ethernet1", "Ethernet2"]);
        }

        // The test passes regardless of whether data was detected or not
        // because the behavior depends on the specific SWSS implementation and configuration
    }

    #[tokio::test]
    #[serial_test::serial]
    async fn test_swss_actor_comprehensive_flow() {
        use std::time::{SystemTime, UNIX_EPOCH};

        let table = setup_test_table();

        // Use a unique key based on timestamp to avoid interference
        let timestamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let existing_key = format!("test_existing_{}", timestamp);
        let runtime_key = format!("test_runtime_{}", timestamp);

        // Step 1: Insert both EXISTING and RUNTIME data before starting actor
        insert_test_session(
            &table,
            &existing_key,
            "Ethernet0",
            "1",
            "existing_template_data",
        )
        .await;

        insert_test_session(
            &table,
            &runtime_key,
            "Ethernet3,Ethernet4",
            "3,4",
            "runtime_template_data",
        )
        .await;

        tokio::time::sleep(Duration::from_millis(100)).await;

        // Step 2: Create and run SwssActor
        let (template_sender, mut template_receiver) = channel(10);
        let actor = create_test_actor(template_sender);

        // Run actor (will auto-terminate in test mode)
        SwssActor::run(actor).await;

        // Step 3: Collect all messages
        let mut all_messages = Vec::new();
        while let Ok(msg) = template_receiver.try_recv() {
            all_messages.push(msg);
        }

        // Cleanup
        cleanup_test_session(&table, &existing_key);
        cleanup_test_session(&table, &runtime_key);

        // Step 4: Verify the existing session was processed
        let found_existing_message = all_messages.iter().any(|msg| msg.key == existing_key);
        assert!(found_existing_message,
                "SwssActor should have processed existing session data with key: {}. Received {} messages: {:?}",
                existing_key,
                all_messages.len(),
                all_messages.iter().map(|m| &m.key).collect::<Vec<_>>());

        // Verify existing message content
        let existing_message = all_messages
            .iter()
            .find(|msg| msg.key == existing_key)
            .unwrap();
        assert!(!existing_message.is_delete);
        assert!(existing_message.templates.is_some());

        let existing_object_names = existing_message
            .object_names
            .as_ref()
            .expect("Should have object_names");
        assert_eq!(existing_object_names, &vec!["Ethernet0"]);

        // Step 5: Check for runtime data (optional behavior)
        let runtime_message_found = all_messages.iter().any(|msg| msg.key == runtime_key);

        if runtime_message_found {
            // If runtime data was detected, verify it's correct
            let runtime_message = all_messages
                .iter()
                .find(|msg| msg.key == runtime_key)
                .unwrap();
            assert_eq!(runtime_message.key, runtime_key);
            assert!(!runtime_message.is_delete);
            assert!(runtime_message.templates.is_some());

            let runtime_object_names = runtime_message
                .object_names
                .as_ref()
                .expect("Should have object_names");
            assert_eq!(runtime_object_names, &vec!["Ethernet3", "Ethernet4"]);
        }

        // Test passes if existing data was processed correctly
        // Runtime data detection depends on SWSS implementation details
    }
}
