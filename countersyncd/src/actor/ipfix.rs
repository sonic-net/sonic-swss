use std::{cell::RefCell, collections::LinkedList, rc::Rc, sync::Arc, time::SystemTime};

use log::{debug, warn};
use tokio::{
    select,
    sync::mpsc::{Receiver, Sender},
};
use once_cell::sync::Lazy;

use ahash::{HashMap, HashMapExt};
use byteorder::{ByteOrder, NetworkEndian};
use ipfix::get_message_length;
use ipfixrw::{
    information_elements::Formatter,
    parse_ipfix_message,
    parser::{DataRecord, DataRecordKey, DataRecordValue, FieldSpecifier},
    template_store::TemplateStore,
};

use super::super::message::{
    buffer::SocketBufferMessage,
    ipfix::IPFixTemplates,
    saistats::{SAIStat, SAIStats, SAIStatsMessage},
};

struct IpfixCache {
    pub templates: TemplateStore,
    pub formatter: Rc<Formatter>,
    pub last_observer_time: Option<u64>,
}

impl IpfixCache {
    pub fn new() -> Self {
        let duration_since_epoch = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .unwrap();
        IpfixCache {
            templates: Rc::new(RefCell::new(HashMap::new())),
            formatter: Rc::new(Formatter::new()),
            last_observer_time: Some(duration_since_epoch.as_nanos() as u64),
        }
    }
}

type IpfixCacheRef = Rc<RefCell<IpfixCache>>;

pub struct IpfixActor {
    saistats_recipients: LinkedList<Sender<SAIStatsMessage>>,
    template_recipient: Receiver<IPFixTemplates>,
    record_recipient: Receiver<SocketBufferMessage>,

    #[cfg(test)]
    prober: Option<Sender<String>>,
}

impl IpfixActor {
    pub fn new(
        template_recipient: Receiver<IPFixTemplates>,
        record_recipient: Receiver<SocketBufferMessage>,
    ) -> Self {
        IpfixActor {
            saistats_recipients: LinkedList::new(),
            template_recipient,
            record_recipient,
            #[cfg(test)]
            prober: None,
        }
    }

    pub fn add_recipient(&mut self, recipient: Sender<SAIStatsMessage>) {
        self.saistats_recipients.push_back(recipient);
    }

    fn handle_template(&mut self, templates: IPFixTemplates) {
        let cache_ref = Self::get_cache();
        let cache = cache_ref.borrow_mut();
        let mut read_size: usize = 0;
        while read_size < templates.len() {
            let len = get_message_length(&templates[read_size..]).unwrap();
            let template = &templates[read_size..read_size + len as usize];
            // We suppose that the template is always valid, otherwise we need to raise the panic
            parse_ipfix_message(&template, cache.templates.clone(), cache.formatter.clone())
                .unwrap();
            read_size += len as usize;
        }
        #[cfg(test)]
        self.probe(format!("Template {:?} consumed", templates));
    }

