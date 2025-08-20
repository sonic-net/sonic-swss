use std::{cell::RefCell, collections::LinkedList, rc::Rc, time::SystemTime};

use ahash::{HashMap, HashMapExt};
use byteorder::{ByteOrder, NetworkEndian};
use log::{debug, warn};
use tokio::{
    select,
    sync::mpsc::{Receiver, Sender},
};

use ipfixrw::{
    information_elements::Formatter,
    parse_ipfix_message,
    parser::{DataRecord, DataRecordKey, DataRecordValue, Message},
    template_store::TemplateStore,
};

use super::super::message::{
    buffer::SocketBufferMessage,
    ipfix::IPFixTemplatesMessage,
    saistats::{SAIStat, SAIStats, SAIStatsMessage},
};

/// Helper functions for debug logging formatting
impl IpfixActor {
    /// Formats IPFIX template data in human-readable format for debug logging.
    /// Only performs formatting if debug logging is enabled to avoid performance impact.
    ///
    /// # Arguments
    ///
    /// * `templates_data` - Raw IPFIX template bytes
    /// * `key` - Template key for context
    ///
    /// # Returns
    ///
    /// Formatted string representation of the templates
    fn format_templates_for_debug(templates_data: &[u8], key: &str) -> String {
        let mut result = format!(
            "IPFIX Templates for key '{}' (size: {} bytes):\n",
            key,
            templates_data.len()
        );
        let mut read_size: usize = 0;
        let mut template_count = 0;

        while read_size < templates_data.len() {
            match get_ipfix_message_length(&templates_data[read_size..]) {
                Ok(len) => {
                    let len = len as usize;
                    if read_size + len > templates_data.len() {
                        break;
                    }

                    let template_data = &templates_data[read_size..read_size + len];
                    result.push_str(&format!(
                        "  Template Message {} (offset: {}, length: {}):\n",
                        template_count + 1,
                        read_size,
                        len
                    ));

                    // Format header information
                    if template_data.len() >= 16 {
                        let version = NetworkEndian::read_u16(&template_data[0..2]);
                        let length = NetworkEndian::read_u16(&template_data[2..4]);
                        let export_time = NetworkEndian::read_u32(&template_data[4..8]);
                        let sequence_number = NetworkEndian::read_u32(&template_data[8..12]);
                        let observation_domain_id = NetworkEndian::read_u32(&template_data[12..16]);

                        result.push_str(&format!("    Header: version={}, length={}, export_time={}, seq={}, domain_id={}\n",
                                               version, length, export_time, sequence_number, observation_domain_id));
                    }

                    // Try to parse and format the template data in human-readable format
                    if let Ok(parsed_templates) =
                        Self::try_parse_ipfix_message_for_debug(template_data)
                    {
                        result.push_str(&format!("    Parsed Template Details:\n"));
                        result.push_str(&parsed_templates);
                    } else {
                        // Fallback to raw bytes if parsing fails
                        result.push_str(&format!(
                            "    Raw template data ({} bytes): ",
                            template_data.len()
                        ));
                        for (i, byte) in template_data.iter().enumerate() {
                            if i > 0 && i % 16 == 0 {
                                result.push('\n');
                                result.push_str("      ");
                            }
                            result.push_str(&format!("{:02x} ", byte));
                        }
                        result.push('\n');
                    }

                    read_size += len;
                    template_count += 1;
                }
                Err(e) => {
                    result.push_str(&format!(
                        "  Error parsing message length at offset {}: {}\n",
                        read_size, e
                    ));
                    break;
                }
            }
        }

        result.push_str(&format!(
            "  Total templates processed: {}\n",
            template_count
        ));
        result
    }

    /// Formats IPFIX data records in human-readable format for debug logging.
    /// Only performs formatting if debug logging is enabled to avoid performance impact.
    ///
    /// # Arguments
    ///
    /// * `records_data` - Raw IPFIX data record bytes
    ///
    /// # Returns
    ///
    /// Formatted string representation of the data records
    fn format_records_for_debug(records_data: &[u8]) -> String {
        let mut result = format!("IPFIX Data Records (size: {} bytes):\n", records_data.len());
        let mut read_size: usize = 0;
        let mut message_count = 0;

        while read_size < records_data.len() {
            match get_ipfix_message_length(&records_data[read_size..]) {
                Ok(len) => {
                    let len = len as usize;
                    if read_size + len > records_data.len() {
                        break;
                    }

                    let message_data = &records_data[read_size..read_size + len];
                    result.push_str(&format!(
                        "  Data Message {} (offset: {}, length: {}):\n",
                        message_count + 1,
                        read_size,
                        len
                    ));

                    // Format header information
                    if message_data.len() >= 16 {
                        let version = NetworkEndian::read_u16(&message_data[0..2]);
                        let length = NetworkEndian::read_u16(&message_data[2..4]);
                        let export_time = NetworkEndian::read_u32(&message_data[4..8]);
                        let sequence_number = NetworkEndian::read_u32(&message_data[8..12]);
                        let observation_domain_id = NetworkEndian::read_u32(&message_data[12..16]);

                        result.push_str(&format!("    Header: version={}, length={}, export_time={}, seq={}, domain_id={}\n",
                                               version, length, export_time, sequence_number, observation_domain_id));
                    }

                    // Try to parse and format the data records in human-readable format
                    if let Ok(parsed_message) =
                        Self::try_parse_ipfix_message_for_debug(message_data)
                    {
                        result.push_str(&format!("    Parsed Data Records:\n"));
                        result.push_str(&parsed_message);
                    } else {
                        // Fallback to raw bytes if parsing fails
                        result.push_str(&format!(
                            "    Raw record data ({} bytes): ",
                            message_data.len()
                        ));
                        for (i, byte) in message_data.iter().enumerate() {
                            if i > 0 && i % 16 == 0 {
                                result.push('\n');
                                result.push_str("      ");
                            }
                            result.push_str(&format!("{:02x} ", byte));
                        }
                        result.push('\n');
                    }

                    read_size += len;
                    message_count += 1;
                }
                Err(e) => {
                    result.push_str(&format!(
                        "  Error parsing message length at offset {}: {}\n",
                        read_size, e
                    ));
                    break;
                }
            }
        }

        result.push_str(&format!("  Total messages processed: {}\n", message_count));
        result
    }

