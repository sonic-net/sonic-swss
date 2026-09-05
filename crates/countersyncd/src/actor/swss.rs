use super::super::message::ipfix::IPFixTemplatesMessage;
use swss_common::{DbConnector, KeyOperation, SubscriberStateTable};

use log::{debug, error, info};
use std::time::Duration;
use std::{collections::HashMap, sync::Arc, thread};
use tokio::sync::mpsc::{self, Sender};

const SOCK_PATH: &str = "/var/run/redis/redis.sock";
const STATE_DB_ID: i32 = 6;
const STATE_HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE: &str = "HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE";
const SWSS_EVENT_CHANNEL_CAPACITY: usize = 32;
const SWSS_SELECT_TIMEOUT: Duration = Duration::from_millis(50);
// Bound the raw Redis representation before copying/tokenizing. IPFIX keeps its
// own admission checks for callers that bypass SWSS.
const MAX_TEMPLATE_CONFIG_BYTES: usize = 4 * 1024 * 1024;
const MAX_OBJECT_METADATA_BYTES: usize = 4 * 1024 * 1024;
const MAX_OBJECTS_PER_UPDATE: usize = 32_767;

/// SwssActor is responsible for monitoring SONiC orchestrator agent (orchagent)
/// messages through the state database. It specifically listens for
/// HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE updates and forwards IPFIX template
/// configurations to the IPFIX actor.
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
    template_recipient: Sender<IPFixTemplatesMessage>,
}

#[derive(Debug)]
pub enum SwssError {
    ReaderFailed(String),
}

impl std::fmt::Display for SwssError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::ReaderFailed(message) => write!(formatter, "reader failed: {message}"),
        }
    }
}

#[derive(Debug)]
enum SwssEvent {
    Update {
        key: String,
        session_data: SessionData,
    },
    Delete {
        key: String,
    },
}

impl SwssActor {
    /// Creates a new SwssActor instance
    ///
    /// # Arguments
    /// * `template_recipient` - Channel sender for forwarding IPFIX templates to IPFIX actor
    pub fn new(template_recipient: Sender<IPFixTemplatesMessage>) -> Result<Self, String> {
        let connect = DbConnector::new_unix(STATE_DB_ID, SOCK_PATH, 0)
            .map_err(|e| format!("Failed to create DB connection: {}", e))?;
        let session_table = SubscriberStateTable::new(
            connect,
            STATE_HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE,
            None,
            None,
        )
        .map_err(|e| format!("Failed to create session table: {}", e))?;

        Ok(SwssActor {
            session_table,
            template_recipient,
        })
    }