    fn handle_record(&mut self, records: SocketBufferMessage) -> Vec<SAIStatsMessage> {
        let cache_ref = Self::get_cache();
        let mut cache = cache_ref.borrow_mut();
        let mut read_size: usize = 0;
        let mut messages: Vec<SAIStatsMessage> = Vec::new();
        while read_size < records.len() {
            let len = get_message_length(&records[read_size..]);
            if len.is_err() || len.unwrap() as usize + read_size > records.len() {
                warn!("Wrong length in the records {:?}", records);
                #[cfg(test)]
                self.probe("Discard record due to error length".to_string());
                break;
            }
            let len = len.unwrap();
            let data = &records[read_size..read_size + len as usize];
            let data_message =
                parse_ipfix_message(&data, cache.templates.clone(), cache.formatter.clone());
            if data_message.is_err() {
                warn!("Not support data message {:?}", data);
                read_size += len as usize;
                #[cfg(test)]
                self.probe(format!("Unknown template, Discard record {:?}", data_message.err().unwrap()));
                continue;
            }
            let data_message = data_message.unwrap();
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
                #[cfg(test)]
                self.probe("Record parsed".to_string());
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

    #[cfg(test)]
    pub fn set_prober(&mut self, prober: Sender<String>) {
        self.prober = Some(prober);
    }

    #[cfg(test)]
    fn probe(&self, message: String) {
        self.prober.as_ref().unwrap().try_send(message).unwrap();
    }
}

impl Drop for IpfixActor {
    fn drop(&mut self) {
        self.template_recipient.close();
    }
}

static OBSERVATION_TIME_KEY: Lazy<DataRecordKey> =
Lazy::new(|| DataRecordKey::Unrecognized(FieldSpecifier::new(None, 325, 8)));

fn get_observation_time(data_record: &DataRecord) -> Option<u64> {
    let val = data_record.values.get(&*OBSERVATION_TIME_KEY);
    match val {
        Some(DataRecordValue::Bytes(val)) => Some(NetworkEndian::read_u64(val)),
        _ => None,
    }
}

#[cfg(test)]
mod test {
    use super::*;

    use tokio::{spawn, sync::mpsc::channel};

    #[tokio::test]
    async fn test_ipfix() {
        let (buffer_sender, buffer_reciver) = channel(1);
        let (template_sender, template_reciver) = channel(1);
        let (saistats_sender, mut saistats_reciver) = channel(100);
        let (prober, mut prober_reciver) = channel(100);

        let mut actor = IpfixActor::new(template_reciver, buffer_reciver);
        actor.add_recipient(saistats_sender);
        actor.set_prober(prober);
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
            0x01, 0x00, 0x00, 0x03, // line 5 Template ID 256, 3 fields
            0x01, 0x45, 0x00, 0x08, // line 6 Field ID 325, 4 bytes
            0x80, 0x01, 0x00, 0x08, // line 7 Field ID 128, 8 bytes
            0x00, 0x01, 0x00, 0x02, // line 8 Enterprise Number 1, Field ID 1
            0x80, 0x02, 0x00, 0x08, // line 9 Field ID 129, 8 bytes
            0x80, 0x03, 0x80, 0x04, // line 10 Enterprise Number 128, Field ID 2
        ];

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
            0x01, 0x00, 0x00, 0x1C, // line 29 Record 2
            0x00, 0x00, 0x00, 0x00, // line 30
            0x00, 0x00, 0x00, 0x02, // line 31
            0x00, 0x00, 0x00, 0x00, // line 32
            0x00, 0x00, 0x00, 0x02, // line 33
            0x00, 0x00, 0x00, 0x00, // line 34
            0x00, 0x00, 0x00, 0x07, // line 35
        ];

        template_sender
            .send(Arc::new(Vec::from(template_bytes)))
            .await
            .unwrap();
        let pm = prober_reciver
            .recv()
            .await
            .expect("The template not consumed");
        assert!(pm.contains("Template"));
        assert!(pm.contains("consumed"));

        buffer_sender
            .send(Arc::new(Vec::from(valid_records_bytes)))
            .await
            .unwrap();
        for _ in 0..4 {
            let pm = prober_reciver
                .recv()
                .await
                .expect("The record not consumed");
            assert!(pm.contains("Record"));
            assert!(pm.contains("parsed"));
        }

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
        assert!(prober_reciver
            .recv()
            .await
            .expect("The record not consumed")
            .contains("Discard record due to error length"));

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
        assert!(prober_reciver
            .recv()
            .await
            .expect("The record not consumed")
            .contains("Unknown template, Discard record"));

        drop(buffer_sender);
        drop(template_sender);

        let mut received_stats = Vec::new();
        while let Some(stats) = saistats_reciver.recv().await {
            received_stats.push(Arc::try_unwrap(stats).unwrap());
        }

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
        assert_eq!(received_stats, expected_stats);

        drop(saistats_reciver);

        actor_handle.await.unwrap();
    }
}