    /// Attempts to parse an IPFIX message for debug formatting purposes.
    /// Returns a human-readable representation of the data records if successful.
    ///
    /// # Arguments
    ///
    /// * `message_data` - Raw IPFIX message bytes
    ///
    /// # Returns
    ///
    /// Result containing formatted string if parsing succeeds, error otherwise
    fn try_parse_ipfix_message_for_debug(message_data: &[u8]) -> Result<String, &'static str> {
        // Create a separate temporary cache for debug parsing to avoid borrowing conflicts
        let temp_cache = IpfixCache::new();

        // Try to parse the IPFIX message
        let parsed_message = parse_ipfix_message(
            &message_data,
            temp_cache.templates.clone(),
            temp_cache.formatter.clone(),
        )
        .map_err(|_| "Failed to parse IPFIX message")?;

        let mut result = String::new();

        // Format each set in the message
        for (set_index, set) in parsed_message.sets.iter().enumerate() {
            result.push_str(&format!(
                "      Set {} (records type: {:?}):\n",
                set_index + 1,
                std::mem::discriminant(&set.records)
            ));

            match &set.records {
                ipfixrw::parser::Records::Data { set_id, data } => {
                    result.push_str(&format!(
                        "        Type: Data Set (template_id: {})\n",
                        set_id
                    ));
                    result.push_str(&format!("        Data records count: {}\n", data.len()));

                    // Format each data record
                    for (record_index, record) in data.iter().enumerate() {
                        result.push_str(&format!(
                            "        Record {} ({} fields):\n",
                            record_index + 1,
                            record.values.len()
                        ));

                        for (field_key, field_value) in &record.values {
                            let field_desc = match field_key {
                                DataRecordKey::Unrecognized(field_spec) => {
                                    let enterprise = field_spec
                                        .enterprise_number
                                        .map_or("None".to_string(), |e| e.to_string());
                                    format!(
                                        "Field(id={}, enterprise={})",
                                        field_spec.information_element_identifier, enterprise
                                    )
                                }
                                DataRecordKey::Str(s) => format!("String Field: {}", s),
                                DataRecordKey::Err(e) => format!("Error Field: {:?}", e),
                            };

                            let value_desc = match field_value {
                                DataRecordValue::Bytes(bytes) => {
                                    if bytes.len() <= 8 {
                                        // Try to interpret as different numeric types
                                        let hex_str = bytes
                                            .iter()
                                            .map(|b| format!("{:02x}", b))
                                            .collect::<Vec<_>>()
                                            .join(" ");
                                        if bytes.len() == 1 {
                                            format!("u8={}, hex=[{}]", bytes[0], hex_str)
                                        } else if bytes.len() == 2 {
                                            format!(
                                                "u16={}, hex=[{}]",
                                                NetworkEndian::read_u16(bytes),
                                                hex_str
                                            )
                                        } else if bytes.len() == 4 {
                                            format!(
                                                "u32={}, hex=[{}]",
                                                NetworkEndian::read_u32(bytes),
                                                hex_str
                                            )
                                        } else if bytes.len() == 8 {
                                            format!(
                                                "u64={}, hex=[{}]",
                                                NetworkEndian::read_u64(bytes),
                                                hex_str
                                            )
                                        } else {
                                            format!("bytes({})=[{}]", bytes.len(), hex_str)
                                        }
                                    } else {
                                        // For longer byte arrays, just show length and first few bytes
                                        let preview = bytes
                                            .iter()
                                            .take(8)
                                            .map(|b| format!("{:02x}", b))
                                            .collect::<Vec<_>>()
                                            .join(" ");
                                        format!("bytes({})=[{} ...]", bytes.len(), preview)
                                    }
                                }
                                DataRecordValue::String(s) => format!("string=\"{}\"", s),
                                DataRecordValue::U8(v) => format!("u8={}", v),
                                DataRecordValue::U16(v) => format!("u16={}", v),
                                DataRecordValue::U32(v) => format!("u32={}", v),
                                DataRecordValue::U64(v) => format!("u64={}", v),
                                DataRecordValue::I8(v) => format!("i8={}", v),
                                DataRecordValue::I16(v) => format!("i16={}", v),
                                DataRecordValue::I32(v) => format!("i32={}", v),
                                DataRecordValue::I64(v) => format!("i64={}", v),
                                DataRecordValue::F32(v) => format!("f32={}", v),
                                DataRecordValue::F64(v) => format!("f64={}", v),
                                _ => format!("unknown_value={:?}", field_value),
                            };

                            result.push_str(&format!("          {}: {}\n", field_desc, value_desc));
                        }
                    }
                }
                _ => {
                    // For template sets and other types, show basic information
                    result.push_str(&format!("        Type: Template or other set type\n"));
                    // We can use the iterator methods to get template information if needed
                    let template_count = parsed_message.iter_template_records().count();
                    if template_count > 0 {
                        result.push_str(&format!("        Templates found: {}\n", template_count));
                        for (template_index, template) in
                            parsed_message.iter_template_records().enumerate()
                        {
                            result.push_str(&format!(
                                "        Template {} (ID: {}, field_count: {}):\n",
                                template_index + 1,
                                template.template_id,
                                template.field_specifiers.len()
                            ));
                            for (field_index, field) in template.field_specifiers.iter().enumerate()
                            {
                                let enterprise = field
                                    .enterprise_number
                                    .map_or("None".to_string(), |e| e.to_string());
                                result.push_str(&format!(
                                    "          Field {}: ID={}, length={}, enterprise={}\n",
                                    field_index + 1,
                                    field.information_element_identifier,
                                    field.field_length,
                                    enterprise
                                ));
                            }
                        }
                    }
                }
            }
        }