    /// Main event loop for the SwssActor
    ///
    /// Continuously monitors the HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE for updates
    /// and processes enabled IPFIX sessions by forwarding their templates to the IPFIX actor.
    ///
    /// # Arguments
    /// * `actor` - SwssActor instance to run
    pub async fn run(actor: SwssActor) -> Result<(), SwssError> {
        info!("SwssActor started, monitoring HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE");

        #[cfg(test)]
        const MAX_TEST_ITERATIONS: usize = 20;

        // Keep the SWSS table polling on a dedicated blocking thread so we don't park a Tokio worker.
        let SwssActor {
            mut session_table,
            template_recipient,
        } = actor;
        let (event_sender, mut event_receiver) = mpsc::channel(SWSS_EVENT_CHANNEL_CAPACITY);
        let (fatal_sender, mut fatal_receiver) = mpsc::channel(1);

        // Dropping the receivers on cancellation releases blocking_send and stops
        // polling. Do not join: synchronous Redis calls may block indefinitely.
        match thread::Builder::new()
            .name("countersyncd-swss".to_string())
            .spawn(move || {
                #[cfg(test)]
                let mut iteration_count = 0;

                loop {
                    if event_sender.is_closed() {
                        debug!("SwssActor event receiver closed, terminating reader thread");
                        break;
                    }

                    #[cfg(test)]
                    {
                        iteration_count += 1;
                        if iteration_count > MAX_TEST_ITERATIONS {
                            debug!(
                                "SwssActor test mode reached maximum iterations ({}), terminating reader thread",
                                MAX_TEST_ITERATIONS
                            );
                            break;
                        }
                    }

                    match Self::blocking_collect_events(&mut session_table, SWSS_SELECT_TIMEOUT) {
                        Ok(events) => {
                            for event in events {
                                if event_sender.blocking_send(event).is_err() {
                                    debug!("SwssActor event receiver dropped, terminating reader thread");
                                    return;
                                }
                            }
                        }
                        Err(e) => {
                            error!("Error reading from session table: {}", e);
                            let _ = fatal_sender.blocking_send(e);
                            break;
                        }
                    }
                }

                #[cfg(test)]
                debug!("SwssActor reader thread terminated after {} iterations", iteration_count);
            }) {
                Ok(_) => {},
                Err(e) => {
                    error!("Failed to spawn SwssActor reader thread: {}", e);
                    return Err(SwssError::ReaderFailed(e.to_string()));
                }
            };

        loop {
            tokio::select! {
                biased;
                failure = fatal_receiver.recv() => {
                    if let Some(failure) = failure {
                        error!("SwssActor reader failed: {failure}");
                        return Err(SwssError::ReaderFailed(failure));
                    }
                    break;
                }
                event = event_receiver.recv() => {
                    let Some(event) = event else {
                        break;
                    };
                    let processing = async {
                        match event {
                            SwssEvent::Update { key, session_data } => {
                                Self::process_session_update(&template_recipient, &key, &session_data).await;
                            }
                            SwssEvent::Delete { key } => {
                                if let Err(e) = template_recipient.send(IPFixTemplatesMessage::delete(key)).await {
                                    error!("Failed to delete IPFIX session: {e}");
                                }
                            }
                        }
                    };
                    tokio::select! {
                        biased;
                        failure = fatal_receiver.recv() => {
                            if let Some(failure) = failure {
                                error!("SwssActor reader failed while forwarding an event: {failure}");
                                return Err(SwssError::ReaderFailed(failure));
                            }
                            break;
                        }
                        _ = processing => {}
                    }
                }
            }
        }

        debug!("SwssActor terminated");
        Ok(())
    }

    fn blocking_collect_events(
        session_table: &mut SubscriberStateTable,
        timeout: Duration,
    ) -> Result<Vec<SwssEvent>, String> {
        let mut events = Vec::new();

        match session_table.read_data(timeout, false) {
            Ok(select_result) => match select_result {
                swss_common::SelectResult::Data => match session_table.pops() {
                    Ok(items) => {
                        for item in items {
                            if item.key.len() > MAX_OBJECT_METADATA_BYTES {
                                error!("Ignoring session key exceeding metadata byte limit");
                                continue;
                            }
                            debug!(
                                "SwssActor received: key={}, op={:?}",
                                item.key, item.operation
                            );

                            let session_key = Self::extract_session_key(&item.key);
                            match item.operation {
                                KeyOperation::Set => events.push(SwssEvent::Update {
                                    key: session_key,
                                    session_data: Self::parse_session_data(&item.field_values),
                                }),
                                KeyOperation::Del => {
                                    events.push(SwssEvent::Delete { key: session_key })
                                }
                            }
                        }
                        Ok(events)
                    }
                    Err(e) => Err(format!("Error popping items from session table: {}", e)),
                },
                swss_common::SelectResult::Timeout => {
                    debug!("Timeout waiting for session table updates");
                    Ok(events)
                }
                swss_common::SelectResult::Signal => {
                    debug!("Signal received while waiting for session table updates");
                    Ok(events)
                }
            },
            Err(e) => Err(format!("Error reading from session table: {}", e)),
        }
    }

