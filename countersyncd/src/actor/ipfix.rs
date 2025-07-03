use std::{
    cell::RefCell, 
    collections::LinkedList, 
    rc::Rc, 
    sync::Arc, 
    time::SystemTime
};

use ahash::{HashMap, HashMapExt};
use byteorder::{ByteOrder, NetworkEndian};
use log::{debug, warn};
use once_cell::sync::Lazy;
use tokio::{
    select,
    sync::mpsc::{Receiver, Sender},
};

use ipfixrw::{
    information_elements::Formatter,
    parse_ipfix_message,
    parser::{DataRecord, DataRecordKey, DataRecordValue, FieldSpecifier, Message},
    template_store::TemplateStore,
};

use super::super::message::{
    buffer::SocketBufferMessage,
    ipfix::IPFixTemplatesMessage,
    saistats::{SAIStat, SAIStats, SAIStatsMessage},
};

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
    /// * `templates` - Tuple containing message key and template data
    fn handle_template(&mut self, templates: IPFixTemplatesMessage) {
        let (msg_key, templates) = templates;
        let cache_ref = Self::get_cache();
        let cache = cache_ref.borrow_mut();
        let mut read_size: usize = 0;
        
        while read_size < templates.len() {
            let len = match get_ipfix_message_length(&templates[read_size..]) {
                Ok(len) => len,
                Err(e) => {
                    warn!("Failed to parse IPFIX message length: {}", e);
                    break;
                }
            };
            
            let template = &templates[read_size..read_size + len as usize];
            // Parse the template message - if this fails, it indicates a serious protocol error
            let new_templates: ipfixrw::parser::Message =
                parse_ipfix_message(&template, cache.templates.clone(), cache.formatter.clone())
                    .expect("IPFIX template should be valid according to protocol specification");
            
            self.insert_temporary_template(&msg_key, new_templates);
            read_size += len as usize;
        }
        debug!("Template handled successfully for key: {}", msg_key);
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
        while read_size < records.len() {
            let len = get_ipfix_message_length(&records[read_size..]);
            if len.is_err() || len.unwrap() as usize + read_size > records.len() {
                warn!("Wrong length in the records {:?}", records);
                break;
            }
            let len = len.unwrap();
            let data = &records[read_size..read_size + len as usize];
            let data_message =
                parse_ipfix_message(&data, cache.templates.clone(), cache.formatter.clone());
            if data_message.is_err() {
                warn!("Not support data message {:?}", data);
                read_size += len as usize;
                continue;
            }
            let data_message = data_message.unwrap();
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
                } else if observation_time.unwrap() > cache.last_observer_time.unwrap() {
                    cache.last_observer_time = observation_time;
                }
                let mut saistats = SAIStatsMessage::new(SAIStats {
                    observation_time: observation_time.unwrap(),
                    stats: Vec::new(),
                });
                for (key, val) in record.values.iter() {
                    if key == &*OBSERVATION_TIME_KEY {
                        // skip the observation time data
                        continue;
                    }
                    match key {
                        DataRecordKey::Unrecognized(key) => {
                            Arc::get_mut(&mut saistats)
                                .unwrap()
                                .stats
                                .push(SAIStat::from((key, val)));
                        }
                        _ => continue,
                    }
                }
                messages.push(saistats.clone());
                debug!("Record parsed {:?}", saistats);
            }
            read_size += len as usize;
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
const OBSERVATION_TIME_FIELD_LENGTH: u16 = 8;

/// Lazy-initialized key for observation time field used in IPFIX data records
static OBSERVATION_TIME_KEY: Lazy<DataRecordKey> =
    Lazy::new(|| DataRecordKey::Unrecognized(FieldSpecifier::new(
        None, 
        OBSERVATION_TIME_FIELD_ID, 
        OBSERVATION_TIME_FIELD_LENGTH
    )));

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
    let val = data_record.values.get(&*OBSERVATION_TIME_KEY);
    match val {
        Some(DataRecordValue::Bytes(val)) => Some(NetworkEndian::read_u64(val)),
        _ => None,
    }
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
    use countersyncd::test_common::{assert_logs, capture_logs};
    use tokio::{spawn, sync::mpsc::channel};

    #[tokio::test]
    async fn test_ipfix() {
        capture_logs();
        let (buffer_sender, buffer_receiver) = channel(1);
        let (template_sender, template_receiver) = channel(1);
        let (saistats_sender, mut saistats_receiver) = channel(100);
        let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
        actor.add_recipient(saistats_sender);

        let actor_handle = spawn(IpfixActor::run(actor));

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
            .send((String::from(""), Arc::new(Vec::from(template_bytes))))
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
                        label: 2,
                        type_id: 536870915,
                        stat_id: 536870916,
                        counter: 1,
                    },
                    SAIStat {
                        label: 1,
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
                        label: 2,
                        type_id: 536870915,
                        stat_id: 536870916,
                        counter: 3,
                    },
                    SAIStat {
                        label: 1,
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
                        label: 2,
                        type_id: 536870915,
                        stat_id: 536870916,
                        counter: 4,
                    },
                    SAIStat {
                        label: 1,
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
                        label: 2,
                        type_id: 536870915,
                        stat_id: 536870916,
                        counter: 7,
                    },
                    SAIStat {
                        label: 1,
                        type_id: 1,
                        stat_id: 2,
                        counter: 2,
                    },
                ],
            },
        ];

        let mut received_stats = Vec::new();
        while let Some(stats) = saistats_receiver.recv().await {
            let unwrapped_stats = Arc::try_unwrap(stats)
                .expect("Failed to unwrap Arc<SAIStatsMessage>");
            received_stats.push(unwrapped_stats);
            if received_stats.len() == expected_stats.len() {
                break;
            }
        }

        assert_eq!(received_stats, expected_stats);

        drop(buffer_sender);
        drop(template_sender);
        drop(saistats_receiver);

        actor_handle.await.expect("Actor task should complete successfully");
        assert_logs(vec![
            "[DEBUG] Template handle",
            "[WARN] Wrong length in the records",
            "[WARN] Not support data message",
            "[DEBUG] Record parsed",
            "[DEBUG] Record parsed",
            "[DEBUG] Record parsed",
            "[DEBUG] Record parsed",
        ]);
    }
}