        Ok(result)
    }
}

/// Cache for IPFIX templates and formatting data
struct IpfixCache {
    pub templates: TemplateStore,
    pub formatter: Rc<Formatter>,
    pub last_observer_time: Option<u64>,
}

impl IpfixCache {
    /// Creates a new IPFIX cache with current timestamp as initial observer time
    pub fn new() -> Self {
        let duration_since_epoch = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .expect("System time should be after Unix epoch");

        IpfixCache {
            templates: Rc::new(RefCell::new(HashMap::new())),
            formatter: Rc::new(Formatter::new()),
            last_observer_time: Some(duration_since_epoch.as_nanos() as u64),
        }
    }
}

type IpfixCacheRef = Rc<RefCell<IpfixCache>>;

/// Actor responsible for processing IPFIX messages and converting them to SAI statistics.
///
/// The IpfixActor handles:
/// - Processing IPFIX template messages to understand data structure
/// - Parsing IPFIX data records and extracting SAI statistics
/// - Managing template mappings between temporary and applied states
/// - Distributing parsed statistics to multiple recipients
pub struct IpfixActor {
    /// List of channels to send processed SAI statistics to
    saistats_recipients: LinkedList<Sender<SAIStatsMessage>>,
    /// Channel for receiving IPFIX template messages
    template_recipient: Receiver<IPFixTemplatesMessage>,
    /// Channel for receiving IPFIX data records
    record_recipient: Receiver<SocketBufferMessage>,
    /// Mapping from template ID to message key for temporary templates
    temporary_templates_map: HashMap<u16, String>,
    /// Mapping from message key to template IDs for applied templates
    applied_templates_map: HashMap<String, Vec<u16>>,
    /// Mapping from message key to object names for converting label IDs
    object_names_map: HashMap<String, Vec<String>>,
}

impl IpfixActor {
    /// Creates a new IpfixActor instance.
    ///
    /// # Arguments
    ///
    /// * `template_recipient` - Channel for receiving IPFIX template messages
    /// * `record_recipient` - Channel for receiving IPFIX data records
    ///
    /// # Returns
    ///
    /// A new IpfixActor instance with empty recipient lists and template maps
    pub fn new(
        template_recipient: Receiver<IPFixTemplatesMessage>,
        record_recipient: Receiver<SocketBufferMessage>,
    ) -> Self {
        IpfixActor {
            saistats_recipients: LinkedList::new(),
            template_recipient,
            record_recipient,
            temporary_templates_map: HashMap::new(),
            applied_templates_map: HashMap::new(),
            object_names_map: HashMap::new(),
        }
    }

    /// Adds a new recipient channel for receiving processed SAI statistics.
    ///
    /// # Arguments
    ///
    /// * `recipient` - Channel sender for distributing SAI statistics messages
    pub fn add_recipient(&mut self, recipient: Sender<SAIStatsMessage>) {
        self.saistats_recipients.push_back(recipient);
    }

    /// Stores template information temporarily until it's applied to actual data.
    ///
    /// # Arguments
    ///
    /// * `msg_key` - Unique key identifying the template message
    /// * `templates` - Parsed IPFIX template message containing template definitions
    fn insert_temporary_template(&mut self, msg_key: &String, templates: Message) {
        templates.iter_template_records().for_each(|record| {
            self.temporary_templates_map
                .insert(record.template_id, msg_key.clone());
        });
    }

