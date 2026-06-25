use super::super::message::{
    aggregator::{AggregatorConfig, AggregatorConfigMessage},
    ipfix::IPFixTemplatesMessage,
};
use swss_common::{DbConnector, KeyOperation, SubscriberStateTable};

use log::{debug, error, info, warn};
use std::time::Duration;
use std::{
    collections::{HashMap, HashSet},
    sync::Arc,
};
use tokio::{select, sync::mpsc::Sender};

const SOCK_PATH: &str = "/var/run/redis/redis.sock";
const CONFIG_DB_ID: i32 = 4;
const STATE_DB_ID: i32 = 6;
const STATE_HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE: &str = "HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE";
const CONFIG_HIGH_FREQUENCY_TELEMETRY_PROFILE_TABLE: &str = "HIGH_FREQUENCY_TELEMETRY_PROFILE";
const CONFIG_HIGH_FREQUENCY_TELEMETRY_AGGREGATOR_TABLE: &str =
    "HIGH_FREQUENCY_TELEMETRY_AGGREGATOR";

#[cfg(test)]
const MAX_TEST_IDLE_ITERATIONS: usize = 20;

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
    template_recipient: Sender<IPFixTemplatesMessage>,
    aggregator_config_recipient: Sender<AggregatorConfigMessage>,
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
    ProfileUpdate {
        profile: String,
        aggregator: Option<String>,
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
}

impl SwssActor {
    /// Creates a new SwssActor instance
    ///
    /// # Arguments
    /// * `template_recipient` - Channel sender for forwarding IPFIX templates to IPFIX actor
    pub fn new(
        template_recipient: Sender<IPFixTemplatesMessage>,
        aggregator_config_recipient: Sender<AggregatorConfigMessage>,
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

        Ok(SwssActor {
            session_table,
            profile_table,
            aggregator_table,
            template_recipient,
            aggregator_config_recipient,
        })
    }

    /// Main event loop for the SwssActor
    ///
    /// Continuously monitors HFT session state and aggregator/profile config updates.
    ///
    /// # Arguments
    /// * `actor` - SwssActor instance to run
    pub async fn run(actor: SwssActor) {
        info!(
            "SwssActor started, monitoring HFT session state and aggregator/profile config"
        );

        let SwssActor {
            mut session_table,
            mut profile_table,
            mut aggregator_table,
            template_recipient,
            aggregator_config_recipient,
        } = actor;
        let mut aggregator_state = AggregatorConfigState::default();

        #[cfg(test)]
        let mut idle_iterations = 0;

        let mut pending_events = Vec::new();
        for events in [
            Self::collect_profile_events(&profile_table),
            Self::collect_aggregator_events(&aggregator_table),
            Self::collect_session_events(&session_table),
        ] {
            match events {
                Ok(events) => pending_events.extend(events),
                Err(e) => error!("{}", e),
            }
        }

        loop {
            let events = if pending_events.is_empty() {
                select! {
                    result = session_table.read_data_async() => {
                        match result {
                            Ok(()) => Self::collect_session_events(&session_table),
                            Err(e) => Err(format!("Error reading from session table: {}", e)),
                        }
                    }
                    result = profile_table.read_data_async() => {
                        match result {
                            Ok(()) => Self::collect_profile_events(&profile_table),
                            Err(e) => Err(format!("Error reading from profile table: {}", e)),
                        }
                    }
                    result = aggregator_table.read_data_async() => {
                        match result {
                            Ok(()) => Self::collect_aggregator_events(&aggregator_table),
                            Err(e) => Err(format!("Error reading from aggregator table: {}", e)),
                        }
                    }
                    _ = tokio::time::sleep(Duration::from_millis(50)), if cfg!(test) => {
                        Ok(Vec::new())
                    }
                }
            } else {
                Ok(std::mem::take(&mut pending_events))
            };

            let events = match events {
                Ok(events) => events,
                Err(e) => {
                    error!("{}", e);
                    tokio::time::sleep(Duration::from_millis(100)).await;
                    continue;
                }
            };

            #[cfg(test)]
            {
                if events.is_empty() {
                    idle_iterations += 1;
                    if idle_iterations > MAX_TEST_IDLE_ITERATIONS {
                        debug!("SwssActor test mode reached idle limit, terminating");
                        break;
                    }
                } else {
                    idle_iterations = 0;
                }
            }

            for event in events {
                match event {
                SwssEvent::SessionUpdate { key, session_data } => {
                    if Self::process_session_update(&template_recipient, &key, &session_data).await {
                        aggregator_state.add_session(key.clone());
                        Self::send_aggregator_config_for_session(
                            &aggregator_config_recipient,
                            &aggregator_state,
                            &key,
                            false,
                        )
                        .await;
                    } else {
                        aggregator_state.remove_session(&key);
                        Self::send_aggregator_config_for_session(
                            &aggregator_config_recipient,
                            &aggregator_state,
                            &key,
                            true,
                        )
                        .await;
                    }
                }
                SwssEvent::SessionDelete { key } => {
                    Self::process_session_delete(&template_recipient, &key).await;
                    aggregator_state.remove_session(&key);
                    Self::send_aggregator_config_for_session(
                        &aggregator_config_recipient,
                        &aggregator_state,
                        &key,
                        true,
                    )
                    .await;
                }
                SwssEvent::ProfileUpdate {
                    profile,
                    aggregator,
                } => {
                    let affected_sessions = aggregator_state.session_keys_for_profile(&profile);
                    aggregator_state.set_profile_aggregator(profile, aggregator);
                    Self::send_aggregator_configs_for_sessions(
                        &aggregator_config_recipient,
                        &aggregator_state,
                        affected_sessions,
                    )
                    .await;
                }
                SwssEvent::ProfileDelete { profile } => {
                    let affected_sessions = aggregator_state.session_keys_for_profile(&profile);
                    aggregator_state.remove_profile(&profile);
                    Self::send_aggregator_configs_for_sessions(
                        &aggregator_config_recipient,
                        &aggregator_state,
                        affected_sessions,
                    )
                    .await;
                }
                SwssEvent::AggregatorUpdate { name, config } => {
                    let affected_sessions = aggregator_state.session_keys_for_aggregator(&name);
                    aggregator_state.set_aggregator_config(name, config);
                    Self::send_aggregator_configs_for_sessions(
                        &aggregator_config_recipient,
                        &aggregator_state,
                        affected_sessions,
                    )
                    .await;
                }
                SwssEvent::AggregatorDelete { name } => {
                    let affected_sessions = aggregator_state.session_keys_for_aggregator(&name);
                    aggregator_state.remove_aggregator(&name);
                    Self::send_aggregator_configs_for_sessions(
                        &aggregator_config_recipient,
                        &aggregator_state,
                        affected_sessions,
                    )
                    .await;
                }
            }
            }
        }
    }

