use criterion::{black_box, criterion_group, criterion_main, Criterion, BenchmarkId, Throughput};
use countersyncd::message::{
    saistats::{SAIStat, SAIStats, SAIStatsMessage, SAIStatsMessageExt},
    otel::{OtelMetrics, OtelMetricsMessageExt, OtelGauge, OtelDataPoint, OtelAttribute},
};
use opentelemetry_proto::tonic::{
    common::v1::{KeyValue as ProtoKeyValue, AnyValue, any_value::Value, InstrumentationScope},
    metrics::v1::{Metric, Gauge as ProtoGauge, ResourceMetrics, ScopeMetrics, NumberDataPoint},
    resource::v1::Resource as ProtoResource,
    collector::metrics::v1::ExportMetricsServiceRequest,
};
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::time::timeout;

/// Test patterns for OpenTelemetry export benchmarks
#[derive(Clone, Copy)]
enum ExportPattern {
    SmallBatch,      // Small number of metrics (10-50)
    MediumBatch,     // Medium batch size (100-500)  
    LargeBatch,      // Large batch size (1000-5000)
    MixedSizes,      // Variable metric sizes
    HighFrequency,   // Many small exports
    BulkExport,      // Few large exports
}

/// Helper function to create test SAI statistics for export benchmarks
fn create_test_sai_stats_for_export(stat_count: usize, pattern: ExportPattern) -> SAIStats {
    let stats = match pattern {
        ExportPattern::SmallBatch => (0..stat_count)
            .map(|i| SAIStat {
                object_name: format!("Eth{}", i),
                type_id: 1,
                stat_id: 1 + (i % 4) as u32,
                counter: 1000 + i as u64,
            })
            .collect(),
        ExportPattern::MediumBatch => (0..stat_count)
            .map(|i| SAIStat {
                object_name: format!("Ethernet{}", i / 4),
                type_id: 1 + (i % 3) as u32,
                stat_id: 1 + (i % 10) as u32,
                counter: 5000 + i as u64,
            })
            .collect(),
        ExportPattern::LargeBatch => (0..stat_count)
            .map(|i| SAIStat {
                object_name: format!("Interface{}", i / 8),
                type_id: 1 + (i % 5) as u32,
                stat_id: 1 + (i % 20) as u32,
                counter: 10000 + i as u64,
            })
            .collect(),
        ExportPattern::MixedSizes => (0..stat_count)
            .map(|i| {
                let name_size = match i % 4 {
                    0 => format!("E{}", i),
                    1 => format!("Ethernet{}", i),
                    2 => format!("BufferPool_{}", i),
                    _ => format!("VeryLongInterfaceNameForTesting_{}", i),
                };
                SAIStat {
                    object_name: name_size,
                    type_id: 1 + (i % 10) as u32,
                    stat_id: 1 + (i % 50) as u32,
                    counter: 1000 + i as u64,
                }
            })
            .collect(),
        ExportPattern::HighFrequency => (0..stat_count)
            .map(|i| SAIStat {
                object_name: format!("Port{}", i % 10), // Reuse port names
                type_id: 1,
                stat_id: 1 + (i % 4) as u32,
                counter: i as u64,
            })
            .collect(),
        ExportPattern::BulkExport => (0..stat_count)
            .map(|i| SAIStat {
                object_name: format!("BulkInterface_{}", i),
                type_id: 1 + (i % 20) as u32,
                stat_id: 1 + (i % 100) as u32,
                counter: 50000 + i as u64,
            })
            .collect(),
    };
    
    SAIStats::new(1672531200000000000u64, stats)
}

/// Benchmark protobuf conversion performance for OTel export
fn bench_protobuf_conversion(c: &mut Criterion) {
    let mut group = c.benchmark_group("protobuf_conversion");
    group.measurement_time(Duration::from_secs(8));
    
    for &size in &[10, 50, 100, 500, 1000, 5000] {
        let sai_stats = create_test_sai_stats_for_export(size, ExportPattern::MediumBatch);
        let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats);
        
        group.throughput(Throughput::Elements(size as u64));
        
        group.bench_with_input(
            BenchmarkId::new("data_points_to_proto", size),
            &otel_metrics,
            |b, metrics| {
                b.iter(|| {
                    let mut proto_data_points = Vec::new();
                    for gauge in &metrics.gauges {
                        for data_point in &gauge.data_points {
                            let proto = data_point.to_proto();
                            proto_data_points.push(black_box(proto));
                        }
                    }
                    black_box(proto_data_points)
                })
            }
        );
        
        group.bench_with_input(
            BenchmarkId::new("gauges_to_proto", size),
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
    }
    group.finish();
}

