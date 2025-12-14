use criterion::{black_box, criterion_group, criterion_main, Criterion, BenchmarkId, Throughput};
use countersyncd::message::{
    saistats::{SAIStat, SAIStats, SAIStatsMessage, SAIStatsMessageExt},
    otel::{OtelMetrics, OtelMetricsMessageExt, OtelGauge, OtelDataPoint, OtelAttribute},
    ipfix::IPFixTemplatesMessage,
    buffer::SocketBufferMessage,
};
use ipfixrw::parser::{DataRecordValue, FieldSpecifier};
use byteorder::{ByteOrder, NetworkEndian};
use opentelemetry_proto::tonic::{
    common::v1::{KeyValue as ProtoKeyValue, AnyValue, any_value::Value, InstrumentationScope},
    metrics::v1::{Metric, Gauge as ProtoGauge, ResourceMetrics, ScopeMetrics},
    resource::v1::Resource as ProtoResource,
    collector::metrics::v1::ExportMetricsServiceRequest,
};
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::sync::mpsc;

/// End-to-end pipeline scenarios for benchmarking
#[derive(Clone, Copy)]
enum PipelineScenario {
    LightTraffic,      // Low volume, typical home/office network
    MediumTraffic,     // Medium volume, enterprise network
    HeavyTraffic,      // High volume, data center network
    BurstTraffic,      // Intermittent high bursts
    MixedWorkload,     // Realistic mixed traffic patterns
    ScaleTest,         // Maximum throughput testing
}

/// Network interface patterns for realistic testing
#[derive(Clone, Copy)]
enum InterfacePattern {
    SmallSwitch,       // 24-48 ports
    MediumSwitch,      // 48-96 ports  
    LargeSwitch,       // 96+ ports
    DataCenter,        // High-density switching
    Mixed,             // Various interface types
}

/// Helper function to create realistic IPFIX data for end-to-end testing
fn create_realistic_ipfix_data(
    interface_count: usize,
    stats_per_interface: usize,
    scenario: PipelineScenario,
) -> (Vec<String>, Vec<(FieldSpecifier, DataRecordValue)>) {
    let object_names: Vec<String> = (0..interface_count)
        .map(|i| match i % 5 {
            0 => format!("Ethernet{}", i),
            1 => format!("Port-Channel{}", i / 5),
            2 => format!("Vlan{}", 100 + i),
            3 => format!("BufferPool{}", i / 10),
            _ => format!("Queue_Ethernet{}_{}", i / 5, i % 8),
        })
        .collect();

    let mut ipfix_records = Vec::new();
    
    for interface_idx in 0..interface_count {
        for stat_idx in 0..stats_per_interface {
            let (enterprise_number, counter_value) = match scenario {
                PipelineScenario::LightTraffic => {
                    let type_id = 1u32; // Port stats mainly
                    let stat_id = 1 + (stat_idx % 4) as u32; // RX/TX packets/bytes
                    let enterprise = (type_id << 16) | stat_id;
                    let counter = 1000 + (interface_idx * 100 + stat_idx) as u64;
                    (enterprise, counter)
                }
                PipelineScenario::MediumTraffic => {
                    let type_id = 1 + (stat_idx % 3) as u32; // Port, Buffer, Queue
                    let stat_id = 1 + (stat_idx % 10) as u32;
                    let enterprise = (type_id << 16) | stat_id;
                    let counter = 10000 + (interface_idx * 1000 + stat_idx * 100) as u64;
                    (enterprise, counter)
                }
                PipelineScenario::HeavyTraffic => {
                    let type_id = 1 + (stat_idx % 5) as u32;
                    let stat_id = 1 + (stat_idx % 20) as u32;
                    let enterprise = (type_id << 16) | stat_id;
                    let counter = 100000 + (interface_idx * 10000 + stat_idx * 1000) as u64;
                    (enterprise, counter)
                }
                PipelineScenario::BurstTraffic => {
                    let type_id = 1u32;
                    let stat_id = 1 + (stat_idx % 4) as u32;
                    let enterprise = (type_id << 16) | stat_id;
                    // Simulate burst - much higher counters
                    let burst_multiplier = if interface_idx % 10 < 3 { 1000 } else { 10 };
                    let counter = (interface_idx * 1000 + stat_idx * 100) as u64 * burst_multiplier;
                    (enterprise, counter)
                }
                PipelineScenario::MixedWorkload => {
                    let pattern = (interface_idx + stat_idx) % 6;
                    let (type_id, stat_id, base_counter) = match pattern {
                        0 => (1, 1, 5000),   // Port RX packets
                        1 => (1, 2, 50000),  // Port RX bytes  
                        2 => (1, 3, 3000),   // Port TX packets
                        3 => (1, 4, 30000),  // Port TX bytes
                        4 => (24, 1, 1000),  // Buffer pool usage
                        _ => (5, 1, 500),    // Queue depth
                    };
                    let enterprise = (type_id << 16) | stat_id;
                    let counter = base_counter + (interface_idx * 100 + stat_idx * 10) as u64;
                    (enterprise, counter)
                }
                PipelineScenario::ScaleTest => {
                    let type_id = 1 + (stat_idx % 10) as u32;
                    let stat_id = 1 + (stat_idx % 50) as u32;
                    let enterprise = (type_id << 16) | stat_id;
                    let counter = 1000000 + (interface_idx * 100000 + stat_idx * 1000) as u64;
                    (enterprise, counter)
                }
            };

            let label = 1 + (interface_idx % object_names.len()) as u16;
            let field_spec = FieldSpecifier::new(Some(enterprise_number), label, 8);
            
            let mut bytes = [0u8; 8];
            NetworkEndian::write_u64(&mut bytes, counter_value);
            let value = DataRecordValue::Bytes(bytes.to_vec());
            
            ipfix_records.push((field_spec, value));
        }
    }

    (object_names, ipfix_records)
}

