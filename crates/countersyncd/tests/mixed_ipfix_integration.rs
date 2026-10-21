//! Original per-group HFT publication through real Redis and unmodified actors.
use countersyncd::{
    actor::{ipfix::IpfixActor, swss::SwssActor},
    message::{
        buffer::SocketBufferMessage,
        ipfix::{IPFixTemplateOperation, IPFixTemplatesMessage},
        saistats::SAIStatsBatchMessage,
    },
};
use std::{
    sync::Arc,
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use swss_common::{CxxString, DbConnector, Table};
use tokio::{sync::mpsc, task::JoinSet, time::timeout};

const DEADLINE: Duration = Duration::from_secs(10);
const GROUPS: [(&str, u32); 4] = [
    ("PORT", 1),
    ("QUEUE", 21),
    ("INGRESS_PRIORITY_GROUP", 26),
    ("BUFFER_POOL", 24),
];
const WIDTHS: [u16; 4] = [8, 4, 4, 8];
const VALUES: [u64; 4] = [(1 << 40) + 11, 22, 33, (1 << 42) + 44];

struct Fixture {
    table: Table,
    keys: [String; 5],
    names: [[String; 4]; 2],
    domain: u32,
    tasks: JoinSet<()>,
}

impl Fixture {
    fn new() -> Self {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let prefix = format!("mixed_ipfix_{}_{nonce}", std::process::id());
        Self {
            table: Table::new(
                DbConnector::new_unix(6, "/var/run/redis/redis.sock", 1000)
                    .expect("STATE_DB Redis is required; this test must not skip"),
                "HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE",
            )
            .unwrap(),
            keys: std::array::from_fn(|i| {
                if i < 4 {
                    format!("{prefix}|{}", GROUPS[i].0)
                } else {
                    format!("{prefix}_independent|PORT")
                }
            }),
            names: std::array::from_fn(|generation| {
                let name = format!("{prefix}_gen{generation}");
                [
                    format!("{name}_port"),
                    format!("{name}|7"),
                    format!("{name}|7"),
                    format!("{name}_pool"),
                ]
            }),
            domain: (nonce as u32) | 1,
            tasks: JoinSet::new(),
        }
    }

    fn row(&self, index: usize, generation: usize) -> IPFixTemplatesMessage {
        let (id, count, name, label) = if index == 4 {
            (700, 1, &self.keys[4], 1)
        } else {
            (
                300 + generation as u16,
                4,
                &self.names[generation][index],
                index as u16 + 1,
            )
        };
        let mut fields = id.to_be_bytes().to_vec();
        fields.extend_from_slice(&(count as u16 + 2).to_be_bytes());
        fields.extend_from_slice(&[1, 69, 0, 8]); // observationTimeNanoseconds
        fields.extend_from_slice(&[128, 0, 0, 4, 0, 0, 0, 0]); // hardware padding slot
        for i in 0..count {
            fields.extend_from_slice(&(0x8000 | (i as u16 + 1)).to_be_bytes());
            fields.extend_from_slice(&WIDTHS[i].to_be_bytes());
            fields.extend_from_slice(&(GROUPS[i].1 << 16).to_be_bytes());
        }
        IPFixTemplatesMessage::new(
            self.keys[index].clone(),
            Arc::new(self.message(2, &fields)),
            Some(vec![name.clone()]),
            Some(vec![label]),
        )
    }

    fn publish(&self, row: &IPFixTemplatesMessage) {
        let name = row.object_names.as_ref().unwrap()[0].as_str();
        let label = row.object_ids.as_ref().unwrap()[0].to_string();
        let config = row.templates.as_ref().unwrap().as_ref().clone();
        self.table
            .set(
                &row.key,
                [
                    ("stream_status", CxxString::from("enabled")),
                    ("session_type", CxxString::from("ipfix")),
                    ("object_names", CxxString::from(name)),
                    ("object_ids", CxxString::from(label)),
                    ("session_config", CxxString::from(config)),
                ],
            )
            .unwrap();
    }

    fn message(&self, set_id: u16, payload: &[u8]) -> Vec<u8> {
        let mut bytes = vec![0, 10];
        bytes.extend_from_slice(&((20 + payload.len()) as u16).to_be_bytes());
        bytes.extend_from_slice(&[0; 8]); // export time and sequence
        bytes.extend_from_slice(&self.domain.to_be_bytes());
        bytes.extend_from_slice(&set_id.to_be_bytes());
        bytes.extend_from_slice(&((4 + payload.len()) as u16).to_be_bytes());
        bytes.extend_from_slice(payload);
        bytes
    }

    fn record(&self, id: u16, time: u64, count: usize) -> Vec<u8> {
        let mut payload = time.to_be_bytes().to_vec();
        payload.extend_from_slice(&0xdead_beefu32.to_be_bytes());
        for i in 0..count {
            payload.extend_from_slice(&VALUES[i].to_be_bytes()[8 - WIDTHS[i] as usize..]);
        }
        self.message(id, &payload)
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        // Also abort on assertion failure. Actors only read Redis; cleanup cannot
        // race a writer recreating our rows. Never delete another fixture's keys.
        self.tasks.abort_all();
        let errors: Vec<_> = self
            .keys
            .iter()
            .filter_map(|key| self.table.del(key).err())
            .collect();
        if !errors.is_empty() {
            if std::thread::panicking() {
                eprintln!("Redis fixture cleanup failed: {errors:?}");
            } else {
                panic!("Redis fixture cleanup failed: {errors:?}");
            }
        }
    }
}

async fn acknowledged(
    receiver: &mut mpsc::Receiver<IPFixTemplatesMessage>,
    mut pending: Vec<IPFixTemplatesMessage>,
) {
    timeout(DEADLINE, async {
        while !pending.is_empty() {
            let row = receiver.recv().await.expect("SWSS relay closed");
            pending.retain(|expected| {
                !(row.key == expected.key
                    && row.operation == expected.operation
                    && row.templates == expected.templates
                    && row.object_names == expected.object_names
                    && row.object_ids == expected.object_ids)
            });
        }
    })
    .await
    .unwrap_or_else(|_| panic!("exact SWSS revision not acknowledged: {pending:?}"));
}

struct Capture {
    sender: mpsc::Sender<SocketBufferMessage>,
    receiver: mpsc::Receiver<SAIStatsBatchMessage>,
}

impl Capture {
    async fn probe(
        &mut self,
        fixture: &Fixture,
        inputs: &[(u16, u64)],
        expected: Option<(u64, usize)>,
    ) {
        timeout(DEADLINE, async {
            let mut input: Vec<u8> = inputs
                .iter()
                .flat_map(|&(id, time)| fixture.record(id, time, 4))
                .collect();
            // Same-domain FIFO output fence proves rejected data was processed and
            // the independent owner survived. No sleeps or absence-only assertions.
            input.extend_from_slice(&fixture.record(700, 9000, 1));
            self.sender.send(Arc::new(input)).await.unwrap();
            let mut expected = expected
                .into_iter()
                .map(|(time, generation)| (time, fixture.names[generation].as_slice()))
                .chain(std::iter::once((9000, &fixture.keys[4..])))
                .peekable();
            while expected.peek().is_some() {
                let batch = self.receiver.recv().await.expect("IPFIX output closed");
                assert_eq!(
                    batch.counter_count(),
                    batch.iter().map(|r| r.stats.len()).sum::<usize>()
                );
                for record in batch.iter() {
                    let (time, names) = expected.next().expect("unexpected extra record");
                    assert_eq!(record.observation_time, time);
                    assert_eq!(record.stats.len(), names.len(), "no padding counter");
                    let stats = record
                        .stats
                        .iter()
                        .map(|s| (s.object_name.to_string(), s.type_id, s.stat_id, s.counter))
                        .collect::<Vec<_>>();
                    let expected: Vec<_> = names
                        .iter()
                        .enumerate()
                        .map(|(i, name)| (name.clone(), GROUPS[i].1, 0, VALUES[i]))
                        .collect();
                    assert_eq!(stats, expected, "exact names, types, statistics and values");
                }
            }
        })
        .await
        .expect("IPFIX output fence deadline");
    }
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn per_group_binary_snapshots_start_promote_and_invalidate_together() {
    let mut fixture = Fixture::new();
    let startup: Vec<_> = (0..5).map(|i| fixture.row(i, 0)).collect();
    for row in &startup {
        fixture.publish(row);
    }
    // Publish every group before constructing the subscriber: exercise startup
    // enumeration, not just live notifications.
    let (swss_tx, mut swss_rx) = mpsc::channel::<IPFixTemplatesMessage>(1);
    let swss = SwssActor::new(swss_tx).unwrap();
    let (templates, template_rx) = mpsc::channel(1);
    let (ack, mut ack_rx) = mpsc::channel(16);
    let (records, record_rx) = mpsc::channel(1);
    let (stats, stats_rx) = mpsc::channel(8);
    let mut capture = Capture {
        sender: records,
        receiver: stats_rx,
    };
    let mut ipfix = IpfixActor::new(template_rx, record_rx);
    ipfix.add_recipient(stats);
    let keys = fixture.keys.clone();
    fixture.tasks.spawn(async move {
        while let Some(row) = swss_rx.recv().await {
            // Forward ALL real SWSS messages unchanged. Capacity one ensures
            // receipt; synchronous template handling precedes subsequent data.
            templates.send(row.clone()).await.unwrap();
            drop(templates.reserve().await.unwrap());
            if keys.contains(&row.key) {
                ack.send(row).await.unwrap();
            }
        }
    });
    fixture.tasks.spawn(IpfixActor::run(ipfix));
    fixture.tasks.spawn(SwssActor::run(swss));
    acknowledged(&mut ack_rx, startup).await;
    capture.probe(&fixture, &[(300, 10)], Some((10, 0))).await;

    for (step, index) in [1, 3, 0, 2].into_iter().enumerate() {
        let row = fixture.row(index, 1);
        fixture.publish(&row);
        acknowledged(&mut ack_rx, vec![row]).await;
        let mut inputs = Vec::new();
        if step < 3 {
            inputs.push((301, 100 + step as u64)); // incomplete must not decode
        }
        let time = 20 + step as u64;
        inputs.push((300, time));
        capture.probe(&fixture, &inputs, Some((time, 0))).await;
    }
    // Complete metadata alone did not promote. First new-ID data does; old-ID
    // data later in this same input must already be retired.
    capture
        .probe(&fixture, &[(301, 30), (300, 31)], Some((30, 1)))
        .await;

    for operation in [
        IPFixTemplateOperation::Delete,
        IPFixTemplateOperation::Deactivate,
    ] {
        let row = fixture.row(1, 1);
        fixture.publish(&row);
        acknowledged(&mut ack_rx, vec![row]).await;
        capture.probe(&fixture, &[(301, 40)], Some((40, 1))).await;
        let removal = if operation == IPFixTemplateOperation::Delete {
            fixture.table.del(&fixture.keys[1]).unwrap();
            IPFixTemplatesMessage::delete(fixture.keys[1].clone())
        } else {
            let disabled = CxxString::from("disabled");
            fixture
                .table
                .hset(&fixture.keys[1], "stream_status", &disabled)
                .unwrap();
            IPFixTemplatesMessage::deactivate(fixture.keys[1].clone())
        };
        acknowledged(&mut ack_rx, vec![removal]).await;
        capture.probe(&fixture, &[(300, 50), (301, 51)], None).await;
    }
    fixture.tasks.abort_all();
    timeout(DEADLINE, async {
        while let Some(result) = fixture.tasks.join_next().await {
            if let Err(error) = result {
                assert!(error.is_cancelled(), "actor failed: {error}");
            }
        }
    })
    .await
    .expect("actor shutdown deadline");
}