/// Benchmark full export request creation
fn bench_export_request_creation(c: &mut Criterion) {
    let mut group = c.benchmark_group("export_request_creation");
    
    // Pre-create reusable resource and instrumentation scope (like in OtelActor)
    let resource = ProtoResource {
        attributes: vec![ProtoKeyValue {
            key: "service.name".to_string(),
            value: Some(AnyValue {
                value: Some(Value::StringValue("countersyncd".to_string())),
            }),
        }],
        dropped_attributes_count: 0,
    };

    let instrumentation_scope = InstrumentationScope {
        name: "countersyncd".to_string(),
        version: "1.0".to_string(),
        attributes: vec![],
        dropped_attributes_count: 0,
    };
    
    for &size in &[10, 100, 1000, 5000] {
        let sai_stats = create_test_sai_stats_for_export(size, ExportPattern::MediumBatch);
        let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats);
        
        group.throughput(Throughput::Elements(size as u64));
        
        group.bench_with_input(
            BenchmarkId::new("full_export_request", size),
            &otel_metrics,
            |b, metrics| {
                b.iter(|| {
                    // Convert to protobuf metrics (matching OtelActor implementation)
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

                    // Create resource metrics
                    let resource_metrics = ResourceMetrics {
                        resource: Some(black_box(resource.clone())),
                        scope_metrics: vec![ScopeMetrics {
                            scope: Some(black_box(instrumentation_scope.clone())),
                            schema_url: String::new(),
                            metrics: proto_metrics,
                        }],
                        schema_url: String::new(),
                    };

                    // Create export request
                    let request = ExportMetricsServiceRequest {
                        resource_metrics: vec![resource_metrics],
                    };
                    
                    black_box(request)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark different export batch sizes and patterns  
fn bench_export_patterns(c: &mut Criterion) {
    let mut group = c.benchmark_group("export_patterns");
    group.measurement_time(Duration::from_secs(10));
    
    let patterns = [
        ("small_batch", ExportPattern::SmallBatch, 25),
        ("medium_batch", ExportPattern::MediumBatch, 250),
        ("large_batch", ExportPattern::LargeBatch, 2500),
        ("mixed_sizes", ExportPattern::MixedSizes, 500),
        ("high_frequency", ExportPattern::HighFrequency, 50),
        ("bulk_export", ExportPattern::BulkExport, 5000),
    ];
    
    for (pattern_name, pattern, size) in patterns.iter() {
        let sai_stats = create_test_sai_stats_for_export(*size, *pattern);
        let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats);
        
        group.throughput(Throughput::Elements(*size as u64));
        
        group.bench_with_input(
            BenchmarkId::new("pattern_conversion", pattern_name),
            &otel_metrics,
            |b, metrics| {
                b.iter(|| {
                    // Full conversion to export request
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
    }
    group.finish();
}

/// Benchmark attribute serialization performance
fn bench_attribute_serialization(c: &mut Criterion) {
    let mut group = c.benchmark_group("attribute_serialization");
    
    // Create test data with varying attribute counts
    for &attr_count in &[1, 3, 5, 10, 20] {
        for &metric_count in &[10, 100, 1000] {
            group.bench_with_input(
                BenchmarkId::new("attributes_to_proto", format!("{}attrs_{}metrics", attr_count, metric_count)),
                &(attr_count, metric_count),
                |b, &(attrs, metrics)| {
                    b.iter(|| {
                        let mut proto_attributes = Vec::new();
                        
                        for _ in 0..metrics {
                            for i in 0..attrs {
                                let attr = OtelAttribute::new(
                                    format!("key_{}", i),
                                    format!("value_{}", i)
                                );
                                let proto = attr.to_proto();
                                proto_attributes.push(black_box(proto));
                            }
                        }
                        
                        black_box(proto_attributes)
                    })
                }
            );
        }
    }
    
    group.finish();
}

/// Benchmark memory allocation patterns during export
fn bench_export_memory_patterns(c: &mut Criterion) {
    let mut group = c.benchmark_group("export_memory_patterns");
    
    for &size in &[100, 1000, 5000] {
        let sai_stats = create_test_sai_stats_for_export(size, ExportPattern::MediumBatch);
        let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats);
        
        // Pre-allocated vs dynamic allocation
        group.bench_with_input(
            BenchmarkId::new("pre_allocated_vectors", size),
            &otel_metrics,
            |b, metrics| {
                b.iter(|| {
                    let mut proto_metrics = Vec::with_capacity(black_box(metrics.gauges.len()));
                    
                    for gauge in &metrics.gauges {
                        let mut proto_data_points = Vec::with_capacity(gauge.data_points.len());
                        
                        for data_point in &gauge.data_points {
                            proto_data_points.push(data_point.to_proto());
                        }

                        let proto_gauge = ProtoGauge {
                            data_points: proto_data_points,
                        };

                        proto_metrics.push(Metric {
                            name: gauge.name.clone(),
                            description: gauge.description.clone(),
                            metadata: vec![],
                            data: Some(opentelemetry_proto::tonic::metrics::v1::metric::Data::Gauge(proto_gauge)),
                            ..Default::default()
                        });
                    }
                    
                    black_box(proto_metrics)
                })
            }
        );
        
        group.bench_with_input(
            BenchmarkId::new("dynamic_allocation", size),
            &otel_metrics,
            |b, metrics| {
                b.iter(|| {
                    let mut proto_metrics = Vec::new(); // No pre-allocation
                    
                    for gauge in &metrics.gauges {
                        let mut proto_data_points = Vec::new(); // No pre-allocation
                        
                        for data_point in &gauge.data_points {
                            proto_data_points.push(data_point.to_proto());
                        }

                        let proto_gauge = ProtoGauge {
                            data_points: proto_data_points,
                        };

                        proto_metrics.push(Metric {
                            name: gauge.name.clone(),
                            description: gauge.description.clone(),
                            metadata: vec![],
                            data: Some(opentelemetry_proto::tonic::metrics::v1::metric::Data::Gauge(proto_gauge)),
                            ..Default::default()
                        });
                    }
                    
                    black_box(proto_metrics)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark string operations during export (metric names, descriptions, attribute values)
fn bench_export_string_operations(c: &mut Criterion) {
    let mut group = c.benchmark_group("export_string_operations");
    
    let string_patterns = [
        ("short_names", ExportPattern::HighFrequency),
        ("medium_names", ExportPattern::MediumBatch),  
        ("long_names", ExportPattern::MixedSizes),
    ];
    
    for (pattern_name, pattern) in string_patterns.iter() {
        let sai_stats = create_test_sai_stats_for_export(1000, *pattern);
        let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats);
        
        group.bench_with_input(
            BenchmarkId::new("string_cloning", pattern_name),
            &otel_metrics,
            |b, metrics| {
                b.iter(|| {
                    let mut cloned_strings = Vec::new();
                    
                    for gauge in &metrics.gauges {
                        cloned_strings.push(black_box(gauge.name.clone()));
                        cloned_strings.push(black_box(gauge.description.clone()));
                        
                        for data_point in &gauge.data_points {
                            for attr in &data_point.attributes {
                                cloned_strings.push(black_box(attr.key.clone()));
                                cloned_strings.push(black_box(attr.value.clone()));
                            }
                        }
                    }
                    
                    black_box(cloned_strings)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark concurrent export scenarios
fn bench_concurrent_export_simulation(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let mut group = c.benchmark_group("concurrent_export_simulation");
    group.measurement_time(Duration::from_secs(8));
    
    for &worker_count in &[1, 2, 4, 8] {
        for &batch_size in &[100, 500] {
            group.bench_with_input(
                BenchmarkId::new("parallel_export_prep", format!("{}workers_{}batch", worker_count, batch_size)),
                &(worker_count, batch_size),
                |b, &(workers, batch)| {
                    b.to_async(&rt).iter(|| async {
                        let mut handles = Vec::new();
                        
                        for worker_id in 0..workers {
                            let sai_stats = create_test_sai_stats_for_export(
                                batch, 
                                ExportPattern::MediumBatch
                            );
                            
                            let handle = tokio::spawn(async move {
                                let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats);
                                
                                // Simulate export preparation
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
                                
                                black_box((worker_id, proto_metrics.len()))
                            });
                            
                            handles.push(handle);
                        }
                        
                        let mut total_metrics = 0;
                        for handle in handles {
                            let (_, count) = handle.await.unwrap();
                            total_metrics += count;
                        }
                        
                        black_box(total_metrics)
                    })
                }
            );
        }
    }
    group.finish();
}

/// Benchmark export throughput simulation (without actual network calls)
fn bench_export_throughput_simulation(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let mut group = c.benchmark_group("export_throughput_simulation");
    group.measurement_time(Duration::from_secs(10));
    
    for &metrics_per_second in &[100, 500, 1000, 5000] {
        for &export_interval_ms in &[100, 500, 1000] {
            let batches_per_second = 1000 / export_interval_ms;
            let metrics_per_batch = metrics_per_second / batches_per_second;
            
            if metrics_per_batch == 0 { continue; }
            
            group.throughput(Throughput::Elements(metrics_per_second as u64));
            
            group.bench_with_input(
                BenchmarkId::new(
                    "simulated_throughput", 
                    format!("{}mps_{}ms", metrics_per_second, export_interval_ms)
                ),
                &(metrics_per_batch, batches_per_second, export_interval_ms),
                |b, &(batch_size, batches, interval_ms)| {
                    b.to_async(&rt).iter(|| async {
                        let mut total_exported = 0;
                        let start_time = Instant::now();
                        
                        for batch_id in 0..batches {
                            let sai_stats = create_test_sai_stats_for_export(
                                batch_size, 
                                ExportPattern::HighFrequency
                            );
                            let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats);
                            
                            // Simulate export preparation (the expensive part)
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
                            
                            total_exported += proto_metrics.len();
                            
                            // Simulate interval timing
                            if batch_id < batches - 1 {
                                tokio::time::sleep(Duration::from_millis(interval_ms as u64)).await;
                            }
                        }
                        
                        let duration = start_time.elapsed();
                        black_box((total_exported, duration))
                    })
                }
            );
        }
    }
    group.finish();
}

/// Benchmark resource and scope reuse patterns (like in OtelActor)
fn bench_resource_reuse_patterns(c: &mut Criterion) {
    let mut group = c.benchmark_group("resource_reuse_patterns");
    
    // Pre-create resources like OtelActor does
    let resource = Arc::new(ProtoResource {
        attributes: vec![ProtoKeyValue {
            key: "service.name".to_string(),
            value: Some(AnyValue {
                value: Some(Value::StringValue("countersyncd".to_string())),
            }),
        }],
        dropped_attributes_count: 0,
    });

    let instrumentation_scope = Arc::new(InstrumentationScope {
        name: "countersyncd".to_string(),
        version: "1.0".to_string(),
        attributes: vec![],
        dropped_attributes_count: 0,
    });
    
    for &size in &[100, 1000, 5000] {
        let sai_stats = create_test_sai_stats_for_export(size, ExportPattern::MediumBatch);
        let otel_metrics = OtelMetrics::from_sai_stats(&sai_stats);
        
        // Reuse pattern (like OtelActor)
        group.bench_with_input(
            BenchmarkId::new("reuse_resources", size),
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

                    let resource_metrics = ResourceMetrics {
                        resource: Some(black_box((*resource).clone())), // Reuse
                        scope_metrics: vec![ScopeMetrics {
                            scope: Some(black_box((*instrumentation_scope).clone())), // Reuse
                            schema_url: String::new(),
                            metrics: proto_metrics,
                        }],
                        schema_url: String::new(),
                    };
                    
                    black_box(resource_metrics)
                })
            }
        );
        
        // Recreate pattern (less efficient)
        group.bench_with_input(
            BenchmarkId::new("recreate_resources", size),
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

                    // Recreate each time (inefficient)
                    let fresh_resource = ProtoResource {
                        attributes: vec![ProtoKeyValue {
                            key: "service.name".to_string(),
                            value: Some(AnyValue {
                                value: Some(Value::StringValue("countersyncd".to_string())),
                            }),
                        }],
                        dropped_attributes_count: 0,
                    };

                    let fresh_scope = InstrumentationScope {
                        name: "countersyncd".to_string(),
                        version: "1.0".to_string(),
                        attributes: vec![],
                        dropped_attributes_count: 0,
                    };

                    let resource_metrics = ResourceMetrics {
                        resource: Some(black_box(fresh_resource)),
                        scope_metrics: vec![ScopeMetrics {
                            scope: Some(black_box(fresh_scope)),
                            schema_url: String::new(),
                            metrics: proto_metrics,
                        }],
                        schema_url: String::new(),
                    };
                    
                    black_box(resource_metrics)
                })
            }
        );
    }
    group.finish();
}

criterion_group!(
    benches,
    bench_protobuf_conversion,
    bench_export_request_creation,
    bench_export_patterns,
    bench_attribute_serialization,
    bench_export_memory_patterns,
    bench_export_string_operations,
    bench_concurrent_export_simulation,
    bench_export_throughput_simulation,
    bench_resource_reuse_patterns
);
criterion_main!(benches);