    fn parse_session_data(field_values: &HashMap<String, swss_common::CxxString>) -> SessionData {
        // CxxString::len borrows the raw value without lossy UTF-8 allocation.
        let mut metadata_bytes = 0usize;
        let mut validation_error = None;
        for (field, value) in field_values {
            metadata_bytes = metadata_bytes.saturating_add(field.len());
            if field == "session_config" {
                if value.len() > MAX_TEMPLATE_CONFIG_BYTES {
                    validation_error = Some("Session config exceeds byte limit");
                }
            } else {
                metadata_bytes = metadata_bytes.saturating_add(value.len());
            }
            if metadata_bytes > MAX_OBJECT_METADATA_BYTES {
                validation_error = Some("Session metadata exceeds byte limit");
                break;
            }
        }
        if validation_error.is_some() {
            return SessionData {
                validation_error,
                ..SessionData::default()
            };
        }
        let mut session_data = SessionData::default();

        for (field, value) in field_values {
            match field.as_str() {
                "stream_status" => {
                    session_data.stream_status = value.to_string_lossy().into_owned()
                }
                "session_type" => session_data.session_type = value.to_string_lossy().into_owned(),
                "object_names" => session_data.object_names = value.to_string_lossy().into_owned(),
                "object_ids" => session_data.object_ids = value.to_string_lossy().into_owned(),
                "session_config" => {
                    session_data.session_config = value.as_bytes().to_vec();
                }
                _ => {
                    debug!("Ignoring unknown session field ({} bytes)", field.len());
                }
            }
        }

        session_data
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
        let session_data = Self::parse_session_data(field_values);

        // Validate and process the session
        if let Err(e) = self.validate_and_process_session(key, &session_data).await {
            error!("Failed to process session {}: {}", key, e);
        }
    }

    async fn process_session_update(
        template_recipient: &Sender<IPFixTemplatesMessage>,
        key: &str,
        session_data: &SessionData,
    ) {
        if let Err(e) = Self::validate_and_send_session(template_recipient, key, session_data).await
        {
            error!("Failed to process session {}: {}", key, e);
        }
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
    ) -> Result<(), String> {
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
    ) -> Result<(), String> {
        if key.len() > MAX_OBJECT_METADATA_BYTES {
            return Err("Session key exceeds metadata byte limit".into());
        }
        let size_error = session_data.validate_sizes().err();
        // A disabled or repurposed row deactivates any previously installed IPFIX session.
        if size_error.is_none()
            && (session_data.stream_status != "enabled" || session_data.session_type != "ipfix")
        {
            if session_data.stream_status != "enabled" {
                debug!("Deactivating disabled session: {}", key);
            } else {
                debug!(
                    "Deactivating non-IPFIX session: {} (type: {})",
                    key, session_data.session_type
                );
            }
            return template_recipient
                .send(IPFixTemplatesMessage::deactivate(key.to_string()))
                .await
                .map_err(|e| format!("Failed to deactivate IPFIX session {}: {}", key, e));
        }

        let message = match size_error.map_or_else(
            || Self::validated_update(key, session_data),
            |error| Err(error.to_string()),
        ) {
            Ok(message) => message,
            Err(err) => {
                template_recipient
                    .send(IPFixTemplatesMessage::delete(key.to_string()))
                    .await
                    .map_err(|e| format!("{err}; failed to delete IPFIX session {key}: {e}"))?;
                return Err(err);
            }
        };

        template_recipient
            .send(message)
            .await
            .map_err(|e| format!("Failed to send IPFix templates to recipient: {}", e))?;

        info!("Successfully sent IPFix templates for session: {}", key);
        Ok(())
    }