/// Helper function to determine interface count based on pattern
fn get_interface_count(pattern: InterfacePattern) -> usize {
    match pattern {
        InterfacePattern::SmallSwitch => 48,
        InterfacePattern::MediumSwitch => 96,
        InterfacePattern::LargeSwitch => 256,
        InterfacePattern::DataCenter => 512,
        InterfacePattern::Mixed => 128,
    }
}

/// Benchmark complete IPFIX → SAI → OTel pipeline
fn bench_complete_pipeline(c: &mut Criterion) {
    let mut group = c.benchmark_group("complete_pipeline");
    group.measurement_time(Duration::from_secs(10));
    
    let scenarios = [
        ("light_traffic", PipelineScenario::LightTraffic, InterfacePattern::SmallSwitch, 4),
        ("medium_traffic", PipelineScenario::MediumTraffic, InterfacePattern::MediumSwitch, 8),
        ("heavy_traffic", PipelineScenario::HeavyTraffic, InterfacePattern::LargeSwitch, 12),
        ("burst_traffic", PipelineScenario::BurstTraffic, InterfacePattern::SmallSwitch, 6),
        ("mixed_workload", PipelineScenario::MixedWorkload, InterfacePattern::Mixed, 10),
    ];
    
    for (scenario_name, scenario, interface_pattern, stats_per_interface) in scenarios.iter() {
        let interface_count = get_interface_count(*interface_pattern);
        let total_stats = interface_count * stats_per_interface;
        let (object_names, ipfix_records) = create_realistic_ipfix_data(
            interface_count, 
            *stats_per_interface, 
            *scenario
        );
        
        group.throughput(Throughput::Elements(total_stats as u64));
        
        group.bench_with_input(
            BenchmarkId::new("full_pipeline", format!("{}_{}stats", scenario_name, total_stats)),
            &(object_names, ipfix_records),
            |b, (names, records)| {
                b.iter(|| {
                    // Step 1: IPFIX → SAI Stats conversion
                    let mut sai_stats = Vec::with_capacity(records.len());
                    let observation_time = 1672531200000000000u64;
                    
                    for (field_spec, value) in records {
                        let stat = SAIStat::from_ipfix(
                            black_box(field_spec),
                            black_box(value),
                            black_box(names)
                        );
                        sai_stats.push(stat);
                    }
                    
                    let sai_stats_collection = SAIStats::new(observation_time, sai_stats);
                    
                    // Step 2: SAI Stats → OTel conversion
                    let otel_metrics = OtelMetrics::from_sai_stats(black_box(&sai_stats_collection));
                    
                    // Step 3: OTel → Protobuf conversion (export preparation)
                    let proto_metrics: Vec<Metric> = otel_metrics.gauges.iter().map(|gauge| {
                        let proto_data_points = gauge.data_points.iter()
                            .map(|dp| dp.to_proto())
                            .collect();

                        let proto_gauge = ProtoGauge {
                            data_points: proto_data_points,
                        };

                        Metric {
                            name: gauge.name.clone(),
                            description: gauge.description.clone(),
                            metadata: vec![],
                            data: Some(opentelemetry_proto::tonic::metrics::v1::metric::Data::Gauge(proto_gauge)),
                            ..Default::default()
                        }
                    }).collect();
                    
                    black_box((sai_stats_collection, otel_metrics, proto_metrics))
                })
            }
        );
    }
    group.finish();
}

