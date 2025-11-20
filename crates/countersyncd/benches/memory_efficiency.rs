use criterion::{black_box, criterion_group, criterion_main, Criterion, BenchmarkId, Throughput};
use countersyncd::message::{
    saistats::{SAIStat, SAIStats, SAIStatsMessage, SAIStatsMessageExt},
    otel::{OtelMetrics, OtelMetricsMessageExt, OtelGauge, OtelDataPoint, OtelAttribute},
};
use std::sync::Arc;
use std::collections::{HashMap, BTreeMap};
use std::time::Duration;
use tokio::sync::mpsc;

/// Memory allocation patterns for testing
#[derive(Clone, Copy)]
enum AllocationPattern {
    PreAllocation,    // Pre-allocate with known capacity
    DynamicGrowth,    // Start small and grow dynamically
    Chunked,          // Allocate in chunks
    Pooled,           // Simulated object pooling
}

/// Data size patterns for memory testing
#[derive(Clone, Copy)]
enum DataSizePattern {
    Small,      // Small objects (< 1KB each)
    Medium,     // Medium objects (1-10KB each)
    Large,      // Large objects (10-100KB each)
    Variable,   // Variable size objects
    Realistic,  // Realistic SAI statistics sizes
}

/// Helper function to create test SAI stats with specific memory characteristics
fn create_memory_test_sai_stats(
    count: usize,
    data_pattern: DataSizePattern,
    allocation_pattern: AllocationPattern,
) -> SAIStats {
    let stats = match allocation_pattern {
        AllocationPattern::PreAllocation => {
            let mut stats = Vec::with_capacity(count);
            for i in 0..count {
                stats.push(create_sai_stat_by_pattern(i, data_pattern));
            }
            stats
        }
        AllocationPattern::DynamicGrowth => {
            let mut stats = Vec::new();
            for i in 0..count {
                stats.push(create_sai_stat_by_pattern(i, data_pattern));
            }
            stats
        }
        AllocationPattern::Chunked => {
            let chunk_size = 100;
            let mut stats = Vec::new();
            for chunk_start in (0..count).step_by(chunk_size) {
                let chunk_end = (chunk_start + chunk_size).min(count);
                let mut chunk = Vec::with_capacity(chunk_size);
                for i in chunk_start..chunk_end {
                    chunk.push(create_sai_stat_by_pattern(i, data_pattern));
                }
                stats.extend(chunk);
            }
            stats
        }
        AllocationPattern::Pooled => {
            // Simulate object pooling by reusing object name strings
            let pool_size = 10;
            let object_names: Vec<String> = (0..pool_size)
                .map(|i| format!("PooledObject{}", i))
                .collect();
            
            let mut stats = Vec::with_capacity(count);
            for i in 0..count {
                let pooled_name = object_names[i % pool_size].clone();
                stats.push(SAIStat {
                    object_name: pooled_name,
                    type_id: 1 + (i % 10) as u32,
                    stat_id: 1 + (i % 20) as u32,
                    counter: 1000 + i as u64,
                });
            }
            stats
        }
    };
    
    SAIStats::new(1672531200000000000u64, stats)
}

fn create_sai_stat_by_pattern(index: usize, pattern: DataSizePattern) -> SAIStat {
    let object_name = match pattern {
        DataSizePattern::Small => format!("E{}", index),
        DataSizePattern::Medium => format!("Ethernet_Interface_{}", index),
        DataSizePattern::Large => format!(
            "VeryLongInterfaceNameWithExtensiveDescriptiveInformation_{}_{}_{}_{}_End",
            index, index * 2, index * 3, index * 4
        ),
        DataSizePattern::Variable => {
            match index % 4 {
                0 => format!("S{}", index),
                1 => format!("Medium_Interface_{}", index),
                2 => format!("VeryLongInterfaceNameForTesting_{}", index),
                _ => format!("ExtremelylongInterfaceNameWithManyCharactersAndDescriptiveInformation_{}_Extended", index),
            }
        }
        DataSizePattern::Realistic => {
            match index % 6 {
                0 => format!("Ethernet{}", index / 6),
                1 => format!("BufferPool{}", index / 6),
                2 => format!("Queue_Ethernet{}_{}", index / 6, index % 8),
                3 => format!("Port_Channel{}", index / 6),
                4 => format!("Vlan{}", 100 + (index / 6)),
                _ => format!("Tunnel_Interface_{}", index / 6),
            }
        }
    };
    
    SAIStat {
        object_name,
        type_id: 1 + (index % 10) as u32,
        stat_id: 1 + (index % 50) as u32,
        counter: 1000 + index as u64,
    }
}