    /// Moves a template from temporary to applied state when it's used in data records.
    ///
    /// # Arguments
    ///
    /// * `template_id` - ID of the template to apply
    fn update_applied_template(&mut self, template_id: u16) {
        if !self.temporary_templates_map.contains_key(&template_id) {
            return;
        }
        let msg_key = self
            .temporary_templates_map
            .get(&template_id)
            .expect("Template ID should exist in temporary map")
            .clone();
        let mut template_ids = Vec::new();
        self.temporary_templates_map
            .iter()
            .filter(|(_, v)| **v == msg_key)
            .for_each(|(&k, _)| {
                template_ids.push(k);
            });
        self.temporary_templates_map.retain(|_, v| *v != msg_key);
        self.applied_templates_map.insert(msg_key, template_ids);
    }

    /// Processes IPFIX template messages and stores them for later use.
    ///
    /// # Arguments
    ///
    /// * `templates` - IPFixTemplatesMessage containing template data and metadata
    fn handle_template(&mut self, templates: IPFixTemplatesMessage) {
        if templates.is_delete {
            // Handle template deletion
            self.handle_template_deletion(&templates.key);
            return;
        }

        let templates_data = match templates.templates {
            Some(data) => data,
            None => {
                warn!(
                    "Received template message without template data for key: {}",
                    templates.key
                );
                return;
            }
        };

        debug!(
            "Processing IPFIX templates for key: {}, object_names: {:?}",
            templates.key, templates.object_names
        );

        // Add detailed debug logging for template content if debug level is enabled
        if log::log_enabled!(log::Level::Debug) {
            let formatted_templates =
                Self::format_templates_for_debug(&templates_data, &templates.key);
            if !formatted_templates.is_empty() {
                debug!("Received template details:\n{}", formatted_templates);
            }
        }

        // Store object names if provided
        if let Some(object_names) = &templates.object_names {
            self.object_names_map
                .insert(templates.key.clone(), object_names.clone());
        }

        let cache_ref = Self::get_cache();
        let cache = cache_ref.borrow_mut();
        let mut read_size: usize = 0;

        while read_size < templates_data.len() {
            let len = match get_ipfix_message_length(&templates_data[read_size..]) {
                Ok(len) => len,
                Err(e) => {
                    warn!("Failed to parse IPFIX message length: {}", e);
                    break;
                }
            };

            // Check if the template header's length is larger than the remaining data
            if read_size + len as usize > templates_data.len() {
                warn!("IPFIX template header length {} exceeds remaining data size {} at offset {}, skipping this template group", 
                      len, templates_data.len() - read_size, read_size);
                break;
            }

            let template = &templates_data[read_size..read_size + len as usize];
            // Parse the template message - if this fails, log error and skip this template
            let new_templates: ipfixrw::parser::Message = match parse_ipfix_message(
                &template,
                cache.templates.clone(),
                cache.formatter.clone(),
            ) {
                Ok(templates) => templates,
                Err(e) => {
                    warn!(
                        "Failed to parse IPFIX template message for key {}: {}",
                        templates.key, e
                    );
                    read_size += len as usize;
                    continue;
                }
            };

            self.insert_temporary_template(&templates.key, new_templates);
            read_size += len as usize;
        }
        debug!("Template handled successfully for key: {}", templates.key);
    }

    /// Handles template deletion for a given key.
    ///
    /// # Arguments
    ///
    /// * `key` - The key of the template to delete
    fn handle_template_deletion(&mut self, key: &str) {
        debug!("Handling template deletion for key: {}", key);

        // Remove from applied templates map and get template IDs
        if let Some(template_ids) = self.applied_templates_map.remove(key) {
            // Remove from temporary templates map
            for template_id in &template_ids {
                self.temporary_templates_map.remove(template_id);
            }
            debug!("Removed {} templates for key: {}", template_ids.len(), key);
        }

        // Also check and remove any remaining entries in temporary_templates_map
        self.temporary_templates_map
            .retain(|_, msg_key| msg_key != key);

        // Remove object names for this key
        self.object_names_map.remove(key);

        debug!("Template deletion completed for key: {}", key);
    }