/// Benchmark pipeline stages individually to identify bottlenecks
fn bench_pipeline_stage_breakdown(c: &mut Criterion) {
    let mut group = c.benchmark_group("pipeline_stage_breakdown");
    
    let interface_count = 128;
    let stats_per_interface = 8;
    let (object_names, ipfix_records) = create_realistic_ipfix_data(
        interface_count, 
        stats_per_interface, 
        PipelineScenario::MixedWorkload
    );
    
    let total_stats = interface_count * stats_per_interface;
    group.throughput(Throughput::Elements(total_stats as u64));
    
    // Stage 1: IPFIX parsing only
    group.bench_with_input(
        BenchmarkId::new("stage1_ipfix_parsing", total_stats),
        &(object_names.clone(), ipfix_records.clone()),
        |b, (names, records)| {
            b.iter(|| {
                let mut sai_stats = Vec::with_capacity(records.len());
                
                for (field_spec, value) in records {
                    let stat = SAIStat::from_ipfix(
                        black_box(field_spec),
                        black_box(value),
                        black_box(names)
                    );
                    sai_stats.push(stat);
                }
                
                black_box(sai_stats)
            })
        }
    );
    
    // Stage 2: SAI Stats creation
    let sai_stats: Vec<SAIStat> = ipfix_records.iter()
        .map(|(field_spec, value)| SAIStat::from_ipfix(field_spec, value, &object_names))
        .collect();
    
    group.bench_with_input(
        BenchmarkId::new("stage2_sai_stats_creation", total_stats),
        &sai_stats,
        |b, stats| {
            b.iter(|| {
                let observation_time = 1672531200000000000u64;
                let sai_stats_collection = SAIStats::new(observation_time, black_box(stats.clone()));
                black_box(sai_stats_collection)
            })
        }
    );
    
    // Stage 3: OTel conversion
    let sai_stats_collection = SAIStats::new(1672531200000000000u64, sai_stats);
    
    group.bench_with_input(
        BenchmarkId::new("stage3_otel_conversion", total_stats),
        &sai_stats_collection,
        |b, stats| {
            b.iter(|| {
                let otel_metrics = OtelMetrics::from_sai_stats(black_box(stats));
                black_box(otel_metrics)
            })
        }
    );
    
    // Stage 4: Protobuf conversion
    let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats_collection);
    
    group.bench_with_input(
        BenchmarkId::new("stage4_protobuf_conversion", total_stats),
        &otel_metrics,
        |b, metrics| {
            b.iter(|| {
                let proto_metrics: Vec<Metric> = metrics.gauges.iter().map(|gauge| {
                    let proto_data_points = gauge.data_points.iter()
                        .map(|dp| dp.to_proto())
                        .collect();

                    let proto_gauge = ProtoGauge {
                        data_points: proto_data_points,
                    };

                    Metric {
                        name: gauge.name.clone(),
                        description: gauge.description.clone(),
                        metadata: vec![],
                        data: Some(opentelemetry_proto::tonic::metrics::v1::metric::Data::Gauge(proto_gauge)),
                        ..Default::default()
                    }
                }).collect();
                
                black_box(proto_metrics)
            })
        }
    );
    
    group.finish();
}

