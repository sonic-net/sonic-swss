//! Producer-shaped Redis rows through all four real actors. Keep the cases in
//! one test binary: SwssActor subscribes to a hardcoded, shared STATE_DB table.

#[allow(dead_code)]
mod test_common;

use countersyncd::actor::{
    counter_db::{CounterDBActor, CounterDBConfig},
    ipfix::IpfixActor,
    otel::{OtelActor, OtelActorConfig},
    swss::SwssActor,
};
use countersyncd::message::{
    buffer::SocketBufferMessage,
    ipfix::{IPFixTemplateOperation, IPFixTemplatesMessage},
    saistats::SAIStatsBatchMessage,
};
use opentelemetry_proto::tonic::{
    collector::metrics::v1::{
        metrics_service_server::{MetricsService, MetricsServiceServer},
        ExportMetricsServiceRequest, ExportMetricsServiceResponse,
    },
    common::v1::{any_value, KeyValue},
    metrics::v1::{metric, number_data_point},
};
use std::{
    collections::{BTreeMap, HashMap},
    future::{poll_fn, Future},
    panic::{catch_unwind, resume_unwind, AssertUnwindSafe},
    sync::Arc,
    task::Poll,
    time::{Duration, SystemTime, UNIX_EPOCH},
};
use swss_common::{CxxString, DbConnector, Table};
use tokio::{
    sync::{mpsc, oneshot},
    task::JoinHandle,
    time::{sleep, timeout},
};
use tokio_stream::wrappers::TcpListenerStream;
use tonic::{transport::Server, Request, Response, Status};

const SOCKET: &str = "/var/run/redis/redis.sock";
const SESSION_TABLE: &str = "HIGH_FREQUENCY_TELEMETRY_SESSION_TABLE";
const DEADLINE: Duration = Duration::from_secs(10);
const POLL: Duration = Duration::from_millis(10);
const EXPORT_CAP: usize = 3;
// Stat zero is supported for every group, unlike the IPFIX-only fixture's stat 7.
const GROUPS: [(&str, u32, &str, &str, usize); 4] = [
    (
        "PORT",
        1,
        "COUNTERS_PORT_NAME_MAP",
        "SAI_PORT_STAT_IF_IN_OCTETS",
        8,
    ),
    (
        "QUEUE",
        21,
        "COUNTERS_QUEUE_NAME_MAP",
        "SAI_QUEUE_STAT_PACKETS",
        4,
    ),
    (
        "BUFFER_POOL",
        24,
        "COUNTERS_BUFFER_POOL_NAME_MAP",
        "SAI_BUFFER_POOL_STAT_CURR_OCCUPANCY_BYTES",
        8,
    ),
    (
        "INGRESS_PRIORITY_GROUP",
        26,
        "COUNTERS_PG_NAME_MAP",
        "SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS",
        4,
    ),
];

struct Collector(mpsc::Sender<ExportMetricsServiceRequest>);

#[tonic::async_trait]
impl MetricsService for Collector {
    async fn export(
        &self,
        request: Request<ExportMetricsServiceRequest>,
    ) -> Result<Response<ExportMetricsServiceResponse>, Status> {
        self.0
            .send(request.into_inner())
            .await
            .map_err(|_| Status::unavailable("capture receiver closed"))?;
        Ok(Response::new(ExportMetricsServiceResponse::default()))
    }
}