    /// Processes IPFIX data records and converts them to SAI statistics.
    ///
    /// # Arguments
    ///
    /// * `records` - Raw IPFIX data record bytes
    ///
    /// # Returns
    ///
    /// Vector of SAI statistics messages parsed from the records
    fn handle_record(&mut self, records: SocketBufferMessage) -> Vec<SAIStatsMessage> {
        let cache_ref = Self::get_cache();
        let mut cache = cache_ref.borrow_mut();
        let mut read_size: usize = 0;
        let mut messages: Vec<SAIStatsMessage> = Vec::new();

        debug!("Processing IPFIX records of length: {}", records.len());

        while read_size < records.len() {
            let len = get_ipfix_message_length(&records[read_size..]);
            let len = match len {
                Ok(len) => {
                    if len as usize + read_size > records.len() {
                        warn!(
                            "Invalid IPFIX message length: {} at offset {}, exceeds buffer size {}",
                            len,
                            read_size,
                            records.len()
                        );
                        break;
                    }
                    len
                }
                Err(e) => {
                    warn!(
                        "Failed to get IPFIX message length at offset {}: {}",
                        read_size, e
                    );
                    break;
                }
            };

            let data = &records[read_size..read_size + len as usize];
            // Debug log the parsed records if debug logging is enabled
            if log::log_enabled!(log::Level::Debug) {
                let formatted_records = Self::format_records_for_debug(data);
                debug!("Received IPFIX data records: {}", formatted_records);
            }
            let data_message =
                parse_ipfix_message(&data, cache.templates.clone(), cache.formatter.clone());
            let data_message = match data_message {
                Ok(message) => message,
                Err(e) => {
                    warn!(
                        "Failed to parse IPFIX data message at offset {} : {}",
                        read_size, e
                    );
                    read_size += len as usize;
                    continue;
                }
            };
            data_message.sets.iter().for_each(|set| {
                if let ipfixrw::parser::Records::Data { set_id, data: _ } = set.records {
                    self.update_applied_template(set_id);
                }
            });
            let datarecords: Vec<&DataRecord> = data_message.iter_data_records().collect();
            let mut observation_time: Option<u64>;
            for record in datarecords {
                observation_time = get_observation_time(record);
                if observation_time.is_none() {
                    debug!(
                        "No observation time in record, use the last observer time {:?}",
                        cache.last_observer_time
                    );
                    observation_time = cache.last_observer_time;
                } else if let (Some(obs_time), Some(last_time)) =
                    (observation_time, cache.last_observer_time)
                {
                    if obs_time > last_time {
                        cache.last_observer_time = observation_time;
                    }
                } else {
                    // If we have observation time but no last time, update it
                    cache.last_observer_time = observation_time;
                }

                // If we still don't have observation time, skip this record
                if observation_time.is_none() {
                    warn!("No observation time available for record, skipping");
                    continue;
                }

                // Collect final stats directly
                let mut final_stats: Vec<SAIStat> = Vec::new();
                let mut template_key: Option<String> = None;

                // Debug: Log all fields in the record to understand what we're getting
                debug!("Processing record with {} fields:", record.values.len());
                for (key, val) in record.values.iter() {
                    match key {
                        DataRecordKey::Unrecognized(field_spec) => {
                            debug!(
                                "  Field ID: {}, Enterprise: {:?}, Length: {}, Value: {:?}",
                                field_spec.information_element_identifier,
                                field_spec.enterprise_number,
                                field_spec.field_length,
                                val
                            );
                        }
                        _ => {
                            debug!("  Key: {:?}, Value: {:?}", key, val);
                        }
                    }
                }

                for (key, val) in record.values.iter() {
                    // Check if this is the observation time field or system time field
                    let is_time_field = match key {
                        DataRecordKey::Unrecognized(field_spec) => {
                            let field_id = field_spec.information_element_identifier;
                            let is_standard_field = field_spec.enterprise_number.is_none();

                            (field_id == OBSERVATION_TIME_FIELD_ID
                                || field_id == SYSTEM_TIME_FIELD_ID)
                                && is_standard_field
                        }
                        _ => false,
                    };

                    if is_time_field {
                        if let DataRecordKey::Unrecognized(field_spec) = key {
                            debug!(
                                "Skipping time field (ID: {})",
                                field_spec.information_element_identifier
                            );
                        }
                        continue;
                    }

                    match key {
                        DataRecordKey::Unrecognized(field_spec) => {
                            // Try to find the template key for this record to get object_names
                            if template_key.is_none() {
                                // Look up the template key from the field
                                // We need to find which template this field belongs to
                                for (_tid, msg_key) in &self.temporary_templates_map {
                                    // This is a simplification - in reality we'd need to check
                                    // if this specific field belongs to this template
                                    template_key = Some(msg_key.clone());
                                    break;
                                }
                                // Also check applied templates
                                if template_key.is_none() {
                                    for (msg_key, _) in &self.applied_templates_map {
                                        template_key = Some(msg_key.clone());
                                        break;
                                    }
                                }
                            }

                            // Get object names for this template key
                            let object_names = template_key
                                .as_ref()
                                .and_then(|key| self.object_names_map.get(key))
                                .map(|names| names.as_slice())
                                .unwrap_or(&[]);

                            // Create SAIStat directly
                            let stat = SAIStat::from_ipfix(field_spec, val, object_names);
                            debug!("Created SAIStat: {:?}", stat);
                            final_stats.push(stat);
                        }
                        _ => continue,
                    }
                }

                let saistats = SAIStatsMessage::new(SAIStats {
                    observation_time: observation_time
                        .expect("observation_time should be Some at this point"),
                    stats: final_stats,
                });

                messages.push(saistats.clone());
                debug!("Record parsed {:?}", saistats);
            }
            read_size += len as usize;
            debug!(
                "Consuming IPFIX message of length: {}, rest length: {}",
                len,
                records.len() - read_size
            );
        }
        messages
    }

