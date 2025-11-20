use criterion::{black_box, criterion_group, criterion_main, Criterion, BenchmarkId, Throughput};
use countersyncd::message::{
    saistats::{SAIStat, SAIStats, SAIStatsMessage, SAIStatsMessageExt},
    otel::{OtelMetrics, OtelMetricsMessageExt},
    ipfix::IpfixMessage,
    netlink::NetlinkMessage,
    buffer::BufferMessage,
    swss::SwssMessage,
};
use tokio::sync::{mpsc, oneshot};
use tokio::time::{timeout, Duration, Instant};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

/// Test patterns for generating different types of workloads
#[derive(Clone, Copy)]
enum WorkloadPattern {
    Burst,      // Send all messages as fast as possible
    Steady,     // Send messages at regular intervals
    Bursty,     // Alternate between bursts and pauses
    Random,     // Random intervals between messages
}

/// Helper function to create test SAI statistics messages
fn create_test_sai_stats_message(stat_count: usize, sequence: u64) -> SAIStatsMessage {
    let stats = (0..stat_count)
        .map(|i| SAIStat {
            object_name: format!("Ethernet{}", i),
            type_id: 1 + (i % 10) as u32,
            stat_id: 1 + (i % 20) as u32,
            counter: sequence * 1000 + i as u64,
        })
        .collect();
    
    SAIStats::from_parts(1672531200000000000u64 + sequence, stats)
}

/// Helper function to create various message types for testing
fn create_test_messages(count: usize, message_type: &str) -> Vec<Box<dyn std::any::Any + Send>> {
    match message_type {
        "sai_stats" => (0..count)
            .map(|i| Box::new(create_test_sai_stats_message(100, i as u64)) as Box<dyn std::any::Any + Send>)
            .collect(),
        _ => panic!("Unknown message type: {}", message_type),
    }
}

/// Benchmark basic message passing throughput between actors
fn bench_message_passing_throughput(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    
    let mut group = c.benchmark_group("message_passing_throughput");
    group.measurement_time(Duration::from_secs(10));
    
    // Test different channel buffer sizes
    for &buffer_size in &[1, 10, 100, 1000, 10000] {
        // Test different message counts
        for &message_count in &[100, 1000, 10000] {
            // Test different SAI stats sizes
            for &stats_size in &[10, 100, 1000] {
                group.throughput(Throughput::Elements(message_count as u64));
                
                group.bench_with_input(
                    BenchmarkId::new(
                        "sai_stats_mpsc", 
                        format!("buf{}_msg{}_stats{}", buffer_size, message_count, stats_size)
                    ),
                    &(buffer_size, message_count, stats_size),
                    |b, &(buf_size, msg_count, stat_size)| {
                        b.to_async(&rt).iter(|| async {
                            let (tx, mut rx) = mpsc::channel::<SAIStatsMessage>(buf_size);
                            let start_time = Instant::now();
                            
                            // Spawn sender task
                            let sender_handle = tokio::spawn(async move {
                                for i in 0..msg_count {
                                    let msg = create_test_sai_stats_message(stat_size, i as u64);
                                    if tx.send(black_box(msg)).await.is_err() {
                                        break;
                                    }
                                }
                            });
                            
                            // Receive messages
                            let mut received = 0;
                            while let Some(msg) = rx.recv().await {
                                black_box(msg);
                                received += 1;
                                if received >= msg_count {
                                    break;
                                }
                            }
                            
                            let duration = start_time.elapsed();
                            sender_handle.await.unwrap();
                            
                            black_box((received, duration))
                        })
                    }
                );
            }
        }
    }
    group.finish();
}