/// Benchmark realistic actor-based pipeline simulation
fn bench_actor_pipeline_simulation(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let mut group = c.benchmark_group("actor_pipeline_simulation");
    group.measurement_time(Duration::from_secs(12));
    
    let scenarios = [
        ("small_load", 24, 4, 100),    // 24 interfaces, 4 stats each, 100 messages
        ("medium_load", 96, 8, 200),   // 96 interfaces, 8 stats each, 200 messages  
        ("large_load", 256, 12, 500),  // 256 interfaces, 12 stats each, 500 messages
    ];
    
    for (scenario_name, interface_count, stats_per_interface, message_count) in scenarios.iter() {
        let total_stats_per_message = interface_count * stats_per_interface;
        
        group.throughput(Throughput::Elements((total_stats_per_message * message_count) as u64));
        
        group.bench_with_input(
            BenchmarkId::new(
                "actor_simulation", 
                format!("{}_{}total", scenario_name, total_stats_per_message * message_count)
            ),
            &(*interface_count, *stats_per_interface, *message_count),
            |b, &(interfaces, stats_per_intf, msg_count)| {
                b.to_async(&rt).iter(|| async {
                    // Create channels for actor communication
                    let (ipfix_tx, mut ipfix_rx) = mpsc::channel::<(Vec<String>, Vec<(FieldSpecifier, DataRecordValue)>)>(100);
                    let (sai_tx, mut sai_rx) = mpsc::channel::<SAIStatsMessage>(100);
                    let (otel_tx, mut otel_rx) = mpsc::channel::<Arc<OtelMetrics>>(100);
                    
                    // IPFIX processing actor simulation
                    let ipfix_processor = tokio::spawn(async move {
                        let mut processed = 0;
                        while let Some((object_names, ipfix_records)) = ipfix_rx.recv().await {
                            let mut sai_stats = Vec::with_capacity(ipfix_records.len());
                            
                            for (field_spec, value) in ipfix_records {
                                let stat = SAIStat::from_ipfix(&field_spec, &value, &object_names);
                                sai_stats.push(stat);
                            }
                            
                            let sai_stats_collection = SAIStats::new(
                                1672531200000000000u64 + processed, 
                                sai_stats
                            );
                            let message = Arc::new(sai_stats_collection);
                            
                            if sai_tx.send(message).await.is_err() {
                                break;
                            }
                            processed += 1;
                        }
                        processed
                    });
                    
                    // OTel processing actor simulation
                    let otel_processor = tokio::spawn(async move {
                        let mut converted = 0;
                        while let Some(sai_message) = sai_rx.recv().await {
                            let otel_metrics = OtelMetrics::from_sai_stats(&sai_message);
                            let otel_message = Arc::new(otel_metrics);
                            
                            if otel_tx.send(otel_message).await.is_err() {
                                break;
                            }
                            converted += 1;
                        }
                        converted
                    });
                    
                    // Export processor simulation
                    let export_processor = tokio::spawn(async move {
                        let mut exported = 0;
                        while let Some(otel_message) = otel_rx.recv().await {
                            // Simulate export preparation
                            let _proto_metrics: Vec<Metric> = otel_message.gauges.iter().map(|gauge| {
                                let proto_data_points = gauge.data_points.iter()
                                    .map(|dp| dp.to_proto())
                                    .collect();

                                let proto_gauge = ProtoGauge {
                                    data_points: proto_data_points,
                                };

                                Metric {
                                    name: gauge.name.clone(),
                                    description: gauge.description.clone(),
                                    metadata: vec![],
                                    data: Some(opentelemetry_proto::tonic::metrics::v1::metric::Data::Gauge(proto_gauge)),
                                    ..Default::default()
                                }
                            }).collect();
                            
                            exported += 1;
                        }
                        exported
                    });
                    
                    // Data producer
                    let producer = tokio::spawn(async move {
                        let mut sent = 0;
                        for i in 0..msg_count {
                            let (object_names, ipfix_records) = create_realistic_ipfix_data(
                                interfaces,
                                stats_per_intf,
                                PipelineScenario::MixedWorkload,
                            );
                            
                            if ipfix_tx.send((object_names, ipfix_records)).await.is_err() {
                                break;
                            }
                            sent += 1;
                            
                            // Simulate realistic message intervals
                            if i % 10 == 0 {
                                tokio::time::sleep(Duration::from_millis(1)).await;
                            }
                        }
                        sent
                    });
                    
                    // Wait for all processing to complete
                    let (sent, processed, converted, exported) = tokio::join!(
                        producer,
                        ipfix_processor,
                        otel_processor,
                        export_processor
                    );
                    
                    black_box((
                        sent.unwrap(),
                        processed.unwrap(),
                        converted.unwrap(),
                        exported.unwrap()
                    ))
                })
            }
        );
    }
    group.finish();
}