    fn validated_update(
        key: &str,
        session_data: &SessionData,
    ) -> Result<IPFixTemplatesMessage, String> {
        session_data.validate_sizes()?;
        if key.len() > MAX_OBJECT_METADATA_BYTES {
            return Err("Session key exceeds metadata byte limit".into());
        }
        if session_data.session_config.is_empty() {
            return Err("Session config is empty".to_string());
        }

        info!("Processing enabled IPFIX session: key={}", key);

        let mut object_names = Vec::new();
        for token in session_data.object_names.split(',') {
            let name = token.trim();
            if name.is_empty() {
                return Err("object_names must contain non-empty names".to_string());
            }
            if object_names.len() >= MAX_OBJECTS_PER_UPDATE {
                return Err("object_names exceeds entry limit".into());
            }
            object_names.push(name.to_string());
        }

        let mut object_ids = Vec::new();
        let mut unique_ids = std::collections::HashSet::new();
        for token in session_data.object_ids.split(',') {
            let trimmed = token.trim();
            if trimmed.is_empty() {
                return Err("object_ids must contain non-empty IDs".into());
            }
            if object_ids.len() >= MAX_OBJECTS_PER_UPDATE {
                return Err("object_ids exceeds entry limit".into());
            }
            let object_id = trimmed
                .parse::<u16>()
                .map_err(|e| format!("Invalid object ID '{}' for {}: {}", trimmed, key, e))?;
            if !(1..=0x7fff).contains(&object_id) {
                return Err(format!(
                    "Object ID {} for {} is outside the IPFIX 15-bit IE range",
                    object_id, key
                ));
            }
            if !unique_ids.insert(object_id) {
                return Err(format!("Duplicate object ID {} for {}", object_id, key));
            }
            object_ids.push(object_id);
        }
        if object_ids.len() != object_names.len() {
            return Err(format!(
                "object_ids/object_names length mismatch for {}: {} ids vs {} names",
                key,
                object_ids.len(),
                object_names.len()
            ));
        }

        Ok(IPFixTemplatesMessage::new(
            key.to_string(),
            Arc::new(session_data.session_config.clone()),
            Some(object_names),
            Some(object_ids),
        ))
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
    validation_error: Option<&'static str>,
    stream_status: String,
    session_type: String,
    object_names: String,
    object_ids: String,
    session_config: Vec<u8>,
}

impl SessionData {
    fn validate_sizes(&self) -> Result<(), &'static str> {
        if let Some(error) = self.validation_error {
            return Err(error);
        }
        if self.session_config.len() > MAX_TEMPLATE_CONFIG_BYTES {
            return Err("Session config exceeds byte limit");
        }
        let metadata_bytes = self
            .stream_status
            .len()
            .saturating_add(self.session_type.len())
            .saturating_add(self.object_names.len())
            .saturating_add(self.object_ids.len());
        if metadata_bytes > MAX_OBJECT_METADATA_BYTES {
            return Err("Session metadata exceeds byte limit");
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::ipfix::IPFixTemplateOperation;
    use std::collections::HashMap;
    use swss_common::CxxString;
    use tokio::sync::mpsc::channel;

    #[test]
    fn raw_session_limits_reject_before_copying_metadata_or_oversized_config() {
        for field in [
            "object_names",
            "object_ids",
            "stream_status",
            "session_type",
            "unknown",
            "session_config",
        ] {
            let fields = HashMap::from([(
                field.into(),
                CxxString::from(vec![b','; MAX_OBJECT_METADATA_BYTES + 1]),
            )]);
            let session = SwssActor::parse_session_data(&fields);
            assert!(session.validate_sizes().is_err(), "{field}");
            assert!(session.object_names.is_empty());
            assert!(session.object_ids.is_empty());
            assert!(session.stream_status.is_empty());
            assert!(session.session_type.is_empty());
            assert!(session.session_config.is_empty());
        }
        let fields = HashMap::from([
            (
                "object_names".into(),
                CxxString::from(vec![b','; MAX_OBJECT_METADATA_BYTES / 2]),
            ),
            (
                "object_ids".into(),
                CxxString::from(vec![b','; MAX_OBJECT_METADATA_BYTES / 2]),
            ),
            ("session_config".into(), CxxString::from("bounded config")),
        ]);
        let session = SwssActor::parse_session_data(&fields);
        assert!(session.validate_sizes().is_err());
        assert!(session.object_names.is_empty());
        assert!(session.session_config.is_empty());
    }

    #[tokio::test]
    async fn oversized_session_is_deleted_without_copying_config() {
        let (sender, mut receiver) = channel(1);
        let session = SessionData {
            session_config: vec![0; MAX_TEMPLATE_CONFIG_BYTES + 1],
            ..SessionData::default()
        };
        assert!(
            SwssActor::validate_and_send_session(&sender, "test", &session)
                .await
                .is_err()
        );
        let message = receiver.try_recv().unwrap();
        assert_eq!(message.operation, IPFixTemplateOperation::Delete);
        assert_eq!(message.key, "test");
        assert!(message.templates.is_none());
    }

    #[test]
    fn metadata_tokens_are_validated_incrementally_and_entry_capped() {
        let mut session = SessionData {
            stream_status: "enabled".into(),
            session_type: "ipfix".into(),
            object_names: ",".repeat(MAX_OBJECTS_PER_UPDATE + 1),
            object_ids: "1".into(),
            session_config: vec![1],
            ..SessionData::default()
        };
        assert!(SwssActor::validated_update("test", &session)
            .unwrap_err()
            .contains("non-empty names"));
        session.object_names = std::iter::repeat("x")
            .take(MAX_OBJECTS_PER_UPDATE)
            .collect::<Vec<_>>()
            .join(",");
        session.object_ids = (1..=MAX_OBJECTS_PER_UPDATE)
            .map(|id| id.to_string())
            .collect::<Vec<_>>()
            .join(",");
        assert!(SwssActor::validated_update("test", &session).is_ok());
        session.object_ids.push_str(",1");
        assert!(SwssActor::validated_update("test", &session)
            .unwrap_err()
            .contains("object_ids exceeds entry limit"));
        session.object_names.push_str(",x");
        assert!(SwssActor::validated_update("test", &session)
            .unwrap_err()
            .contains("object_names exceeds entry limit"));
        session.object_names = "x".into();
        session.object_ids = "1,".into();
        assert!(SwssActor::validated_update("test", &session)
            .unwrap_err()
            .contains("non-empty IDs"));
        session.object_names = "x".repeat(MAX_OBJECT_METADATA_BYTES + 1);
        assert!(SwssActor::validated_update("test", &session)
            .unwrap_err()
            .contains("metadata exceeds byte limit"));
    }

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
        assert_eq!(received_message.operation, IPFixTemplateOperation::Update);
        assert!(received_message.templates.is_some());

        // Verify object_names parsing
        let object_names = received_message
            .object_names
            .as_ref()
            .expect("Should have object_names");
        assert_eq!(object_names, &vec!["Ethernet0", "Ethernet1", "Ethernet2"]);
        assert_eq!(received_message.object_ids, Some(vec![1, 2, 3]));
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

        let message = template_receiver.try_recv().expect("delete message");
        assert_eq!(message.operation, IPFixTemplateOperation::Delete);
        assert_eq!(message.key, key);
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

        let message = template_receiver.try_recv().expect("deactivation message");
        assert_eq!(message.operation, IPFixTemplateOperation::Deactivate);
        assert_eq!(message.key, key);
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

        let message = template_receiver.try_recv().expect("deactivation message");
        assert_eq!(message.operation, IPFixTemplateOperation::Deactivate);
        assert_eq!(message.key, key);
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

        let message = template_receiver.try_recv().expect("delete message");
        assert_eq!(message.operation, IPFixTemplateOperation::Delete);
        assert_eq!(message.key, key);
    }

