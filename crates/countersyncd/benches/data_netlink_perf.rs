//! Data Netlink transport throughput for large telemetry datagrams.
//!
//! Each 65,535-byte Generic Netlink payload contains 8,191 complete 8-byte metric equivalents
//! plus 7 remainder bytes. The submitted Netlink datagram is 65,555 bytes after its two headers.
//! DataNetlinkActor transports the payload as a whole, so this is byte-derived metric-equivalent
//! throughput rather than downstream IPFIX metric decoding throughput.
//!
//! Run with:
//! `cargo bench -p countersyncd --features benchmark --bench data_netlink_perf`

use std::{sync::Arc, time::Duration};

use countersyncd::{
    actor::data_netlink::{benchmark_parse_datagram, DataNetlinkActor},
    message::netlink::NetlinkCommand,
};
use criterion::{black_box, criterion_group, criterion_main, Criterion, SamplingMode, Throughput};
use netlink_sys::{protocols::NETLINK_USERSOCK, Socket, SocketAddr};
use tokio::{
    runtime::Runtime,
    sync::{mpsc, Mutex},
    task::JoinHandle,
    time::timeout,
};

const METRIC_SIZE_BYTES: usize = 8;
const METRIC_PAYLOAD_BYTES: usize = 65_535;
const NETLINK_HEADER_BYTES: usize = 16;
const GENERIC_NETLINK_HEADER_BYTES: usize = 4;
const DATAGRAM_BYTES: usize =
    NETLINK_HEADER_BYTES + GENERIC_NETLINK_HEADER_BYTES + METRIC_PAYLOAD_BYTES;
const METRICS_PER_DATAGRAM: usize = METRIC_PAYLOAD_BYTES / METRIC_SIZE_BYTES;
const DATAGRAMS_PER_ITERATION: usize = 64;
const MAX_IN_FLIGHT_DATAGRAMS: usize = 4;
const FAMILY_ID: u16 = 0x20;
const CHANNEL_CAPACITY: usize = MAX_IN_FLIGHT_DATAGRAMS * 2;

struct DataNetlinkBench {
    sender: Socket,
    receiver_address: SocketAddr,
    command_sender: mpsc::Sender<NetlinkCommand>,
    payload_receiver: mpsc::Receiver<countersyncd::message::buffer::SocketBufferMessage>,
    actor_task: JoinHandle<()>,
    datagram: Vec<u8>,
    next_sequence: u64,
}

impl DataNetlinkBench {
    fn new(runtime: &Runtime) -> Self {
        let mut receiver = Socket::new(NETLINK_USERSOCK).expect("create receiver socket");
        let receiver_address = receiver.bind_auto().expect("bind receiver socket");
        receiver
            .set_rx_buf_sz(4 * 1024 * 1024)
            .expect("set receiver buffer");
        receiver
            .set_non_blocking(true)
            .expect("set receiver non-blocking");
        let actual_rcvbuf = receiver.get_rx_buf_sz().expect("read receiver buffer size");
        assert!(
            actual_rcvbuf >= DATAGRAM_BYTES * MAX_IN_FLIGHT_DATAGRAMS,
            "receiver buffer {actual_rcvbuf} is too small for the in-flight window"
        );

        let mut sender = Socket::new(NETLINK_USERSOCK).expect("create sender socket");
        sender.bind_auto().expect("bind sender socket");
        sender
            .set_non_blocking(true)
            .expect("set sender non-blocking");

        let (command_sender, command_receiver) = mpsc::channel(1);
        let (payload_sender, payload_receiver) = mpsc::channel(CHANNEL_CAPACITY);
        let mut actor = DataNetlinkActor::new_benchmark(receiver, command_receiver, FAMILY_ID);
        actor.add_recipient(payload_sender);
        let actor_task = runtime.spawn(DataNetlinkActor::run(actor));

        let mut bench = Self {
            sender,
            receiver_address,
            command_sender,
            payload_receiver,
            actor_task,
            datagram: metric_datagram(),
            next_sequence: 0,
        };
        runtime.block_on(bench.process_iteration());
        bench
    }