struct Fixture {
    state: Table,
    counters: DbConnector,
    prefix: String,
    domain: u32,
    names: [[String; 4]; 2],
    oids: [[String; 4]; 2],
    owned_maps: Vec<(&'static str, String, String)>,
}

impl Fixture {
    fn new(case: &str) -> Self {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let prefix = format!("mixed_pipeline_{}_{nonce}_{case}", std::process::id());
        let state = Table::new(
            DbConnector::new_unix(6, SOCKET, 1000).expect("STATE_DB Redis is required"),
            SESSION_TABLE,
        )
        .unwrap();
        let counters = DbConnector::new_unix(2, SOCKET, 1000)
            .expect("COUNTERS_DB Redis is required; this test must not skip");
        let names = std::array::from_fn(|generation| {
            let object_prefix = format!("{prefix}_gen{generation}");
            [
                format!("{object_prefix}_port"),
                format!("{object_prefix}|7"),
                format!("{object_prefix}_pool"),
                format!("{object_prefix}|7"),
            ]
        });
        let oids = std::array::from_fn(|generation| {
            std::array::from_fn(|index| {
                let oid = (u64::from(GROUPS[index].1) << 48)
                    | ((nonce as u64 & 0x0000_7fff_ffff_ffff) << 1)
                    | generation as u64;
                format!("oid:0x{oid:x}")
            })
        });
        let mut fixture = Self {
            state,
            counters,
            prefix,
            domain: (nonce as u32) | 1,
            names,
            oids,
            owned_maps: Vec::new(),
        };
        for generation in 0..2 {
            for (index, &(_, _, map, _, _)) in GROUPS.iter().enumerate() {
                let counters_key = format!("COUNTERS:{}", fixture.oids[generation][index]);
                assert!(!fixture.counters.exists(&counters_key).unwrap());
                // Queue and PG share a producer spelling, not a map or typed OID.
                // Every field belongs to this fixture; never overwrite shared data.
                let field = fixture.names[generation][index].replace('|', ":");
                assert!(!fixture.counters.hexists(map, &field).unwrap());
                fixture.owned_maps.push((map, field.clone(), counters_key));
                fixture
                    .counters
                    .hset(
                        map,
                        &field,
                        &CxxString::from(fixture.oids[generation][index].as_str()),
                    )
                    .unwrap();
            }
        }
        fixture
    }

    fn row_key(&self, index: usize) -> String {
        format!("{}|{}", self.prefix, GROUPS[index].0)
    }

    fn publish(&self, index: usize, generation: u16, snapshot: &[u8]) {
        self.state
            .set(
                &self.row_key(index),
                [
                    ("stream_status", CxxString::from("enabled")),
                    ("session_type", CxxString::from("ipfix")),
                    (
                        "object_names",
                        CxxString::from(self.names[usize::from(generation)][index].as_str()),
                    ),
                    (
                        "object_ids",
                        CxxString::from((generation * 10 + index as u16 + 1).to_string()),
                    ),
                    ("session_config", CxxString::from(snapshot.to_vec())),
                ],
            )
            .unwrap();
    }

    async fn wait_forwarded(
        &self,
        acknowledgements: &mut mpsc::Receiver<IPFixTemplatesMessage>,
        generation: u16,
        snapshot: &[u8],
        indices: &[usize],
    ) {
        timeout(DEADLINE, async {
            let mut pending = indices.to_vec();
            while !pending.is_empty() {
                let row = acknowledgements
                    .recv()
                    .await
                    .expect("template relay closed");
                pending.retain(|&index| {
                    !(row.key == self.row_key(index)
                        && row.operation == IPFixTemplateOperation::Update
                        && row.templates.as_deref().map(Vec::as_slice) == Some(snapshot)
                        && row.object_names.as_deref()
                            == Some(std::slice::from_ref(
                                &self.names[usize::from(generation)][index],
                            ))
                        && row.object_ids.as_deref()
                            == Some([generation * 10 + index as u16 + 1].as_slice()))
                });
            }
        })
        .await
        .unwrap_or_else(|_| {
            panic!(
                "exact forwarded revision deadline: {} generation {generation}, groups {indices:?}",
                self.prefix,
            )
        });
    }

