use criterion::{black_box, criterion_group, criterion_main, Criterion, BenchmarkId, Throughput};
use countersyncd::message::{
    saistats::{SAIStat, SAIStats, SAIStatsMessageExt},
    otel::{OtelMetrics, OtelMetricsMessageExt, OtelGauge, OtelDataPoint, OtelAttribute},
};
use std::sync::Arc;
use std::time::Duration;

/// Helper function to create test SAI statistics with various patterns
fn create_test_sai_stats(stat_count: usize, pattern: TestPattern) -> SAIStats {
    let stats = match pattern {
        TestPattern::Sequential => (0..stat_count)
            .map(|i| SAIStat {
                object_name: format!("Ethernet{}", i),
                type_id: 1 + (i % 10) as u32,
                stat_id: 1 + (i % 20) as u32,
                counter: 1000 + i as u64,
            })
            .collect(),
        TestPattern::PortStats => (0..stat_count)
            .map(|i| SAIStat {
                object_name: format!("Ethernet{}", i / 4), // 4 stats per port
                type_id: 1, // SAI_OBJECT_TYPE_PORT
                stat_id: 1 + (i % 4) as u32, // RX_PACKETS, TX_PACKETS, RX_BYTES, TX_BYTES
                counter: 10000 + (i * 1000) as u64,
            })
            .collect(),
        TestPattern::BufferStats => (0..stat_count)
            .map(|i| SAIStat {
                object_name: format!("BufferPool{}", i / 2),
                type_id: 24, // SAI_OBJECT_TYPE_BUFFER_POOL
                stat_id: 1 + (i % 2) as u32, // CURR_OCCUPANCY_BYTES, WATERMARK_BYTES
                counter: 50000 + (i * 5000) as u64,
            })
            .collect(),
        TestPattern::Mixed => (0..stat_count)
            .map(|i| {
                match i % 3 {
                    0 => SAIStat {
                        object_name: format!("Ethernet{}", i / 3),
                        type_id: 1, // Port
                        stat_id: 1 + (i % 4) as u32,
                        counter: 10000 + i as u64,
                    },
                    1 => SAIStat {
                        object_name: format!("BufferPool{}", i / 3),
                        type_id: 24, // Buffer pool
                        stat_id: 1 + (i % 2) as u32,
                        counter: 50000 + i as u64,
                    },
                    _ => SAIStat {
                        object_name: format!("Queue{}", i / 3),
                        type_id: 5, // Queue
                        stat_id: 1 + (i % 6) as u32,
                        counter: 1000 + i as u64,
                    },
                }
            })
            .collect(),
    };
    
    SAIStats::new(1672531200000000000u64, stats) // 2023-01-01 00:00:00 UTC in nanoseconds
}

#[derive(Clone, Copy)]
enum TestPattern {
    Sequential,
    PortStats,
    BufferStats,
    Mixed,
}