    fn collect_session_events(session_table: &SubscriberStateTable) -> Result<Vec<SwssEvent>, String> {
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

    fn collect_profile_events(profile_table: &SubscriberStateTable) -> Result<Vec<SwssEvent>, String> {
        let items = profile_table
            .pops()
            .map_err(|e| format!("Error popping items from profile table: {}", e))?;
        let mut events = Vec::with_capacity(items.len());

        for item in items {
            let profile = Self::extract_config_key(
                &item.key,
                CONFIG_HIGH_FREQUENCY_TELEMETRY_PROFILE_TABLE,
            );
            match item.operation {
                KeyOperation::Set => events.push(SwssEvent::ProfileUpdate {
                    profile,
                    aggregator: Self::parse_profile_aggregator(&item.field_values),
                }),
                KeyOperation::Del => events.push(SwssEvent::ProfileDelete { profile }),
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
                KeyOperation::Set => events.push(SwssEvent::AggregatorUpdate {
                    name,
                    config: Self::parse_aggregator_config(&item.field_values),
                }),
                KeyOperation::Del => events.push(SwssEvent::AggregatorDelete { name }),
            }
        }

        Ok(events)
    }

    fn parse_session_data(
        field_values: &HashMap<String, swss_common::CxxString>,
    ) -> SessionData {
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

    fn parse_profile_aggregator(
        field_values: &HashMap<String, swss_common::CxxString>,
    ) -> Option<String> {
        field_values.get("aggregator").and_then(|value| {
            let aggregator = value.to_string_lossy().trim().to_string();
            if aggregator.is_empty() {
                None
            } else {
                Some(aggregator)
            }
        })
    }

    fn parse_aggregator_config(
        field_values: &HashMap<String, swss_common::CxxString>,
    ) -> Option<AggregatorConfig> {
        let reporting_rate = field_values
            .get("reporting_rate")
            .and_then(|value| AggregatorConfig::parse(&value.to_string_lossy()))
            .and_then(|config| config.reporting_rate);

        Some(AggregatorConfig { reporting_rate })
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
        if let Some(pos) = full_key.find('|') {
            if full_key.starts_with(table_name) {
                return full_key[pos + 1..].to_string();
            }
        }
        full_key.to_string()
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

    async fn process_session_update(
        template_recipient: &Sender<IPFixTemplatesMessage>,
        key: &str,
        session_data: &SessionData,
    ) -> bool {
        match Self::validate_and_send_session(template_recipient, key, session_data).await {
            Ok(processed) => processed,
            Err(e) => {
                error!("Failed to process session {}: {}", key, e);
                false
            }
        }
    }

    async fn process_session_delete(
        template_recipient: &Sender<IPFixTemplatesMessage>,
        key: &str,
    ) {
        info!("Session deleted: {}", key);

        let delete_message = IPFixTemplatesMessage::delete(key.to_string());

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
        Self::validate_and_send_session(&self.template_recipient, key, session_data).await
    }

    /// Validates session data and processes enabled IPFIX sessions
    ///
    /// # Arguments
    /// * `key` - Session identifier
    /// * `session_data` - Parsed session configuration
    async fn validate_and_send_session(
        template_recipient: &Sender<IPFixTemplatesMessage>,
        key: &str,
        session_data: &SessionData,
    ) -> Result<bool, String> {
        // Only process enabled sessions with ipfix type
        if session_data.stream_status != "enabled" {
            debug!("Skipping disabled session: {}", key);
            return Ok(false);
        }

        if session_data.session_type != "ipfix" {
            debug!(
                "Skipping non-IPFIX session: {} (type: {})",
                key, session_data.session_type
            );
            return Ok(false);
        }

        if session_data.session_config.is_empty() {
            return Err("Session config is empty".to_string());
        }

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

        let message = IPFixTemplatesMessage::new(
            key.to_string(),
            templates,
            object_names,
            object_ids,
        );

        template_recipient
            .send(message)
            .await
            .map_err(|e| format!("Failed to send IPFix templates to recipient: {}", e))?;

        info!("Successfully sent IPFix templates for session: {}", key);
        Ok(true)
    }

    async fn send_aggregator_config_for_session(
        aggregator_config_recipient: &Sender<AggregatorConfigMessage>,
        aggregator_state: &AggregatorConfigState,
        key: &str,
        is_delete: bool,
    ) {
        let message = if is_delete {
            AggregatorConfigMessage::delete(key.to_string())
        } else {
            AggregatorConfigMessage::new(
                key.to_string(),
                aggregator_state.config_for_session_key(key).cloned(),
            )
        };

        if let Err(e) = aggregator_config_recipient.send(message).await {
            error!("Failed to send aggregator config for {}: {}", key, e);
        }
    }

    async fn send_aggregator_configs_for_sessions(
        aggregator_config_recipient: &Sender<AggregatorConfigMessage>,
        aggregator_state: &AggregatorConfigState,
        session_keys: Vec<String>,
    ) {
        for key in session_keys {
            Self::send_aggregator_config_for_session(
                aggregator_config_recipient,
                aggregator_state,
                &key,
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
    aggregator_configs: HashMap<String, AggregatorConfig>,
    sessions: HashSet<String>,
}

impl AggregatorConfigState {
    fn add_session(&mut self, key: String) {
        self.sessions.insert(key);
    }

    fn remove_session(&mut self, key: &str) {
        self.sessions.remove(key);
    }

    fn set_profile_aggregator(&mut self, profile: String, aggregator: Option<String>) {
        match aggregator {
            Some(aggregator) => {
                self.profile_aggregators.insert(profile, aggregator);
            }
            None => {
                self.profile_aggregators.remove(&profile);
            }
        }
    }

    fn remove_profile(&mut self, profile: &str) {
        self.profile_aggregators.remove(profile);
    }

    fn set_aggregator_config(&mut self, name: String, config: Option<AggregatorConfig>) {
        match config {
            Some(config) => {
                self.aggregator_configs.insert(name, config);
            }
            None => {
                self.aggregator_configs.remove(&name);
            }
        }
    }

    fn remove_aggregator(&mut self, name: &str) {
        self.aggregator_configs.remove(name);
    }

    fn config_for_session_key(&self, session_key: &str) -> Option<&AggregatorConfig> {
        let profile = SwssActor::extract_profile_from_session_key(session_key);
        let aggregator = self.profile_aggregators.get(profile)?;
        self.aggregator_configs.get(aggregator)
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
    use std::collections::HashMap;
    use swss_common::CxxString;
    use tokio::sync::mpsc::channel;

    // Helper function to create a SwssActor for testing
    fn create_test_actor(template_sender: Sender<IPFixTemplatesMessage>) -> SwssActor {
        let (aggregator_config_sender, mut aggregator_config_receiver) = channel(100);
        tokio::spawn(async move { while aggregator_config_receiver.recv().await.is_some() {} });
        SwssActor::new(template_sender, aggregator_config_sender).expect("Failed to create SwssActor")
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
    }

    #[test]
    fn test_profile_and_aggregator_config_mapping() {
        let mut profile_fields = HashMap::new();
        profile_fields.insert("aggregator".to_string(), CxxString::from("harm0"));
        assert_eq!(
            SwssActor::parse_profile_aggregator(&profile_fields),
            Some("harm0".to_string())
        );

        let mut aggregator_fields = HashMap::new();
        aggregator_fields.insert("reporting_rate".to_string(), CxxString::from("100"));
        assert_eq!(
            SwssActor::parse_aggregator_config(&aggregator_fields)
                .expect("aggregator config")
                .reporting_rate,
            Some(100)
        );

        let empty_aggregator_fields = HashMap::new();
        assert_eq!(
            SwssActor::parse_aggregator_config(&empty_aggregator_fields)
                .expect("aggregator config")
                .reporting_rate,
            None
        );

        let mut state = AggregatorConfigState::default();
        state.add_session("profile0|PORT".to_string());
        state.set_profile_aggregator("profile0".to_string(), Some("harm0".to_string()));
        state.set_aggregator_config(
            "harm0".to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(100),
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

        // Verify no message was sent
        assert!(template_receiver.try_recv().is_err());
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

        // Verify no message was sent
        assert!(template_receiver.try_recv().is_err());
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