/// Benchmark multi-producer single-consumer scenarios
fn bench_multi_producer_throughput(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    
    let mut group = c.benchmark_group("multi_producer_throughput");
    group.measurement_time(Duration::from_secs(8));
    
    for &producer_count in &[2, 4, 8, 16] {
        for &buffer_size in &[100, 1000] {
            let messages_per_producer = 1000;
            let total_messages = producer_count * messages_per_producer;
            
            group.throughput(Throughput::Elements(total_messages as u64));
            
            group.bench_with_input(
                BenchmarkId::new(
                    "mpsc_producers",
                    format!("{}prod_buf{}", producer_count, buffer_size)
                ),
                &(producer_count, buffer_size, messages_per_producer),
                |b, &(prod_count, buf_size, msg_per_prod)| {
                    b.to_async(&rt).iter(|| async {
                        let (tx, mut rx) = mpsc::channel::<SAIStatsMessage>(buf_size);
                        let start_time = Instant::now();
                        
                        // Spawn multiple producer tasks
                        let mut producer_handles = Vec::new();
                        for producer_id in 0..prod_count {
                            let tx_clone = tx.clone();
                            let handle = tokio::spawn(async move {
                                for i in 0..msg_per_prod {
                                    let msg = create_test_sai_stats_message(
                                        50, 
                                        (producer_id * msg_per_prod + i) as u64
                                    );
                                    if tx_clone.send(black_box(msg)).await.is_err() {
                                        break;
                                    }
                                }
                            });
                            producer_handles.push(handle);
                        }
                        
                        // Drop the original sender to allow receiver to finish
                        drop(tx);
                        
                        // Receive all messages
                        let mut received = 0;
                        while let Some(msg) = rx.recv().await {
                            black_box(msg);
                            received += 1;
                        }
                        
                        let duration = start_time.elapsed();
                        
                        // Wait for all producers to finish
                        for handle in producer_handles {
                            handle.await.unwrap();
                        }
                        
                        black_box((received, duration))
                    })
                }
            );
        }
    }
    group.finish();
}

/// Benchmark single-producer multi-consumer scenarios
fn bench_single_producer_multi_consumer(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    
    let mut group = c.benchmark_group("single_producer_multi_consumer");
    group.measurement_time(Duration::from_secs(8));
    
    for &consumer_count in &[2, 4, 8] {
        for &buffer_size in &[100, 1000] {
            let total_messages = 1000;
            
            group.throughput(Throughput::Elements(total_messages as u64));
            
            group.bench_with_input(
                BenchmarkId::new(
                    "broadcast_consumers",
                    format!("{}cons_buf{}", consumer_count, buffer_size)
                ),
                &(consumer_count, buffer_size, total_messages),
                |b, &(cons_count, buf_size, total_msg)| {
                    b.to_async(&rt).iter(|| async {
                        // Create multiple channels for broadcasting
                        let mut senders = Vec::new();
                        let mut consumer_handles = Vec::new();
                        let received_counter = Arc::new(AtomicU64::new(0));
                        
                        // Create channels and consumers
                        for consumer_id in 0..cons_count {
                            let (tx, mut rx) = mpsc::channel::<SAIStatsMessage>(buf_size);
                            senders.push(tx);
                            
                            let counter = received_counter.clone();
                            let handle = tokio::spawn(async move {
                                let mut local_count = 0;
                                while let Some(msg) = rx.recv().await {
                                    black_box(msg);
                                    local_count += 1;
                                    counter.fetch_add(1, Ordering::Relaxed);
                                }
                                local_count
                            });
                            consumer_handles.push(handle);
                        }
                        
                        let start_time = Instant::now();
                        
                        // Producer task - broadcast to all consumers
                        let producer_handle = tokio::spawn(async move {
                            for i in 0..total_msg {
                                let msg = create_test_sai_stats_message(50, i as u64);
                                let msg = Arc::new(msg);
                                
                                // Send to all consumers
                                for sender in &senders {
                                    if sender.send(msg.clone()).await.is_err() {
                                        break;
                                    }
                                }
                            }
                            
                            // Close all channels
                            drop(senders);
                        });
                        
                        // Wait for producer and all consumers
                        producer_handle.await.unwrap();
                        
                        let mut total_received = 0;
                        for handle in consumer_handles {
                            total_received += handle.await.unwrap();
                        }
                        
                        let duration = start_time.elapsed();
                        black_box((total_received, duration))
                    })
                }
            );
        }
    }
    group.finish();
}