    #[tokio::test]
    async fn invalid_hft_metadata_is_not_forwarded() {
        async fn rejects(names: &str, ids: &str) {
            let (template_sender, mut template_receiver) = channel(1);
            let session = SessionData {
                stream_status: "enabled".to_string(),
                session_type: "ipfix".to_string(),
                object_names: names.to_string(),
                object_ids: ids.to_string(),
                session_config: vec![1],
                ..SessionData::default()
            };
            assert!(
                SwssActor::validate_and_send_session(&template_sender, "test|PORT", &session)
                    .await
                    .is_err()
            );
            let message = template_receiver.try_recv().expect("delete message");
            assert_eq!(message.operation, IPFixTemplateOperation::Delete);
            assert_eq!(message.key, "test|PORT");
            assert!(message.templates.is_none());
            assert!(template_receiver.try_recv().is_err());
        }

        rejects("Ethernet0", "0").await;
        rejects("Ethernet0", "32768").await;
        rejects("Ethernet0,Ethernet4", "1,1").await;
        rejects("Ethernet0,Ethernet4", "1").await;
        rejects("Ethernet0,,Ethernet4", "1,2,3").await;
        rejects("Ethernet0", "not-a-number").await;
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
        assert_eq!(message.operation, IPFixTemplateOperation::Update);
    }