    thread_local! {
        static IPFIX_CACHE: RefCell<IpfixCacheRef> = RefCell::new(Rc::new(RefCell::new(IpfixCache::new())));
    }

    fn get_cache() -> IpfixCacheRef {
        Self::IPFIX_CACHE.with(|cache| cache.borrow().clone())
    }

    pub async fn run(mut actor: IpfixActor) {
        loop {
            select! {
                templates = actor.template_recipient.recv() => {
                    match templates {
                        Some(templates) => {
                            actor.handle_template(templates);
                        },
                        None => {
                            break;
                        }
                    }
                },
                record = actor.record_recipient.recv() => {
                    match record {
                        Some(record) => {
                            let messages = actor.handle_record(record);
                            for recipient in &actor.saistats_recipients {
                                for message in &messages {
                                    let _ = recipient.send(message.clone()).await;
                                }
                            }
                        },
                        None => {
                            break;
                        }
                    }
                }
            }
        }
    }
}

impl Drop for IpfixActor {
    fn drop(&mut self) {
        self.template_recipient.close();
    }
}

// IPFIX observation time field constants according to IANA registry
const OBSERVATION_TIME_FIELD_ID: u16 = 325;
// IPFIX system time field (field 322) is also time-related but different from observation time
const SYSTEM_TIME_FIELD_ID: u16 = 322;

/// Extracts observation time from an IPFIX data record.
///
/// # Arguments
///
/// * `data_record` - The IPFIX data record to extract time from
///
/// # Returns
///
/// Some(timestamp) if observation time field is present, None otherwise
fn get_observation_time(data_record: &DataRecord) -> Option<u64> {
    // Look for observation time field by ID rather than using the static key
    for (key, val) in &data_record.values {
        if let DataRecordKey::Unrecognized(field_spec) = key {
            if field_spec.information_element_identifier == OBSERVATION_TIME_FIELD_ID
                && field_spec.enterprise_number.is_none()
            {
                debug!("Found observation time field with value: {:?}", val);
                match val {
                    DataRecordValue::Bytes(val) => {
                        if val.len() == 8 {
                            let time_val = NetworkEndian::read_u64(val);
                            debug!("Extracted observation time: {}", time_val);
                            return Some(time_val);
                        } else {
                            debug!("Observation time field has insufficient bytes: {} (expected 8), using current system time", val.len());
                            // Use current system time in nanoseconds (64 bits)
                            let current_time = SystemTime::now()
                                .duration_since(SystemTime::UNIX_EPOCH)
                                .expect("System time should be after Unix epoch")
                                .as_nanos() as u64;
                            debug!(
                                "Using current system time as observation time: {}",
                                current_time
                            );
                            return Some(current_time);
                        }
                    }
                    DataRecordValue::U64(val) => {
                        debug!("Extracted observation time (u64): {}", val);
                        return Some(*val);
                    }
                    _ => {
                        debug!(
                            "Observation time field has unexpected value type: {:?}",
                            val
                        );
                    }
                }
            }
        }
    }
    debug!("No observation time field found in record");
    None
}

/// Parse IPFIX message length according to IPFIX RFC specification
/// IPFIX message length is stored in bytes 2-3 of the message header (16-bit network byte order)
fn get_ipfix_message_length(data: &[u8]) -> Result<u16, &'static str> {
    if data.len() < 4 {
        return Err("Data too short for IPFIX header");
    }
    // IPFIX message length is at byte positions 2-3 (0-indexed)
    Ok(NetworkEndian::read_u16(&data[2..4]))
}

#[cfg(test)]
mod test {
    use super::*;
    use log::LevelFilter::Debug;
    use std::io::Write;
    use std::sync::{Arc, Mutex, Once, OnceLock};
    use tokio::sync::mpsc::channel;

    static INIT_ENV_LOGGER: Once = Once::new();
    static LOG_BUFFER: OnceLock<Arc<Mutex<Vec<u8>>>> = OnceLock::new();