/// Benchmark actor processing pipeline simulation
fn bench_actor_pipeline_simulation(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    
    let mut group = c.benchmark_group("actor_pipeline_simulation");
    group.measurement_time(Duration::from_secs(10));
    
    for &pipeline_length in &[2, 3, 4, 5] {
        for &buffer_size in &[100, 1000] {
            let messages_count = 1000;
            
            group.throughput(Throughput::Elements(messages_count as u64));
            
            group.bench_with_input(
                BenchmarkId::new(
                    "pipeline_stages",
                    format!("{}stages_buf{}", pipeline_length, buffer_size)
                ),
                &(pipeline_length, buffer_size, messages_count),
                |b, &(stages, buf_size, msg_count)| {
                    b.to_async(&rt).iter(|| async {
                        // Create pipeline of channels
                        let mut channels = Vec::new();
                        let mut stage_handles = Vec::new();
                        
                        // Create channels between stages
                        for stage_id in 0..stages {
                            let (tx, rx) = mpsc::channel::<SAIStatsMessage>(buf_size);
                            channels.push((tx, rx));
                        }
                        
                        // Create processing stages
                        for stage_id in 0..(stages - 1) {
                            let (_, mut input_rx) = channels[stage_id].clone();
                            let (output_tx, _) = channels[stage_id + 1].clone();
                            
                            let handle = tokio::spawn(async move {
                                let mut processed = 0;
                                while let Some(mut msg) = input_rx.recv().await {
                                    // Simulate processing work
                                    if let Some(stats) = Arc::get_mut(&mut msg) {
                                        for stat in &mut stats.stats {
                                            stat.counter += stage_id as u64; // Simulate modification
                                        }
                                    }
                                    
                                    // Forward to next stage
                                    if output_tx.send(black_box(msg)).await.is_err() {
                                        break;
                                    }
                                    processed += 1;
                                }
                                processed
                            });
                            stage_handles.push(handle);
                        }
                        
                        let start_time = Instant::now();
                        
                        // Input producer
                        let (first_tx, _) = &channels[0];
                        let producer_handle = tokio::spawn({
                            let first_tx = first_tx.clone();
                            async move {
                                for i in 0..msg_count {
                                    let msg = create_test_sai_stats_message(50, i as u64);
                                    if first_tx.send(black_box(msg)).await.is_err() {
                                        break;
                                    }
                                }
                            }
                        });
                        
                        // Final consumer
                        let (_, mut final_rx) = channels[stages - 1].clone();
                        let consumer_handle = tokio::spawn(async move {
                            let mut received = 0;
                            while let Some(msg) = final_rx.recv().await {
                                black_box(msg);
                                received += 1;
                                if received >= msg_count {
                                    break;
                                }
                            }
                            received
                        });
                        
                        // Wait for completion
                        producer_handle.await.unwrap();
                        let final_received = consumer_handle.await.unwrap();
                        
                        let duration = start_time.elapsed();
                        
                        // Wait for all processing stages
                        for handle in stage_handles {
                            handle.await.unwrap();
                        }
                        
                        black_box((final_received, duration))
                    })
                }
            );
        }
    }
    group.finish();
}

/// Benchmark different actor workload patterns
fn bench_actor_workload_patterns(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    
    let mut group = c.benchmark_group("actor_workload_patterns");
    group.measurement_time(Duration::from_secs(8));
    
    let patterns = [
        ("burst", WorkloadPattern::Burst),
        ("steady", WorkloadPattern::Steady),
        ("bursty", WorkloadPattern::Bursty),
    ];
    
    for (pattern_name, pattern) in patterns.iter() {
        for &buffer_size in &[100, 1000] {
            let message_count = 1000;
            
            group.bench_with_input(
                BenchmarkId::new("workload_pattern", format!("{}_buf{}", pattern_name, buffer_size)),
                &(*pattern, buffer_size, message_count),
                |b, &(workload_pattern, buf_size, msg_count)| {
                    b.to_async(&rt).iter(|| async {
                        let (tx, mut rx) = mpsc::channel::<SAIStatsMessage>(buf_size);
                        let start_time = Instant::now();
                        
                        // Producer with different patterns
                        let producer_handle = tokio::spawn(async move {
                            match workload_pattern {
                                WorkloadPattern::Burst => {
                                    // Send all messages as fast as possible
                                    for i in 0..msg_count {
                                        let msg = create_test_sai_stats_message(50, i as u64);
                                        if tx.send(black_box(msg)).await.is_err() {
                                            break;
                                        }
                                    }
                                }
                                WorkloadPattern::Steady => {
                                    // Send with small delays
                                    for i in 0..msg_count {
                                        let msg = create_test_sai_stats_message(50, i as u64);
                                        if tx.send(black_box(msg)).await.is_err() {
                                            break;
                                        }
                                        tokio::time::sleep(Duration::from_micros(10)).await;
                                    }
                                }
                                WorkloadPattern::Bursty => {
                                    // Send in bursts with pauses
                                    let burst_size = 50;
                                    let mut sent = 0;
                                    while sent < msg_count {
                                        // Send burst
                                        for i in 0..burst_size.min(msg_count - sent) {
                                            let msg = create_test_sai_stats_message(50, (sent + i) as u64);
                                            if tx.send(black_box(msg)).await.is_err() {
                                                return;
                                            }
                                        }
                                        sent += burst_size;
                                        
                                        // Pause between bursts
                                        if sent < msg_count {
                                            tokio::time::sleep(Duration::from_millis(1)).await;
                                        }
                                    }
                                }
                                WorkloadPattern::Random => {
                                    // Random delays (not implemented in this test)
                                    unreachable!()
                                }
                            }
                        });
                        
                        // Consumer
                        let mut received = 0;
                        while let Some(msg) = rx.recv().await {
                            black_box(msg);
                            received += 1;
                            if received >= msg_count {
                                break;
                            }
                        }
                        
                        let duration = start_time.elapsed();
                        producer_handle.await.unwrap();
                        
                        black_box((received, duration))
                    })
                }
            );
        }
    }
    group.finish();
}