/// Benchmark SAI to OpenTelemetry conversion at different scales
fn bench_sai_to_otel_conversion_scale(c: &mut Criterion) {
    let mut group = c.benchmark_group("sai_to_otel_scale");
    group.measurement_time(Duration::from_secs(10));
    
    for &size in &[10, 50, 100, 500, 1000, 5000, 10000] {
        group.throughput(Throughput::Elements(size as u64));
        
        let sai_stats = create_test_sai_stats(size, TestPattern::Sequential);
        
        group.bench_with_input(
            BenchmarkId::new("full_conversion", size), 
            &sai_stats, 
            |b, stats| {
                b.iter(|| {
                    let otel_metrics = OtelMetrics::from_sai_stats(black_box(stats));
                    black_box(otel_metrics)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark different SAI statistic patterns
fn bench_sai_to_otel_patterns(c: &mut Criterion) {
    let mut group = c.benchmark_group("sai_to_otel_patterns");
    group.measurement_time(Duration::from_secs(8));
    
    let patterns = [
        ("sequential", TestPattern::Sequential),
        ("port_stats", TestPattern::PortStats),
        ("buffer_stats", TestPattern::BufferStats),
        ("mixed_types", TestPattern::Mixed),
    ];
    
    for &size in &[100, 1000] {
        for (pattern_name, pattern) in patterns.iter() {
            let sai_stats = create_test_sai_stats(size, *pattern);
            
            group.bench_with_input(
                BenchmarkId::new("pattern", format!("{}_{}", pattern_name, size)),
                &sai_stats,
                |b, stats| {
                    b.iter(|| {
                        let otel_metrics = OtelMetrics::from_sai_stats(black_box(stats));
                        black_box(otel_metrics)
                    })
                }
            );
        }
    }
    group.finish();
}

/// Benchmark individual OtelGauge creation from SAI stats
fn bench_otel_gauge_creation(c: &mut Criterion) {
    let mut group = c.benchmark_group("otel_gauge_creation");
    
    for &size in &[10, 100, 1000] {
        group.throughput(Throughput::Elements(size as u64));
        
        let sai_stats = create_test_sai_stats(size, TestPattern::Sequential);
        
        group.bench_with_input(
            BenchmarkId::new("from_sai_stats", size),
            &sai_stats,
            |b, stats| {
                b.iter(|| {
                    let gauges = OtelGauge::from_sai_stats(black_box(stats));
                    black_box(gauges)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark OtelDataPoint creation from individual SAI stats
fn bench_otel_data_point_creation(c: &mut Criterion) {
    let mut group = c.benchmark_group("otel_data_point_creation");
    
    let test_stats = [
        SAIStat {
            object_name: "Ethernet0".to_string(),
            type_id: 1,
            stat_id: 1,
            counter: 12345,
        },
        SAIStat {
            object_name: "BufferPool1".to_string(),
            type_id: 24,
            stat_id: 2,
            counter: 67890,
        },
        SAIStat {
            object_name: "Queue_Ethernet0_5".to_string(),
            type_id: 5,
            stat_id: 3,
            counter: 999999,
        },
    ];
    
    for (i, stat) in test_stats.iter().enumerate() {
        group.bench_with_input(
            BenchmarkId::new("from_sai_stat", format!("type_{}", stat.type_id)),
            stat,
            |b, sai_stat| {
                b.iter(|| {
                    let data_point = OtelDataPoint::from_sai_stat(
                        black_box(sai_stat), 
                        black_box(1672531200000000000u64)
                    );
                    black_box(data_point)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark OtelAttribute creation and manipulation
fn bench_otel_attribute_operations(c: &mut Criterion) {
    let mut group = c.benchmark_group("otel_attribute_ops");
    
    // Benchmark attribute creation
    group.bench_function("create_attribute", |b| {
        b.iter(|| {
            let attr = OtelAttribute::new(
                black_box("object_name".to_string()),
                black_box("Ethernet0".to_string())
            );
            black_box(attr)
        })
    });
    
    // Benchmark protobuf conversion
    let test_attr = OtelAttribute::new("sai_type_id", "100");
    group.bench_function("attribute_to_proto", |b| {
        b.iter(|| {
            let proto = black_box(&test_attr).to_proto();
            black_box(proto)
        })
    });
    
    // Benchmark multiple attribute creation (typical for one data point)
    group.bench_function("create_three_attributes", |b| {
        b.iter(|| {
            let attrs = vec![
                OtelAttribute::new(black_box("object_name"), black_box("Ethernet0")),
                OtelAttribute::new(black_box("sai_type_id"), black_box("1")),
                OtelAttribute::new(black_box("sai_stat_id"), black_box("1")),
            ];
            black_box(attrs)
        })
    });
    
    group.finish();
}

/// Benchmark protobuf conversion performance
fn bench_otel_protobuf_conversion(c: &mut Criterion) {
    let mut group = c.benchmark_group("otel_protobuf_conversion");
    
    for &size in &[10, 100, 1000] {
        let sai_stats = create_test_sai_stats(size, TestPattern::Mixed);
        let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats);
        
        group.throughput(Throughput::Elements(size as u64));
        
        group.bench_with_input(
            BenchmarkId::new("data_points_to_proto", size),
            &otel_metrics,
            |b, metrics| {
                b.iter(|| {
                    let mut proto_count = 0;
                    for gauge in &metrics.gauges {
                        for data_point in &gauge.data_points {
                            let proto = data_point.to_proto();
                            black_box(proto);
                            proto_count += 1;
                        }
                    }
                    black_box(proto_count)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark memory allocation patterns
fn bench_otel_memory_patterns(c: &mut Criterion) {
    let mut group = c.benchmark_group("otel_memory_patterns");
    
    let sai_stats = create_test_sai_stats(1000, TestPattern::Mixed);
    
    // Benchmark direct conversion
    group.bench_function("direct_conversion", |b| {
        b.iter(|| {
            let otel_metrics = OtelMetrics::from_sai_stats(black_box(&sai_stats));
            black_box(otel_metrics)
        })
    });
    
    // Benchmark Arc wrapping
    group.bench_function("arc_wrapped_conversion", |b| {
        b.iter(|| {
            let otel_metrics = OtelMetrics::from_sai_stats(black_box(&sai_stats));
            let message = otel_metrics.into_message();
            black_box(message)
        })
    });
    
    // Benchmark cloning Arc-wrapped metrics
    let otel_message = OtelMetrics::from_sai_stats(&sai_stats).into_message();
    group.bench_function("arc_clone", |b| {
        b.iter(|| {
            let cloned = black_box(&otel_message).clone();
            black_box(cloned)
        })
    });
    
    group.finish();
}

/// Benchmark string formatting operations (metric names, descriptions)
fn bench_otel_string_formatting(c: &mut Criterion) {
    let mut group = c.benchmark_group("otel_string_formatting");
    
    let test_cases = [
        (1, 1, "Ethernet0"),
        (24, 2, "BufferPool1"),
        (5, 3, "Queue_Ethernet0_5"),
        (100, 200, "VeryLongObjectNameForTesting_WithSpecialChars_123"),
    ];
    
    for (type_id, stat_id, object_name) in test_cases.iter() {
        group.bench_with_input(
            BenchmarkId::new("metric_name_format", format!("{}_{}", type_id, stat_id)),
            &(*type_id, *stat_id),
            |b, (type_id, stat_id)| {
                b.iter(|| {
                    let name = format!(
                        "sai_counter_type_{}_stat_{}", 
                        black_box(type_id), 
                        black_box(stat_id)
                    );
                    black_box(name)
                })
            }
        );
        
        group.bench_with_input(
            BenchmarkId::new("metric_description_format", format!("{}_{}", type_id, stat_id)),
            &(*type_id, *stat_id, object_name.to_string()),
            |b, (type_id, stat_id, object_name)| {
                b.iter(|| {
                    let description = format!(
                        "SAI counter for object {} (type:{}, stat:{})",
                        black_box(object_name),
                        black_box(type_id),
                        black_box(stat_id)
                    );
                    black_box(description)
                })
            }
        );
    }
    
    group.finish();
}

/// Benchmark end-to-end conversion with different optimization strategies
fn bench_otel_optimization_strategies(c: &mut Criterion) {
    let mut group = c.benchmark_group("otel_optimization_strategies");
    group.measurement_time(Duration::from_secs(8));
    
    let sai_stats = create_test_sai_stats(1000, TestPattern::Mixed);
    
    // Current implementation
    group.bench_function("current_implementation", |b| {
        b.iter(|| {
            let otel_metrics = OtelMetrics::from_sai_stats(black_box(&sai_stats));
            black_box(otel_metrics)
        })
    });
    
    // Pre-allocate vectors optimization simulation
    group.bench_function("simulated_preallocation", |b| {
        b.iter(|| {
            let mut gauges = Vec::with_capacity(black_box(sai_stats.stats.len()));
            for stat in &sai_stats.stats {
                let gauge = OtelGauge::from_sai_stat(stat, sai_stats.observation_time);
                gauges.push(black_box(gauge));
            }
            let otel_metrics = OtelMetrics {
                service_name: "countersyncd".to_string(),
                scope_name: "countersyncd".to_string(),
                scope_version: "1.0".to_string(),
                gauges: black_box(gauges),
            };
            black_box(otel_metrics)
        })
    });
    
    group.finish();
}

criterion_group!(
    benches,
    bench_sai_to_otel_conversion_scale,
    bench_sai_to_otel_patterns,
    bench_otel_gauge_creation,
    bench_otel_data_point_creation,
    bench_otel_attribute_operations,
    bench_otel_protobuf_conversion,
    bench_otel_memory_patterns,
    bench_otel_string_formatting,
    bench_otel_optimization_strategies
);
criterion_main!(benches);