    fn snapshot(&self, generation: u16) -> Vec<u8> {
        let mut fields = Vec::new();
        fields.extend_from_slice(&(300 + generation).to_be_bytes());
        fields.extend_from_slice(&6u16.to_be_bytes()); // time, placeholder, four counters
        fields.extend_from_slice(&325u16.to_be_bytes());
        fields.extend_from_slice(&8u16.to_be_bytes());
        fields.extend_from_slice(&0x8000u16.to_be_bytes());
        fields.extend_from_slice(&4u16.to_be_bytes());
        fields.extend_from_slice(&0u32.to_be_bytes());
        for (index, &(_, type_id, _, _, width)) in GROUPS.iter().enumerate() {
            fields
                .extend_from_slice(&(0x8000 | (generation * 10 + index as u16 + 1)).to_be_bytes());
            fields.extend_from_slice(&(width as u16).to_be_bytes());
            fields.extend_from_slice(&(type_id << 16).to_be_bytes());
        }
        self.message(2, &fields)
    }

    fn message(&self, set_id: u16, payload: &[u8]) -> Vec<u8> {
        let mut bytes = Vec::new();
        bytes.extend_from_slice(&10u16.to_be_bytes());
        bytes.extend_from_slice(&((20 + payload.len()) as u16).to_be_bytes());
        bytes.extend_from_slice(&0u32.to_be_bytes());
        bytes.extend_from_slice(&0u32.to_be_bytes());
        bytes.extend_from_slice(&self.domain.to_be_bytes());
        bytes.extend_from_slice(&set_id.to_be_bytes());
        bytes.extend_from_slice(&((4 + payload.len()) as u16).to_be_bytes());
        bytes.extend_from_slice(payload);
        bytes
    }

    fn record(&self, generation: u16, time: u64) -> Vec<u8> {
        let mut payload = time.to_be_bytes().to_vec();
        payload.extend_from_slice(&0xdead_beefu32.to_be_bytes());
        for (index, &(_, _, _, _, width)) in GROUPS.iter().enumerate() {
            let value = Self::values(time)[index].to_be_bytes();
            payload.extend_from_slice(&value[8 - width..]);
        }
        self.message(300 + generation, &payload)
    }