/// Benchmark actor memory usage and Arc cloning patterns
fn bench_actor_memory_patterns(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    
    let mut group = c.benchmark_group("actor_memory_patterns");
    
    for &message_size in &[10, 100, 1000] {
        let message_count = 1000;
        
        // Test Arc cloning cost
        group.bench_with_input(
            BenchmarkId::new("arc_cloning", format("{}stats", message_size)),
            &(message_size, message_count),
            |b, &(msg_size, msg_count)| {
                b.to_async(&rt).iter(|| async {
                    let (tx, mut rx) = mpsc::channel::<SAIStatsMessage>(100);
                    
                    let producer_handle = tokio::spawn(async move {
                        for i in 0..msg_count {
                            let msg = create_test_sai_stats_message(msg_size, i as u64);
                            // This is an Arc clone
                            if tx.send(black_box(msg)).await.is_err() {
                                break;
                            }
                        }
                    });
                    
                    let mut received = 0;
                    while let Some(msg) = rx.recv().await {
                        // Simulate multiple consumers accessing the same data
                        let _clone1 = msg.clone(); // Arc clone
                        let _clone2 = msg.clone(); // Arc clone
                        black_box((_clone1, _clone2));
                        received += 1;
                        if received >= msg_count {
                            break;
                        }
                    }
                    
                    producer_handle.await.unwrap();
                    black_box(received)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark actor shutdown and cleanup performance
fn bench_actor_shutdown_patterns(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    
    let mut group = c.benchmark_group("actor_shutdown_patterns");
    
    for &actor_count in &[2, 5, 10] {
        group.bench_with_input(
            BenchmarkId::new("graceful_shutdown", format!("{}actors", actor_count)),
            &actor_count,
            |b, &count| {
                b.to_async(&rt).iter(|| async {
                    let mut shutdown_senders = Vec::new();
                    let mut actor_handles = Vec::new();
                    
                    // Create mock actors
                    for _ in 0..count {
                        let (shutdown_tx, mut shutdown_rx) = oneshot::channel::<()>();
                        let (msg_tx, mut msg_rx) = mpsc::channel::<SAIStatsMessage>(100);
                        
                        shutdown_senders.push(shutdown_tx);
                        
                        let actor_handle = tokio::spawn(async move {
                            let mut messages_processed = 0;
                            loop {
                                tokio::select! {
                                    msg = msg_rx.recv() => {
                                        if let Some(msg) = msg {
                                            black_box(msg);
                                            messages_processed += 1;
                                        }
                                    }
                                    _ = &mut shutdown_rx => {
                                        break;
                                    }
                                }
                            }
                            messages_processed
                        });
                        
                        actor_handles.push(actor_handle);
                    }
                    
                    let start_time = Instant::now();
                    
                    // Send shutdown signals
                    for sender in shutdown_senders {
                        let _ = sender.send(());
                    }
                    
                    // Wait for all actors to shut down
                    let mut total_processed = 0;
                    for handle in actor_handles {
                        total_processed += handle.await.unwrap();
                    }
                    
                    let shutdown_duration = start_time.elapsed();
                    
                    black_box((total_processed, shutdown_duration))
                })
            }
        );
    }
    group.finish();
}

criterion_group!(
    benches,
    bench_message_passing_throughput,
    bench_multi_producer_throughput,
    bench_single_producer_multi_consumer,
    bench_actor_pipeline_simulation,
    bench_actor_workload_patterns,
    bench_actor_memory_patterns,
    bench_actor_shutdown_patterns
);
criterion_main!(benches);