    async fn process_iteration(&mut self) -> usize {
        timeout(Duration::from_secs(2), async {
            let first_sequence = self.next_sequence;
            let mut sent_datagrams = 0;
            let mut received_datagrams = 0;
            let mut bytes = 0;

            while received_datagrams < DATAGRAMS_PER_ITERATION {
                while sent_datagrams < DATAGRAMS_PER_ITERATION
                    && sent_datagrams - received_datagrams < MAX_IN_FLIGHT_DATAGRAMS
                {
                    let sequence = first_sequence + sent_datagrams as u64;
                    self.datagram[20..28].copy_from_slice(&sequence.to_ne_bytes());
                    let sent = self
                        .sender
                        .send_to(&self.datagram, &self.receiver_address, 0)
                        .expect("send benchmark datagram");
                    assert_eq!(sent, self.datagram.len());
                    sent_datagrams += 1;
                }

                let payload = self
                    .payload_receiver
                    .recv()
                    .await
                    .expect("data actor channel closed");
                assert_eq!(payload.len(), METRIC_PAYLOAD_BYTES);
                let sequence =
                    u64::from_ne_bytes(payload[..8].try_into().expect("sequence metric"));
                assert_eq!(sequence, first_sequence + received_datagrams as u64);
                bytes += payload.len();
                received_datagrams += 1;
            }

            self.next_sequence += DATAGRAMS_PER_ITERATION as u64;
            bytes
        })
        .await
        .expect("data actor timed out")
    }

    fn stop(self, runtime: &Runtime) {
        runtime.block_on(async {
            self.command_sender
                .send(NetlinkCommand::Close)
                .await
                .expect("send close command");
            timeout(Duration::from_secs(2), self.actor_task)
                .await
                .expect("data actor did not stop")
                .expect("data actor panicked");
        });
    }
}

fn metric_datagram() -> Vec<u8> {
    let mut datagram = vec![0u8; DATAGRAM_BYTES];
    datagram[0..4].copy_from_slice(&(DATAGRAM_BYTES as u32).to_ne_bytes());
    datagram[4..6].copy_from_slice(&FAMILY_ID.to_ne_bytes());
    datagram[16] = 1;

    for (index, metric) in datagram[20..]
        .chunks_exact_mut(METRIC_SIZE_BYTES)
        .enumerate()
    {
        metric.copy_from_slice(&(index as u64).to_ne_bytes());
    }
    datagram
}

fn bench_data_netlink_actor(c: &mut Criterion) {
    let datagram = metric_datagram();
    let mut parse_group = c.benchmark_group("data_netlink_parse");
    parse_group.sample_size(50);
    parse_group.warm_up_time(Duration::from_secs(2));
    parse_group.measurement_time(Duration::from_secs(8));
    parse_group.throughput(Throughput::Elements(METRICS_PER_DATAGRAM as u64));
    parse_group.bench_function("65535_metric_payload_bytes", |b| {
        b.iter(|| {
            black_box(
                benchmark_parse_datagram(black_box(&datagram), FAMILY_ID)
                    .expect("parse benchmark datagram"),
            )
        })
    });
    parse_group.finish();

    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .expect("create Tokio runtime");
    let bench = Arc::new(Mutex::new(DataNetlinkBench::new(&runtime)));
    let mut group = c.benchmark_group("data_netlink_actor");
    group.sample_size(20);
    group.warm_up_time(Duration::from_secs(2));
    group.measurement_time(Duration::from_secs(10));
    group.sampling_mode(SamplingMode::Flat);
    group.throughput(Throughput::Elements(
        (METRICS_PER_DATAGRAM * DATAGRAMS_PER_ITERATION) as u64,
    ));

    group.bench_function("65535_metric_payload_bytes_window4", |b| {
        b.to_async(&runtime).iter(|| {
            let bench = Arc::clone(&bench);
            async move {
                let bytes = bench.lock().await.process_iteration().await;
                black_box(bytes)
            }
        })
    });
    group.finish();
    let bench = match Arc::try_unwrap(bench) {
        Ok(bench) => bench.into_inner(),
        Err(_) => panic!("benchmark actor still has shared owners"),
    };
    bench.stop(&runtime);
}

criterion_group!(benches, bench_data_netlink_actor);
criterion_main!(benches);