    fn values(time: u64) -> [u64; 4] {
        // Exercise actual 64-bit values as well as four-byte counters, without
        // depending on the in-flight OtelDataPoint signed/unsigned API change.
        [
            (1u64 << 40) + time,
            200 + time,
            (1u64 << 42) + time,
            400 + time,
        ]
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        // Readers and writers are stopped before this guard is dropped. Attempt
        // every cleanup even when unwinding a failed assertion.
        let mut errors = Vec::new();
        for index in 0..4 {
            if let Err(error) = self.state.del(&self.row_key(index)) {
                errors.push(error.to_string());
            }
        }
        for (map, field, counters_key) in &self.owned_maps {
            if let Err(error) = self.counters.del(counters_key) {
                errors.push(error.to_string());
            }
            if let Err(error) = self.counters.hdel(map, field) {
                errors.push(error.to_string());
            }
        }
        if !errors.is_empty() {
            if std::thread::panicking() {
                eprintln!("Redis fixture cleanup failed: {errors:?}");
            } else {
                panic!("Redis fixture cleanup failed: {errors:?}");
            }
        }
    }
}

fn attributes(attributes: &[KeyValue]) -> BTreeMap<String, String> {
    attributes
        .iter()
        .map(|attribute| {
            let Some(any_value::Value::StringValue(value)) = attribute
                .value
                .as_ref()
                .and_then(|value| value.value.as_ref())
            else {
                panic!("expected string OTLP attribute: {attribute:?}")
            };
            (attribute.key.clone(), value.clone())
        })
        .collect()
}

async fn verify_outputs(
    fixture: &Fixture,
    records: &mpsc::Sender<SocketBufferMessage>,
    decoded: &mut mpsc::Receiver<SAIStatsBatchMessage>,
    exports: &mut mpsc::Receiver<ExportMetricsServiceRequest>,
    generation: u16,
    time: u64,
    rejected_generation: Option<u16>,
) {
    timeout(DEADLINE, async {
        // An unknown/retired generation followed by a known record in the same
        // input is a FIFO fence: no sleeps or absence-only assertion is needed.
        let mut input = rejected_generation
            .map(|id| fixture.record(id, time + 1000))
            .unwrap_or_default();
        input.extend_from_slice(&fixture.record(generation, time));
        records.send(Arc::new(input)).await.unwrap();
        let batch = decoded.recv().await.expect("IPFIX actor closed output");
        assert_eq!(
            batch.record_count(),
            1,
            "only the active generation may decode"
        );
        assert_eq!(
            batch.counter_count(),
            4,
            "placeholder must not become a counter"
        );
        let record = batch.iter().next().unwrap();
        assert_eq!(record.observation_time, time);
        let values = Fixture::values(time);
        for (index, stat) in record.stats.iter().enumerate() {
            assert_eq!(
                stat.object_name.as_ref(),
                fixture.names[usize::from(generation)][index]
            );
            assert_eq!(stat.type_id, GROUPS[index].1);
            assert_eq!(stat.stat_id, 0);
            assert_eq!(stat.counter, values[index]);
        }

        // A record can span several exports. Flatten resource/scope envelopes in
        // wire order and collect its complete timestamped sample, not one request.
        let mut sample = Vec::new();
        while sample.len() < GROUPS.len() {
            let request = exports.recv().await.expect("collector closed capture");
            let mut request_points = 0;
            for resource in request.resource_metrics {
                assert_eq!(
                    attributes(&resource.resource.as_ref().unwrap().attributes),
                    BTreeMap::from([("service.name".into(), "countersyncd".into())])
                );
                for scope in resource.scope_metrics {
                    assert_eq!(scope.scope.as_ref().unwrap().name, "countersyncd");
                    for metric in scope.metrics {
                        let Some(metric::Data::Gauge(gauge)) = metric.data else {
                            panic!("expected gauge")
                        };
                        assert_eq!(gauge.data_points.len(), 1);
                        for point in gauge.data_points {
                            assert_eq!(point.time_unix_nano, time, "unexpected sample timestamp");
                            sample.push((metric.name.clone(), point));
                            request_points += 1;
                        }
                    }
                }
            }
            assert!(
                (1..=EXPORT_CAP).contains(&request_points),
                "each export must contain 1..={EXPORT_CAP} counters, got {request_points}"
            );
        }
        assert_eq!(
            sample.len(),
            GROUPS.len(),
            "exactly four counters per sample"
        );
        for (index, (name, point)) in sample.iter().enumerate() {
            assert_eq!(
                name, GROUPS[index].3,
                "typed metric order, no duplicates/placeholders"
            );
            assert_eq!(
                point.value,
                Some(number_data_point::Value::AsInt(values[index] as i64))
            );
            assert_eq!(point.attributes.len(), 3);
            assert_eq!(
                attributes(&point.attributes),
                BTreeMap::from([
                    (
                        "object_name".into(),
                        fixture.names[usize::from(generation)][index].clone()
                    ),
                    (
                        "sai_type".into(),
                        format!("SAI_OBJECT_TYPE_{}", GROUPS[index].0)
                    ),
                    ("sai_stat".into(), GROUPS[index].3.into()),
                ])
            );
        }

        loop {
            let actual: Vec<HashMap<String, String>> = fixture.oids[usize::from(generation)]
                .iter()
                .map(|oid| {
                    fixture
                        .counters
                        .hgetall(&format!("COUNTERS:{oid}"))
                        .unwrap()
                        .into_iter()
                        .map(|(key, value)| (key, value.to_string_lossy().into_owned()))
                        .collect()
                })
                .collect();
            let expected: Vec<_> = GROUPS
                .iter()
                .enumerate()
                .map(|(index, group)| {
                    HashMap::from([(group.3.to_string(), values[index].to_string())])
                })
                .collect();
            if actual == expected {
                if generation == 0 {
                    for oid in &fixture.oids[1] {
                        assert!(
                            !fixture.counters.exists(&format!("COUNTERS:{oid}")).unwrap(),
                            "pending names must not receive active-generation counters"
                        );
                    }
                }
                break;
            }
            sleep(POLL).await;
        }
    })
    .await
    .unwrap_or_else(|_| {
        panic!(
            "pipeline output deadline: {} generation {generation}, time {time}\n{}",
            fixture.prefix,
            test_common::capture_logs()
        )
    });
}

fn swss_readers() -> usize {
    // SwssActor owns a detached native reader. Joining its Tokio task alone does
    // not join that reader; Linux truncates its thread name to 15 characters.
    std::fs::read_dir("/proc/self/task")
        .unwrap()
        .filter_map(|entry| std::fs::read_to_string(entry.ok()?.path().join("comm")).ok())
        .filter(|name| name.trim() == &"countersyncd-swss"[..15])
        .count()
}

async fn stop_tasks(tasks: &mut [JoinHandle<()>]) {
    let mut failures = Vec::new();
    for task in tasks.iter() {
        task.abort();
    }
    for task in tasks {
        match timeout(DEADLINE, task).await {
            Ok(Err(error)) if !error.is_cancelled() => failures.push(error.to_string()),
            Err(error) => failures.push(format!("actor cancellation deadline: {error}")),
            _ => {}
        }
    }
    timeout(DEADLINE, async {
        while swss_readers() != 0 {
            sleep(POLL).await;
        }
    })
    .await
    .expect("SWSS reader must stop before Redis cleanup and the next case");
    assert!(failures.is_empty(), "actor shutdown failed: {failures:?}");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn producer_rows_reach_typed_counterdb_and_otlp_outputs() {
    test_common::capture_logs();
    for staggered in [false, true] {
        assert_eq!(swss_readers(), 0, "cases must not share live SWSS readers");
        let fixture = Fixture::new(if staggered { "staggered" } else { "startup" });
        let mut tasks = Vec::new();
        // Catch assertion panics around the async case so cancellation, native
        // reader exit, and Redis cleanup also happen on a regression failure.
        let result = {
            let body = async {
                test_common::clear_logs();
                let initial = fixture.snapshot(0);
                for index in 0..4 {
                    fixture.publish(index, 0, &initial);
                }
                let (swss_tx, mut swss_rx) = mpsc::channel::<IPFixTemplatesMessage>(1);
                let swss = SwssActor::new(swss_tx).unwrap();
                let (templates, template_rx) = mpsc::channel(1);
                let (acknowledge, mut acknowledgements) = mpsc::channel(16);
                let (records, record_rx) = mpsc::channel(1);
                let (stats, mut decoded) = mpsc::channel(8);
                let (db_tx, db_rx) = mpsc::channel(8);
                let (otel_tx, otel_rx) = mpsc::channel(8);
                let (capture, mut exports) = mpsc::channel(8);
                let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
                let endpoint = format!("http://{}", listener.local_addr().unwrap());
                let (server_stop, server_stopped) = oneshot::channel();
                let server = Server::builder()
                    .add_service(MetricsServiceServer::new(Collector(capture)))
                    .serve_with_incoming_shutdown(TcpListenerStream::new(listener), async move {
                        let _ = server_stopped.await;
                    });
                tasks.push(tokio::spawn(async move {
                    server.await.unwrap();
                }));
                let (otel_stop, otel_stopped) = oneshot::channel();
                let otel = OtelActor::new(
                    otel_rx,
                    OtelActorConfig {
                        collector_endpoint: endpoint,
                        max_counters_per_export: EXPORT_CAP,
                        flush_timeout: Duration::from_millis(20),
                    },
                    otel_stop,
                )
                .await
                .unwrap();
                let db =
                    CounterDBActor::new(db_rx, CounterDBConfig::new(Duration::from_millis(20)))
                        .unwrap();
                let mut ipfix = IpfixActor::new(template_rx, record_rx);
                ipfix.add_recipient(stats);
                ipfix.add_recipient(db_tx);
                ipfix.add_recipient(otel_tx);
                let profile_prefix = format!("{}|", fixture.prefix);
                tasks.push(tokio::spawn(async move {
                    while let Some(row) = swss_rx.recv().await {
                        let ours = row.key.starts_with(&profile_prefix);
                        // Relay every real SwssActor message unchanged, including
                        // unrelated baseline rows. Acknowledge the exact revision
                        // only after forwarding and the capacity-one receipt fence.
                        templates.send(row.clone()).await.unwrap();
                        drop(templates.reserve().await.unwrap());
                        if ours {
                            acknowledge.send(row).await.unwrap();
                        }
                    }
                }));
                tasks.push(tokio::spawn(IpfixActor::run(ipfix)));
                tasks.push(tokio::spawn(db.run()));
                tasks.push(tokio::spawn(async move {
                    otel.run().await.unwrap();
                }));
                tasks.push(tokio::spawn(SwssActor::run(swss)));

                fixture
                    .wait_forwarded(&mut acknowledgements, 0, &initial, &[0, 1, 2, 3])
                    .await;
                verify_outputs(&fixture, &records, &mut decoded, &mut exports, 0, 10, None).await;

                if staggered {
                    let next = fixture.snapshot(1);
                    for (step, index) in [1, 3, 0, 2].into_iter().enumerate() {
                        fixture.publish(index, 1, &next);
                        fixture
                            .wait_forwarded(&mut acknowledgements, 1, &next, &[index])
                            .await;
                        // Incomplete snapshots must reject new-generation probes
                        // and retain all old names until data promotes the snapshot.
                        verify_outputs(
                            &fixture,
                            &records,
                            &mut decoded,
                            &mut exports,
                            0,
                            20 + step as u64,
                            (step < 3).then_some(1),
                        )
                        .await;
                    }
                    verify_outputs(&fixture, &records, &mut decoded, &mut exports, 1, 30, None)
                        .await;
                    verify_outputs(
                        &fixture,
                        &records,
                        &mut decoded,
                        &mut exports,
                        1,
                        31,
                        Some(0),
                    )
                    .await;
                }

                // Cancel the endless subscriber first, then let its consumers
                // drain naturally. stop_tasks also handles all failure paths.
                let swss = tasks.last_mut().unwrap();
                swss.abort();
                let result = timeout(DEADLINE, swss).await;
                if result.is_ok() {
                    tasks.pop();
                }
                assert!(result.unwrap().unwrap_err().is_cancelled());
                while tasks.len() > 1 {
                    let result = timeout(DEADLINE, &mut tasks[1]).await;
                    if result.is_ok() {
                        drop(tasks.remove(1));
                    }
                    result.expect("consumer shutdown deadline").unwrap();
                }
                drop(records);
                timeout(DEADLINE, otel_stopped).await.unwrap().unwrap();
                server_stop.send(()).unwrap();
                let result = timeout(DEADLINE, &mut tasks[0]).await;
                if result.is_ok() {
                    tasks.clear();
                }
                result.expect("collector shutdown deadline").unwrap();
            };
            let mut body = std::pin::pin!(body);
            poll_fn(|context| {
                match catch_unwind(AssertUnwindSafe(|| body.as_mut().poll(context))) {
                    Ok(Poll::Ready(())) => Poll::Ready(Ok(())),
                    Ok(Poll::Pending) => Poll::Pending,
                    Err(panic) => Poll::Ready(Err(panic)),
                }
            })
            .await
        };
        stop_tasks(&mut tasks).await;
        drop(fixture);
        if let Err(panic) = result {
            resume_unwind(panic);
        }
    }
}