/// Benchmark throughput under different load patterns
fn bench_throughput_under_load(c: &mut Criterion) {
    let mut group = c.benchmark_group("throughput_under_load");
    group.measurement_time(Duration::from_secs(15));
    
    let load_patterns = [
        ("constant_low", 50, Duration::from_millis(100)),
        ("constant_medium", 200, Duration::from_millis(50)),
        ("constant_high", 1000, Duration::from_millis(10)),
        ("bursty_low", 100, Duration::from_millis(200)),
        ("bursty_high", 500, Duration::from_millis(20)),
    ];
    
    for (pattern_name, stats_per_message, interval) in load_patterns.iter() {
        group.throughput(Throughput::Elements(*stats_per_message as u64));
        
        group.bench_with_input(
            BenchmarkId::new("sustained_throughput", pattern_name),
            &(*stats_per_message, *interval),
            |b, &(stats_count, sleep_duration)| {
                b.iter(|| {
                    let start_time = Instant::now();
                    let mut total_processed = 0;
                    
                    // Simulate processing for a fixed duration
                    while start_time.elapsed() < Duration::from_millis(100) {
                        let (object_names, ipfix_records) = create_realistic_ipfix_data(
                            stats_count / 8, // Assume 8 stats per interface
                            8,
                            PipelineScenario::MixedWorkload,
                        );
                        
                        // Full pipeline processing
                        let mut sai_stats = Vec::with_capacity(ipfix_records.len());
                        for (field_spec, value) in &ipfix_records {
                            let stat = SAIStat::from_ipfix(field_spec, value, &object_names);
                            sai_stats.push(stat);
                        }
                        
                        let sai_stats_collection = SAIStats::new(
                            start_time.elapsed().as_nanos() as u64, 
                            sai_stats
                        );
                        let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats_collection);
                        let _proto_metrics: Vec<_> = otel_metrics.gauges.iter()
                            .map(|g| g.data_points.iter().map(|dp| dp.to_proto()).collect::<Vec<_>>())
                            .collect();
                        
                        total_processed += ipfix_records.len();
                        
                        // Simulate load interval
                        std::thread::sleep(sleep_duration / 10); // Scale down for benchmark
                    }
                    
                    black_box(total_processed)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark resource utilization patterns
fn bench_resource_utilization(c: &mut Criterion) {
    let mut group = c.benchmark_group("resource_utilization");
    
    for &scale_factor in &[1, 5, 10, 20] {
        let base_interfaces = 64;
        let base_stats = 8;
        let interfaces = base_interfaces * scale_factor;
        let stats_per_interface = base_stats;
        let total_stats = interfaces * stats_per_interface;
        
        group.throughput(Throughput::Elements(total_stats as u64));
        
        group.bench_with_input(
            BenchmarkId::new("resource_scaling", format!("{}x_scale_{}stats", scale_factor, total_stats)),
            &(interfaces, stats_per_interface),
            |b, &(interface_count, stats_count)| {
                b.iter(|| {
                    // Memory allocation tracking simulation
                    let mut memory_allocations = 0;
                    let mut processing_steps = 0;
                    
                    // Create data
                    let (object_names, ipfix_records) = create_realistic_ipfix_data(
                        interface_count,
                        stats_count,
                        PipelineScenario::MixedWorkload,
                    );
                    memory_allocations += 2; // object_names + ipfix_records
                    
                    // Process through pipeline
                    let mut sai_stats = Vec::with_capacity(ipfix_records.len());
                    memory_allocations += 1; // sai_stats vector
                    
                    for (field_spec, value) in &ipfix_records {
                        let stat = SAIStat::from_ipfix(field_spec, value, &object_names);
                        sai_stats.push(stat);
                        processing_steps += 1;
                    }
                    
                    let sai_stats_collection = SAIStats::new(1672531200000000000u64, sai_stats);
                    memory_allocations += 1; // sai_stats_collection
                    processing_steps += 1;
                    
                    let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats_collection);
                    memory_allocations += 1; // otel_metrics
                    processing_steps += 1;
                    
                    let proto_metrics: Vec<Metric> = otel_metrics.gauges.iter().map(|gauge| {
                        processing_steps += 1;
                        let proto_data_points = gauge.data_points.iter()
                            .map(|dp| {
                                processing_steps += 1; 
                                dp.to_proto()
                            })
                            .collect();

                        let proto_gauge = ProtoGauge {
                            data_points: proto_data_points,
                        };

                        Metric {
                            name: gauge.name.clone(),
                            description: gauge.description.clone(),
                            metadata: vec![],
                            data: Some(opentelemetry_proto::tonic::metrics::v1::metric::Data::Gauge(proto_gauge)),
                            ..Default::default()
                        }
                    }).collect();
                    memory_allocations += 1; // proto_metrics
                    
                    black_box((memory_allocations, processing_steps, proto_metrics.len()))
                })
            }
        );
    }
    group.finish();
}

/// Benchmark error handling and recovery patterns
fn bench_error_handling_patterns(c: &mut Criterion) {
    let mut group = c.benchmark_group("error_handling_patterns");
    
    let error_scenarios = [
        ("no_errors", 0.0),           // 0% error rate
        ("low_errors", 0.01),         // 1% error rate
        ("medium_errors", 0.05),      // 5% error rate  
        ("high_errors", 0.10),        // 10% error rate
    ];
    
    for (scenario_name, error_rate) in error_scenarios.iter() {
        let interface_count = 128;
        let stats_per_interface = 8;
        let total_stats = interface_count * stats_per_interface;
        
        group.bench_with_input(
            BenchmarkId::new("error_resilience", format!("{}_{:.0}pct", scenario_name, error_rate * 100.0)),
            &(interface_count, stats_per_interface, *error_rate),
            |b, &(interfaces, stats_per_intf, err_rate)| {
                b.iter(|| {
                    let (object_names, mut ipfix_records) = create_realistic_ipfix_data(
                        interfaces,
                        stats_per_intf,
                        PipelineScenario::MixedWorkload,
                    );
                    
                    // Inject errors
                    let error_count = (ipfix_records.len() as f64 * err_rate) as usize;
                    for i in 0..error_count {
                        let error_index = i * (ipfix_records.len() / error_count.max(1));
                        if error_index < ipfix_records.len() {
                            // Corrupt the data to simulate parse errors
                            ipfix_records[error_index].1 = DataRecordValue::Bytes(vec![0xFF; 4]); // Invalid data
                        }
                    }
                    
                    // Process with error handling
                    let mut successful_stats = Vec::new();
                    let mut error_count = 0;
                    
                    for (field_spec, value) in &ipfix_records {
                        // Simulate error detection
                        if let DataRecordValue::Bytes(bytes) = value {
                            if bytes.len() < 8 || bytes.iter().all(|&b| b == 0xFF) {
                                error_count += 1;
                                continue; // Skip corrupted data
                            }
                        }
                        
                        let stat = SAIStat::from_ipfix(field_spec, value, &object_names);
                        successful_stats.push(stat);
                    }
                    
                    if !successful_stats.is_empty() {
                        let sai_stats_collection = SAIStats::new(1672531200000000000u64, successful_stats);
                        let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats_collection);
                        black_box((otel_metrics.len(), error_count))
                    } else {
                        black_box((0, error_count))
                    }
                })
            }
        );
    }
    group.finish();
}

criterion_group!(
    benches,
    bench_complete_pipeline,
    bench_pipeline_stage_breakdown,
    bench_actor_pipeline_simulation,
    bench_throughput_under_load,
    bench_resource_utilization,
    bench_error_handling_patterns
);
criterion_main!(benches);