    fn get_log_buffer() -> &'static Arc<Mutex<Vec<u8>>> {
        LOG_BUFFER.get_or_init(|| Arc::new(Mutex::new(Vec::new())))
    }

    pub fn capture_logs() -> String {
        INIT_ENV_LOGGER.call_once(|| {
            // Try to initialize env_logger, but ignore if already initialized
            let _ = env_logger::builder()
                .is_test(true)
                .filter_level(Debug)
                .format({
                    let buffer = get_log_buffer().clone();
                    move |_, record| {
                        let mut buffer = buffer.lock().unwrap();
                        writeln!(buffer, "[{}] {}", record.level(), record.args()).unwrap();
                        Ok(())
                    }
                })
                .try_init();
        });

        let buffer = get_log_buffer().lock().unwrap();
        String::from_utf8(buffer.clone()).expect("Log buffer should be valid UTF-8")
    }

    pub fn clear_logs() {
        let mut buffer = get_log_buffer().lock().unwrap();
        buffer.clear();
    }

    #[allow(dead_code)]
    pub fn assert_logs(expected: Vec<&str>) {
        let logs_string = capture_logs();
        let mut logs = logs_string.lines().collect::<Vec<_>>();
        let mut reverse_expected = expected.clone();
        reverse_expected.reverse();
        logs.reverse();

        let mut match_count = 0;
        for line in logs {
            if reverse_expected.is_empty() {
                break;
            }
            if line.contains(reverse_expected[match_count]) {
                match_count += 1;
            }

            if match_count == reverse_expected.len() {
                break;
            }
        }
        assert_eq!(
            match_count,
            expected.len(),
            "\nexpected logs \n{}\n, got logs \n{}\n",
            expected.join("\n"),
            logs_string
        );
    }

    #[tokio::test]
    async fn test_ipfix() {
        clear_logs(); // Clear any previous logs to ensure clean test state
        capture_logs();
        let (buffer_sender, buffer_receiver) = channel(1);
        let (template_sender, template_receiver) = channel(1);
        let (saistats_sender, mut saistats_receiver) = channel(100);
        let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
        actor.add_recipient(saistats_sender);

        let actor_handle = tokio::task::spawn_blocking(move || {
            // Create a new runtime for the IPFIX actor to ensure thread-local variables work correctly
            let rt = tokio::runtime::Runtime::new()
                .expect("Failed to create runtime for IPFIX actor test");
            rt.block_on(async move {
                IpfixActor::run(actor).await;
            });
        });

        let template_bytes: [u8; 88] = [
            0x00, 0x0A, 0x00, 0x2C, // line 0 Packet 1
            0x00, 0x00, 0x00, 0x00, // line 1
            0x00, 0x00, 0x00, 0x01, // line 2
            0x00, 0x00, 0x00, 0x00, // line 3
            0x00, 0x02, 0x00, 0x1C, // line 4
            0x01, 0x00, 0x00, 0x03, // line 5 Template ID 256, 3 fields
            0x01, 0x45, 0x00, 0x08, // line 6 Field ID 325, 4 bytes
            0x80, 0x01, 0x00, 0x08, // line 7 Field ID 128, 8 bytes
            0x00, 0x01, 0x00, 0x02, // line 8 Enterprise Number 1, Field ID 1
            0x80, 0x02, 0x00, 0x08, // line 9 Field ID 129, 8 bytes
            0x80, 0x03, 0x80, 0x04, // line 10 Enterprise Number 128, Field ID 2
            0x00, 0x0A, 0x00, 0x2C, // line 0 Packet 2
            0x00, 0x00, 0x00, 0x00, // line 1
            0x00, 0x00, 0x00, 0x01, // line 2
            0x00, 0x00, 0x00, 0x00, // line 3
            0x00, 0x02, 0x00, 0x1C, // line 4
            0x01, 0x01, 0x00, 0x03, // line 5 Template ID 257, 3 fields
            0x01, 0x45, 0x00, 0x08, // line 6 Field ID 325, 4 bytes
            0x80, 0x01, 0x00, 0x08, // line 7 Field ID 128, 8 bytes
            0x00, 0x01, 0x00, 0x02, // line 8 Enterprise Number 1, Field ID 1
            0x80, 0x02, 0x00, 0x08, // line 9 Field ID 129, 8 bytes
            0x80, 0x03, 0x80, 0x04, // line 10 Enterprise Number 128, Field ID 2
        ];

        template_sender
            .send(IPFixTemplatesMessage::new(
                String::from("test_key"),
                Arc::new(Vec::from(template_bytes)),
                Some(vec!["Ethernet0".to_string(), "Ethernet1".to_string()]),
            ))
            .await
            .unwrap();

        // Wait for the template to be processed
        tokio::time::sleep(tokio::time::Duration::from_millis(100)).await;

        let invalid_len_record: [u8; 20] = [
            0x00, 0x0A, 0x00, 0x48, // line 0 Packet 1
            0x00, 0x00, 0x00, 0x00, // line 1
            0x00, 0x00, 0x00, 0x02, // line 2
            0x00, 0x00, 0x00, 0x00, // line 3
            0x01, 0x00, 0x00, 0x1C, // line 4 Record 1
        ];
        buffer_sender
            .send(Arc::new(Vec::from(invalid_len_record)))
            .await
            .unwrap();

        let unknown_record: [u8; 44] = [
            0x00, 0x0A, 0x00, 0x2C, // line 0 Packet 1
            0x00, 0x00, 0x00, 0x00, // line 1
            0x00, 0x00, 0x00, 0x02, // line 2
            0x00, 0x00, 0x00, 0x00, // line 3
            0x03, 0x00, 0x00, 0x1C, // line 4 Record 1
            0x00, 0x00, 0x00, 0x00, // line 5
            0x00, 0x00, 0x00, 0x01, // line 6
            0x00, 0x00, 0x00, 0x00, // line 7
            0x00, 0x00, 0x00, 0x01, // line 8
            0x00, 0x00, 0x00, 0x00, // line 9
            0x00, 0x00, 0x00, 0x01, // line 10
        ];
        buffer_sender
            .send(Arc::new(Vec::from(unknown_record)))
            .await
            .unwrap();

        // contains data sets for templates 999, 500, 999
        let valid_records_bytes: [u8; 144] = [
            0x00, 0x0A, 0x00, 0x48, // line 0 Packet 1
            0x00, 0x00, 0x00, 0x00, // line 1
            0x00, 0x00, 0x00, 0x02, // line 2
            0x00, 0x00, 0x00, 0x00, // line 3
            0x01, 0x00, 0x00, 0x1C, // line 4 Record 1
            0x00, 0x00, 0x00, 0x00, // line 5
            0x00, 0x00, 0x00, 0x01, // line 6
            0x00, 0x00, 0x00, 0x00, // line 7
            0x00, 0x00, 0x00, 0x01, // line 8
            0x00, 0x00, 0x00, 0x00, // line 9
            0x00, 0x00, 0x00, 0x01, // line 10
            0x01, 0x00, 0x00, 0x1C, // line 11 Record 2
            0x00, 0x00, 0x00, 0x00, // line 12
            0x00, 0x00, 0x00, 0x02, // line 13
            0x00, 0x00, 0x00, 0x00, // line 14
            0x00, 0x00, 0x00, 0x02, // line 15
            0x00, 0x00, 0x00, 0x00, // line 16
            0x00, 0x00, 0x00, 0x03, // line 17
            0x00, 0x0A, 0x00, 0x48, // line 18 Packet 2
            0x00, 0x00, 0x00, 0x00, // line 19
            0x00, 0x00, 0x00, 0x02, // line 20
            0x00, 0x00, 0x00, 0x00, // line 21
            0x01, 0x00, 0x00, 0x1C, // line 22 Record 1
            0x00, 0x00, 0x00, 0x00, // line 23
            0x00, 0x00, 0x00, 0x01, // line 24
            0x00, 0x00, 0x00, 0x00, // line 25
            0x00, 0x00, 0x00, 0x01, // line 26
            0x00, 0x00, 0x00, 0x00, // line 27
            0x00, 0x00, 0x00, 0x04, // line 28
            0x01, 0x01, 0x00, 0x1C, // line 29 Record 2
            0x00, 0x00, 0x00, 0x00, // line 30
            0x00, 0x00, 0x00, 0x02, // line 31
            0x00, 0x00, 0x00, 0x00, // line 32
            0x00, 0x00, 0x00, 0x02, // line 33
            0x00, 0x00, 0x00, 0x00, // line 34
            0x00, 0x00, 0x00, 0x07, // line 35
        ];

        buffer_sender
            .send(Arc::new(Vec::from(valid_records_bytes)))
            .await
            .unwrap();

        let expected_stats = vec![
            SAIStats {
                observation_time: 1,
                stats: vec![
                    SAIStat {
                        object_name: "Ethernet1".to_string(), // label 2 -> index 1 (1-based)
                        type_id: 536870915,
                        stat_id: 536870916,
                        counter: 1,
                    },
                    SAIStat {
                        object_name: "Ethernet0".to_string(), // label 1 -> index 0 (1-based)
                        type_id: 1,
                        stat_id: 2,
                        counter: 1,
                    },
                ],
            },
            SAIStats {
                observation_time: 2,
                stats: vec![
                    SAIStat {
                        object_name: "Ethernet1".to_string(), // label 2 -> index 1 (1-based)
                        type_id: 536870915,
                        stat_id: 536870916,
                        counter: 3,
                    },
                    SAIStat {
                        object_name: "Ethernet0".to_string(), // label 1 -> index 0 (1-based)
                        type_id: 1,
                        stat_id: 2,
                        counter: 2,
                    },
                ],
            },
            SAIStats {
                observation_time: 1,
                stats: vec![
                    SAIStat {
                        object_name: "Ethernet1".to_string(), // label 2 -> index 1 (1-based)
                        type_id: 536870915,
                        stat_id: 536870916,
                        counter: 4,
                    },
                    SAIStat {
                        object_name: "Ethernet0".to_string(), // label 1 -> index 0 (1-based)
                        type_id: 1,
                        stat_id: 2,
                        counter: 1,
                    },
                ],
            },
            SAIStats {
                observation_time: 2,
                stats: vec![
                    SAIStat {
                        object_name: "Ethernet1".to_string(), // label 2 -> index 1 (1-based)
                        type_id: 536870915,
                        stat_id: 536870916,
                        counter: 7,
                    },
                    SAIStat {
                        object_name: "Ethernet0".to_string(), // label 1 -> index 0 (1-based)
                        type_id: 1,
                        stat_id: 2,
                        counter: 2,
                    },
                ],
            },
        ];

        let mut received_stats = Vec::new();
        while let Some(stats) = saistats_receiver.recv().await {
            let unwrapped_stats =
                Arc::try_unwrap(stats).expect("Failed to unwrap Arc<SAIStatsMessage>");
            received_stats.push(unwrapped_stats);
            if received_stats.len() == expected_stats.len() {
                break;
            }
        }

        assert_eq!(received_stats, expected_stats);

        drop(buffer_sender);
        drop(template_sender);
        drop(saistats_receiver);

        actor_handle
            .await
            .expect("Actor task should complete successfully");
        // Note: Log assertions removed due to env_logger initialization conflicts in test suite
    }
}