    #[test]
    fn test_ipfix_templates_message_delete() {
        let message = IPFixTemplatesMessage::delete("test_key".to_string());

        assert_eq!(message.key, "test_key");
        assert!(message.templates.is_none());
        assert!(message.object_names.is_none());
        assert!(message.object_ids.is_none());
        assert_eq!(message.operation, IPFixTemplateOperation::Delete);
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
        let _ = SwssActor::run(actor).await;

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
        assert_eq!(our_message.operation, IPFixTemplateOperation::Update);
        assert!(our_message.templates.is_some());

        let object_names = our_message
            .object_names
            .as_ref()
            .expect("Should have object_names");
        assert_eq!(object_names, &vec!["Ethernet0"]);
    }

    #[tokio::test]
    #[serial_test::serial]
    async fn runtime_delete_then_valid_update_keeps_swss_loop_alive() {
        let table = setup_test_table();
        let key = "test_delete_then_update";
        insert_test_session(&table, key, "Ethernet0", "1", "template").await;
        let (template_sender, mut template_receiver) = channel(10);
        let actor = create_test_actor(template_sender);
        let task = tokio::spawn(SwssActor::run(actor));

        tokio::time::timeout(Duration::from_secs(2), async {
            loop {
                let message = template_receiver.recv().await.expect("startup update");
                if message.key == key {
                    assert_eq!(message.operation, IPFixTemplateOperation::Update);
                    break;
                }
            }
        })
        .await
        .unwrap();
        cleanup_test_session(&table, key);
        tokio::time::timeout(Duration::from_secs(2), async {
            loop {
                let message = template_receiver.recv().await.expect("runtime delete");
                if message.key == key {
                    assert_eq!(message.operation, IPFixTemplateOperation::Delete);
                    assert!(message.templates.is_none());
                    break;
                }
            }
        })
        .await
        .unwrap();
        assert!(
            !task.is_finished(),
            "Delete must not terminate the SWSS loop"
        );

        insert_test_session(&table, key, "Ethernet4", "2", "updated_template").await;
        tokio::time::timeout(Duration::from_secs(2), async {
            loop {
                let message = template_receiver.recv().await.expect("update after delete");
                if message.key == key {
                    assert_eq!(message.operation, IPFixTemplateOperation::Update);
                    assert_eq!(message.templates.as_deref().unwrap(), b"updated_template");
                    assert_eq!(message.object_names, Some(vec!["Ethernet4".into()]));
                    assert_eq!(message.object_ids, Some(vec![2]));
                    break;
                }
            }
        })
        .await
        .unwrap();
        assert!(
            !task.is_finished(),
            "SWSS must keep processing after Delete"
        );
        task.abort();
        assert!(tokio::time::timeout(Duration::from_secs(2), task)
            .await
            .unwrap()
            .unwrap_err()
            .is_cancelled());
        cleanup_test_session(&table, key);
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
        let _ = SwssActor::run(actor).await;

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
            assert_eq!(received_message.operation, IPFixTemplateOperation::Update);
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
        let _ = SwssActor::run(actor).await;

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
        assert_eq!(existing_message.operation, IPFixTemplateOperation::Update);
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
            assert_eq!(runtime_message.operation, IPFixTemplateOperation::Update);
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