/// Benchmark different vector allocation patterns
fn bench_vector_allocation_patterns(c: &mut Criterion) {
    let mut group = c.benchmark_group("vector_allocation_patterns");
    group.measurement_time(Duration::from_secs(8));
    
    let patterns = [
        ("pre_allocation", AllocationPattern::PreAllocation),
        ("dynamic_growth", AllocationPattern::DynamicGrowth),
        ("chunked", AllocationPattern::Chunked),
        ("pooled", AllocationPattern::Pooled),
    ];
    
    for &size in &[100, 1000, 10000] {
        for (pattern_name, pattern) in patterns.iter() {
            group.throughput(Throughput::Elements(size as u64));
            
            group.bench_with_input(
                BenchmarkId::new("allocation_pattern", format!("{}_{}", pattern_name, size)),
                &(size, *pattern),
                |b, &(count, alloc_pattern)| {
                    b.iter(|| {
                        let sai_stats = create_memory_test_sai_stats(
                            count, 
                            DataSizePattern::Realistic, 
                            alloc_pattern
                        );
                        black_box(sai_stats)
                    })
                }
            );
        }
    }
    group.finish();
}

/// Benchmark Arc vs Clone memory patterns
fn bench_arc_vs_clone_patterns(c: &mut Criterion) {
    let mut group = c.benchmark_group("arc_vs_clone_patterns");
    
    for &size in &[100, 1000, 5000] {
        let sai_stats = create_memory_test_sai_stats(
            size, 
            DataSizePattern::Realistic, 
            AllocationPattern::PreAllocation
        );
        let sai_message = Arc::new(sai_stats.clone());
        
        // Arc cloning (cheap)
        group.bench_with_input(
            BenchmarkId::new("arc_clone", size),
            &sai_message,
            |b, message| {
                b.iter(|| {
                    let mut clones = Vec::with_capacity(10);
                    for _ in 0..10 {
                        clones.push(black_box(message.clone())); // Arc clone
                    }
                    black_box(clones)
                })
            }
        );
        
        // Full struct cloning (expensive)
        group.bench_with_input(
            BenchmarkId::new("full_clone", size),
            &sai_stats,
            |b, stats| {
                b.iter(|| {
                    let mut clones = Vec::with_capacity(10);
                    for _ in 0..10 {
                        clones.push(black_box(stats.clone())); // Full clone
                    }
                    black_box(clones)
                })
            }
        );
        
        // Arc unwrap and re-wrap pattern
        group.bench_with_input(
            BenchmarkId::new("arc_unwrap_rewrap", size),
            &sai_message,
            |b, message| {
                b.iter(|| {
                    let mut processed = Vec::with_capacity(5);
                    for _ in 0..5 {
                        let cloned_arc = message.clone();
                        // Simulate processing that requires owned data
                        match Arc::try_unwrap(cloned_arc) {
                            Ok(owned) => {
                                let new_arc = Arc::new(owned);
                                processed.push(black_box(new_arc));
                            }
                            Err(arc) => {
                                // Fallback to full clone if multiple references exist
                                let owned = (*arc).clone();
                                let new_arc = Arc::new(owned);
                                processed.push(black_box(new_arc));
                            }
                        }
                    }
                    black_box(processed)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark string allocation and reuse patterns
fn bench_string_allocation_patterns(c: &mut Criterion) {
    let mut group = c.benchmark_group("string_allocation_patterns");
    
    let size_patterns = [
        ("small_strings", DataSizePattern::Small),
        ("medium_strings", DataSizePattern::Medium),
        ("large_strings", DataSizePattern::Large),
        ("variable_strings", DataSizePattern::Variable),
    ];
    
    for (pattern_name, pattern) in size_patterns.iter() {
        for &count in &[100, 1000, 5000] {
            group.bench_with_input(
                BenchmarkId::new("string_creation", format!("{}_{}", pattern_name, count)),
                &(count, *pattern),
                |b, &(size, data_pattern)| {
                    b.iter(|| {
                        let mut strings = Vec::with_capacity(size);
                        for i in 0..size {
                            let stat = create_sai_stat_by_pattern(i, data_pattern);
                            strings.push(black_box(stat.object_name));
                        }
                        black_box(strings)
                    })
                }
            );
            
            // String interning simulation
            group.bench_with_input(
                BenchmarkId::new("string_interning_sim", format!("{}_{}", pattern_name, count)),
                &(count, *pattern),
                |b, &(size, data_pattern)| {
                    b.iter(|| {
                        let mut string_pool = HashMap::new();
                        let mut stats = Vec::with_capacity(size);
                        
                        for i in 0..size {
                            let temp_stat = create_sai_stat_by_pattern(i, data_pattern);
                            let interned_name = string_pool
                                .entry(temp_stat.object_name.clone())
                                .or_insert_with(|| temp_stat.object_name.clone())
                                .clone();
                            
                            stats.push(SAIStat {
                                object_name: interned_name,
                                type_id: temp_stat.type_id,
                                stat_id: temp_stat.stat_id,
                                counter: temp_stat.counter,
                            });
                        }
                        black_box(stats)
                    })
                }
            );
        }
    }
    group.finish();
}

/// Benchmark collection type efficiency for different use cases
fn bench_collection_efficiency(c: &mut Criterion) {
    let mut group = c.benchmark_group("collection_efficiency");
    
    for &size in &[100, 1000, 10000] {
        let test_data: Vec<(String, u64)> = (0..size)
            .map(|i| (format!("Object{}", i), i as u64))
            .collect();
        
        // HashMap vs BTreeMap for lookups
        group.bench_with_input(
            BenchmarkId::new("hashmap_creation", size),
            &test_data,
            |b, data| {
                b.iter(|| {
                    let mut map = HashMap::with_capacity(data.len());
                    for (key, value) in data {
                        map.insert(black_box(key.clone()), black_box(*value));
                    }
                    black_box(map)
                })
            }
        );
        
        group.bench_with_input(
            BenchmarkId::new("btreemap_creation", size),
            &test_data,
            |b, data| {
                b.iter(|| {
                    let mut map = BTreeMap::new();
                    for (key, value) in data {
                        map.insert(black_box(key.clone()), black_box(*value));
                    }
                    black_box(map)
                })
            }
        );
        
        // Lookup performance
        let hashmap: HashMap<String, u64> = test_data.iter().cloned().collect();
        let btreemap: BTreeMap<String, u64> = test_data.iter().cloned().collect();
        
        group.bench_with_input(
            BenchmarkId::new("hashmap_lookup", size),
            &hashmap,
            |b, map| {
                b.iter(|| {
                    let mut found = 0;
                    for i in 0..100.min(size) {
                        let key = format!("Object{}", i);
                        if map.get(&key).is_some() {
                            found += 1;
                        }
                    }
                    black_box(found)
                })
            }
        );
        
        group.bench_with_input(
            BenchmarkId::new("btreemap_lookup", size),
            &btreemap,
            |b, map| {
                b.iter(|| {
                    let mut found = 0;
                    for i in 0..100.min(size) {
                        let key = format!("Object{}", i);
                        if map.get(&key).is_some() {
                            found += 1;
                        }
                    }
                    black_box(found)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark memory usage during SAI to OTel conversion
fn bench_conversion_memory_patterns(c: &mut Criterion) {
    let mut group = c.benchmark_group("conversion_memory_patterns");
    
    for &size in &[100, 1000, 5000] {
        let sai_stats = create_memory_test_sai_stats(
            size, 
            DataSizePattern::Realistic, 
            AllocationPattern::PreAllocation
        );
        
        // Standard conversion
        group.bench_with_input(
            BenchmarkId::new("standard_conversion", size),
            &sai_stats,
            |b, stats| {
                b.iter(|| {
                    let otel_metrics = OtelMetrics::from_sai_stats(black_box(stats));
                    black_box(otel_metrics)
                })
            }
        );
        
        // Pre-allocated conversion simulation
        group.bench_with_input(
            BenchmarkId::new("prealloc_conversion", size),
            &sai_stats,
            |b, stats| {
                b.iter(|| {
                    let mut gauges = Vec::with_capacity(black_box(stats.stats.len()));
                    
                    for stat in &stats.stats {
                        let attributes = Vec::with_capacity(3); // We know we need 3 attributes
                        let mut attrs = attributes;
                        attrs.push(OtelAttribute::new("object_name", &stat.object_name));
                        attrs.push(OtelAttribute::new("sai_type_id", stat.type_id.to_string()));
                        attrs.push(OtelAttribute::new("sai_stat_id", stat.stat_id.to_string()));
                        
                        let data_point = OtelDataPoint {
                            attributes: attrs,
                            time_unix_nano: stats.observation_time,
                            value: stat.counter as i64,
                        };
                        
                        let gauge = OtelGauge {
                            name: format!("sai_counter_type_{}_stat_{}", stat.type_id, stat.stat_id),
                            description: format!("SAI counter for object {} (type:{}, stat:{})", 
                                               stat.object_name, stat.type_id, stat.stat_id),
                            unit: "1".to_string(),
                            data_points: vec![data_point],
                        };
                        
                        gauges.push(gauge);
                    }
                    
                    let otel_metrics = OtelMetrics {
                        service_name: "countersyncd".to_string(),
                        scope_name: "countersyncd".to_string(),
                        scope_version: "1.0".to_string(),
                        gauges,
                    };
                    
                    black_box(otel_metrics)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark memory usage during channel operations
fn bench_channel_memory_patterns(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let mut group = c.benchmark_group("channel_memory_patterns");
    group.measurement_time(Duration::from_secs(8));
    
    for &buffer_size in &[10, 100, 1000] {
        for &message_size in &[100, 1000] {
            group.bench_with_input(
                BenchmarkId::new("channel_throughput", format!("buf{}_msg{}", buffer_size, message_size)),
                &(buffer_size, message_size),
                |b, &(buf_size, msg_size)| {
                    b.to_async(&rt).iter(|| async {
                        let (tx, mut rx) = mpsc::channel::<SAIStatsMessage>(buf_size);
                        
                        // Producer task
                        let producer_handle = tokio::spawn(async move {
                            for i in 0..100 {
                                let sai_stats = create_memory_test_sai_stats(
                                    msg_size,
                                    DataSizePattern::Realistic,
                                    AllocationPattern::PreAllocation,
                                );
                                let message = Arc::new(sai_stats);
                                
                                if tx.send(black_box(message)).await.is_err() {
                                    break;
                                }
                            }
                        });
                        
                        // Consumer
                        let mut received = 0;
                        while let Some(msg) = rx.recv().await {
                            black_box(msg);
                            received += 1;
                            if received >= 100 {
                                break;
                            }
                        }
                        
                        producer_handle.await.unwrap();
                        black_box(received)
                    })
                }
            );
        }
    }
    group.finish();
}

/// Benchmark object pooling patterns
fn bench_object_pooling_patterns(c: &mut Criterion) {
    let mut group = c.benchmark_group("object_pooling_patterns");
    
    // Simulate object pooling for SAIStats
    struct SAIStatsPool {
        pool: Vec<SAIStats>,
        capacity: usize,
    }
    
    impl SAIStatsPool {
        fn new(capacity: usize) -> Self {
            let pool = (0..capacity)
                .map(|_| SAIStats::new(0, Vec::new()))
                .collect();
            
            Self { pool, capacity }
        }
        
        fn get(&mut self) -> Option<SAIStats> {
            self.pool.pop()
        }
        
        fn return_object(&mut self, mut obj: SAIStats) {
            if self.pool.len() < self.capacity {
                obj.stats.clear(); // Reset for reuse
                obj.observation_time = 0;
                self.pool.push(obj);
            }
        }
    }
    
    for &pool_size in &[10, 50, 100] {
        for &usage_count in &[100, 1000] {
            group.bench_with_input(
                BenchmarkId::new("object_pooling", format!("pool{}_use{}", pool_size, usage_count)),
                &(pool_size, usage_count),
                |b, &(pool_cap, usage)| {
                    b.iter(|| {
                        let mut pool = SAIStatsPool::new(pool_cap);
                        let mut used_objects = Vec::new();
                        
                        // Use pool objects
                        for i in 0..usage {
                            if let Some(mut obj) = pool.get() {
                                // Simulate using the object
                                obj.observation_time = i as u64;
                                obj.stats.push(SAIStat {
                                    object_name: format!("Pooled{}", i),
                                    type_id: 1,
                                    stat_id: 1,
                                    counter: i as u64,
                                });
                                used_objects.push(obj);
                            } else {
                                // Pool exhausted, create new
                                let new_obj = SAIStats::new(
                                    i as u64, 
                                    vec![SAIStat {
                                        object_name: format!("New{}", i),
                                        type_id: 1,
                                        stat_id: 1,
                                        counter: i as u64,
                                    }]
                                );
                                used_objects.push(new_obj);
                            }
                        }
                        
                        // Return objects to pool
                        for obj in used_objects {
                            pool.return_object(obj);
                        }
                        
                        black_box(pool)
                    })
                }
            );
            
            // Compare with no pooling
            group.bench_with_input(
                BenchmarkId::new("no_pooling", format!("use{}", usage_count)),
                &usage_count,
                |b, &usage| {
                    b.iter(|| {
                        let mut objects = Vec::new();
                        
                        for i in 0..usage {
                            let obj = SAIStats::new(
                                i as u64, 
                                vec![SAIStat {
                                    object_name: format!("Fresh{}", i),
                                    type_id: 1,
                                    stat_id: 1,
                                    counter: i as u64,
                                }]
                            );
                            objects.push(obj);
                        }
                        
                        black_box(objects)
                    })
                }
            );
        }
    }
    group.finish();
}

/// Benchmark memory fragmentation patterns
fn bench_memory_fragmentation_patterns(c: &mut Criterion) {
    let mut group = c.benchmark_group("memory_fragmentation_patterns");
    
    for &size in &[1000, 5000] {
        // Simulate fragmented allocation pattern
        group.bench_with_input(
            BenchmarkId::new("fragmented_allocation", size),
            &size,
            |b, &count| {
                b.iter(|| {
                    let mut collections = Vec::new();
                    
                    // Create many small collections (fragmented)
                    for i in 0..count / 10 {
                        let small_stats = create_memory_test_sai_stats(
                            10,
                            DataSizePattern::Variable,
                            AllocationPattern::DynamicGrowth,
                        );
                        collections.push(small_stats);
                    }
                    
                    black_box(collections)
                })
            }
        );
        
        // Compare with contiguous allocation
        group.bench_with_input(
            BenchmarkId::new("contiguous_allocation", size),
            &size,
            |b, &count| {
                b.iter(|| {
                    let large_stats = create_memory_test_sai_stats(
                        count,
                        DataSizePattern::Variable,
                        AllocationPattern::PreAllocation,
                    );
                    
                    black_box(large_stats)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark memory usage with different data structures
fn bench_data_structure_memory_efficiency(c: &mut Criterion) {
    let mut group = c.benchmark_group("data_structure_memory_efficiency");
    
    for &size in &[100, 1000, 5000] {
        let test_stats = create_memory_test_sai_stats(
            size,
            DataSizePattern::Realistic,
            AllocationPattern::PreAllocation,
        );
        
        // Vec storage (current approach)
        group.bench_with_input(
            BenchmarkId::new("vec_storage", size),
            &test_stats,
            |b, stats| {
                b.iter(|| {
                    let vec_copy = black_box(stats.stats.clone());
                    // Simulate processing
                    let mut sum = 0u64;
                    for stat in &vec_copy {
                        sum += stat.counter;
                    }
                    black_box(sum)
                })
            }
        );
        
        // HashMap storage (alternative)
        group.bench_with_input(
            BenchmarkId::new("hashmap_storage", size),
            &test_stats,
            |b, stats| {
                b.iter(|| {
                    let mut map = HashMap::with_capacity(stats.stats.len());
                    for (i, stat) in stats.stats.iter().enumerate() {
                        map.insert(i, stat.clone());
                    }
                    
                    // Simulate processing
                    let mut sum = 0u64;
                    for stat in map.values() {
                        sum += stat.counter;
                    }
                    black_box(sum)
                })
            }
        );
        
        // Boxed storage (heap allocation)
        group.bench_with_input(
            BenchmarkId::new("boxed_storage", size),
            &test_stats,
            |b, stats| {
                b.iter(|| {
                    let boxed_stats: Vec<Box<SAIStat>> = stats.stats.iter()
                        .map(|s| Box::new(s.clone()))
                        .collect();
                    
                    // Simulate processing
                    let mut sum = 0u64;
                    for stat in &boxed_stats {
                        sum += stat.counter;
                    }
                    black_box(sum)
                })
            }
        );
    }
    group.finish();
}

criterion_group!(
    benches,
    bench_vector_allocation_patterns,
    bench_arc_vs_clone_patterns,
    bench_string_allocation_patterns,
    bench_collection_efficiency,
    bench_conversion_memory_patterns,
    bench_channel_memory_patterns,
    bench_object_pooling_patterns,
    bench_memory_fragmentation_patterns,
    bench_data_structure_memory_efficiency
);
criterion_main!(benches);
