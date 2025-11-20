use criterion::{black_box, criterion_group, criterion_main, Criterion, BenchmarkId, Throughput};
use countersyncd::message::{
    saistats::{SAIStat, SAIStats, SAIStatsMessage},
    ipfix::IPFixTemplatesMessage,
    buffer::SocketBufferMessage,
};
use ipfixrw::parser::{DataRecordValue, FieldSpecifier};
use byteorder::{ByteOrder, NetworkEndian};
use std::sync::Arc;
use std::time::Duration;

/// Test data patterns for IPFIX parsing benchmarks
#[derive(Clone, Copy)]
enum IpfixDataPattern {
    SimplePort,      // Basic port statistics
    ComplexMixed,    // Mixed object types with various field sizes
    LargeObjects,    // Large object names and many fields
    TimeVariants,    // Different time field formats
    ExtendedIds,     // SAI IDs with extension flags
}

/// Helper function to create test field specifiers
fn create_field_spec(element_id: u16, enterprise_number: Option<u32>) -> FieldSpecifier {
    FieldSpecifier::new(enterprise_number, element_id, 8)
}

/// Helper function to create byte data record values
fn create_byte_value(value: u64, size: usize) -> DataRecordValue {
    let mut bytes = vec![0u8; size];
    match size {
        1 => bytes[0] = value as u8,
        2 => NetworkEndian::write_u16(&mut bytes, value as u16),
        4 => NetworkEndian::write_u32(&mut bytes, value as u32),
        8 => NetworkEndian::write_u64(&mut bytes, value),
        _ => NetworkEndian::write_u64(&mut bytes[..8], value), // Truncate if too large
    }
    DataRecordValue::Bytes(bytes)
}

/// Create test object names of various sizes
fn create_test_object_names(count: usize, pattern: IpfixDataPattern) -> Vec<String> {
    match pattern {
        IpfixDataPattern::SimplePort => (0..count)
            .map(|i| format!("Ethernet{}", i))
            .collect(),
        IpfixDataPattern::ComplexMixed => (0..count)
            .map(|i| match i % 3 {
                0 => format!("Ethernet{}", i / 3),
                1 => format!("BufferPool{}", i / 3),
                _ => format!("Queue_Ethernet{}_{}", i / 3, i % 8),
            })
            .collect(),
        IpfixDataPattern::LargeObjects => (0..count)
            .map(|i| format!("VeryLongObjectNameForBenchmarkTesting_Interface_{}_With_Extended_Naming_Convention", i))
            .collect(),
        IpfixDataPattern::TimeVariants => (0..count)
            .map(|i| format!("TimeSource{}", i))
            .collect(),
        IpfixDataPattern::ExtendedIds => (0..count)
            .map(|i| format!("ExtendedObject{}", i))
            .collect(),
    }
}

/// Create test IPFIX field specifiers and values for benchmarking
fn create_test_ipfix_data(
    count: usize, 
    pattern: IpfixDataPattern
) -> Vec<(FieldSpecifier, DataRecordValue, Vec<String>)> {
    let object_names = create_test_object_names(count, pattern);
    let mut test_data = Vec::new();

    for i in 0..count {
        let (field_spec, value) = match pattern {
            IpfixDataPattern::SimplePort => {
                // Basic port statistics
                let enterprise = 0x00010000 | (1 + (i % 4) as u32); // type_id=1, stat_id varies
                let spec = create_field_spec(1 + (i % 10) as u16, Some(enterprise));
                let val = create_byte_value(1000 + i as u64, 8);
                (spec, val)
            }
            IpfixDataPattern::ComplexMixed => {
                // Mixed object types
                let type_id = match i % 3 {
                    0 => 1,   // Port
                    1 => 24,  // Buffer pool
                    _ => 5,   // Queue
                };
                let stat_id = 1 + (i % 10) as u32;
                let enterprise = (type_id << 16) | stat_id;
                let spec = create_field_spec(1 + (i % 100) as u16, Some(enterprise));
                let val = create_byte_value(5000 + i as u64, 8);
                (spec, val)
            }
            IpfixDataPattern::LargeObjects => {
                // Large object names with many fields
                let enterprise = 0x00640000 | (i % 1000) as u32; // type_id=100, stat_id varies
                let spec = create_field_spec(1 + (i % 200) as u16, Some(enterprise));
                let val = create_byte_value(10000 + i as u64, 8);
                (spec, val)
            }
            IpfixDataPattern::TimeVariants => {
                // Different time field sizes and formats
                let enterprise = 0x00010000 | (322 + (i % 4) as u32); // Time-related fields
                let spec = create_field_spec(322 + (i % 4) as u16, Some(enterprise));
                let size = match i % 3 {
                    0 => 4,  // 32-bit seconds
                    1 => 8,  // 64-bit nanoseconds
                    _ => 8,  // Default 64-bit
                };
                let val = create_byte_value(1672531200000000000u64 + i as u64, size);
                (spec, val)
            }
            IpfixDataPattern::ExtendedIds => {
                // SAI IDs with extension flags
                let base_type = 100 + (i % 50) as u32;
                let base_stat = 200 + (i % 100) as u32;
                let enterprise = 0x80008000 | (base_type << 16) | base_stat; // Both extension flags
                let spec = create_field_spec(1 + (i % 50) as u16, Some(enterprise));
                let val = create_byte_value(50000 + i as u64, 8);
                (spec, val)
            }
        };

        test_data.push((field_spec, value, object_names.clone()));
    }

    test_data
}

/// Benchmark individual SAIStat creation from IPFIX field specifier and value
fn bench_sai_stat_from_ipfix(c: &mut Criterion) {
    let mut group = c.benchmark_group("sai_stat_from_ipfix");
    
    let patterns = [
        ("simple_port", IpfixDataPattern::SimplePort),
        ("complex_mixed", IpfixDataPattern::ComplexMixed),
        ("large_objects", IpfixDataPattern::LargeObjects),
        ("time_variants", IpfixDataPattern::TimeVariants),
        ("extended_ids", IpfixDataPattern::ExtendedIds),
    ];

    for (pattern_name, pattern) in patterns.iter() {
        for &count in &[10, 100, 1000] {
            let test_data = create_test_ipfix_data(count, *pattern);
            
            group.throughput(Throughput::Elements(count as u64));
            
            group.bench_with_input(
                BenchmarkId::new("single_conversion", format!("{}_{}", pattern_name, count)),
                &test_data,
                |b, data| {
                    b.iter(|| {
                        let mut stats = Vec::with_capacity(data.len());
                        for (field_spec, value, object_names) in data {
                            let stat = SAIStat::from_ipfix(
                                black_box(field_spec),
                                black_box(value),
                                black_box(object_names)
                            );
                            stats.push(black_box(stat));
                        }
                        black_box(stats)
                    })
                }
            );
        }
    }
    group.finish();
}

/// Benchmark batch IPFIX data parsing performance
fn bench_batch_ipfix_parsing(c: &mut Criterion) {
    let mut group = c.benchmark_group("batch_ipfix_parsing");
    group.measurement_time(Duration::from_secs(10));
    
    for &batch_size in &[50, 100, 500, 1000, 5000] {
        let test_data = create_test_ipfix_data(batch_size, IpfixDataPattern::ComplexMixed);
        
        group.throughput(Throughput::Elements(batch_size as u64));
        
        group.bench_with_input(
            BenchmarkId::new("batch_processing", batch_size),
            &test_data,
            |b, data| {
                b.iter(|| {
                    let observation_time = 1672531200000000000u64;
                    let mut all_stats = Vec::with_capacity(data.len());
                    
                    // Simulate batch processing
                    for (field_spec, value, object_names) in data {
                        let stat = SAIStat::from_ipfix(
                            black_box(field_spec),
                            black_box(value),
                            black_box(object_names)
                        );
                        all_stats.push(stat);
                    }
                    
                    let sai_stats = SAIStats::new(observation_time, all_stats);
                    black_box(sai_stats)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark different field size parsing performance
fn bench_field_size_parsing(c: &mut Criterion) {
    let mut group = c.benchmark_group("field_size_parsing");
    
    let field_sizes = [1, 2, 4, 8, 16]; // Different byte field sizes
    let object_names = create_test_object_names(100, IpfixDataPattern::SimplePort);
    
    for &size in &field_sizes {
        group.bench_with_input(
            BenchmarkId::new("parse_field_size", size),
            &size,
            |b, &field_size| {
                b.iter(|| {
                    let mut stats = Vec::new();
                    for i in 0..1000 {
                        let field_spec = create_field_spec(1 + (i % 10), Some(0x00010001));
                        let value = create_byte_value(1000 + i, field_size);
                        
                        let stat = SAIStat::from_ipfix(
                            black_box(&field_spec),
                            black_box(&value),
                            black_box(&object_names)
                        );
                        stats.push(black_box(stat));
                    }
                    black_box(stats)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark enterprise number parsing with extension flags
fn bench_enterprise_number_parsing(c: &mut Criterion) {
    let mut group = c.benchmark_group("enterprise_number_parsing");
    
    let test_cases = [
        ("no_extensions", 0x12340567),
        ("type_extension", 0x92340567),    // Type extension flag set
        ("stat_extension", 0x12348567),    // Stat extension flag set  
        ("both_extensions", 0x92348567),   // Both extension flags set
    ];
    
    let object_names = create_test_object_names(100, IpfixDataPattern::SimplePort);
    
    for (case_name, enterprise_number) in test_cases.iter() {
        group.bench_with_input(
            BenchmarkId::new("extension_parsing", case_name),
            enterprise_number,
            |b, &enterprise| {
                b.iter(|| {
                    let mut stats = Vec::new();
                    for i in 0..1000 {
                        let field_spec = create_field_spec(1 + (i % 10), Some(enterprise));
                        let value = create_byte_value(1000 + i, 8);
                        
                        let stat = SAIStat::from_ipfix(
                            black_box(&field_spec),
                            black_box(&value),
                            black_box(&object_names)
                        );
                        stats.push(black_box(stat));
                    }
                    black_box(stats)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark object name resolution performance
fn bench_object_name_resolution(c: &mut Criterion) {
    let mut group = c.benchmark_group("object_name_resolution");
    
    for &name_count in &[10, 100, 1000, 10000] {
        let object_names = create_test_object_names(name_count, IpfixDataPattern::LargeObjects);
        
        group.bench_with_input(
            BenchmarkId::new("name_lookup", name_count),
            &object_names,
            |b, names| {
                b.iter(|| {
                    let mut stats = Vec::new();
                    for i in 0..1000 {
                        let label = 1 + (i % (names.len().min(1000))) as u16; // Valid label range
                        let field_spec = create_field_spec(label, Some(0x00010001));
                        let value = create_byte_value(1000 + i, 8);
                        
                        let stat = SAIStat::from_ipfix(
                            black_box(&field_spec),
                            black_box(&value),
                            black_box(names)
                        );
                        stats.push(black_box(stat));
                    }
                    black_box(stats)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark time field extraction performance
fn bench_time_field_extraction(c: &mut Criterion) {
    let mut group = c.benchmark_group("time_field_extraction");
    
    let time_formats = [
        ("nanoseconds_64", 325, 8, 1672531200000000000u64),
        ("seconds_32", 322, 4, 1672531200u64),
        ("milliseconds_64", 323, 8, 1672531200000u64),
    ];
    
    let object_names = create_test_object_names(100, IpfixDataPattern::TimeVariants);
    
    for (format_name, field_id, size, time_value) in time_formats.iter() {
        group.bench_with_input(
            BenchmarkId::new("time_extraction", format_name),
            &(*field_id, *size, *time_value),
            |b, &(field_id, field_size, time_val)| {
                b.iter(|| {
                    let mut extraction_count = 0;
                    for i in 0..1000 {
                        let field_spec = create_field_spec(field_id, None); // Standard field
                        let value = create_byte_value(time_val + i, field_size);
                        
                        // Simulate time field recognition and extraction
                        let is_time_field = field_spec.enterprise_number.is_none() && 
                                          (field_spec.information_element_identifier == 322 ||
                                           field_spec.information_element_identifier == 325);
                        
                        if is_time_field {
                            extraction_count += 1;
                            black_box(&value);
                        }
                    }
                    black_box(extraction_count)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark memory allocation patterns during IPFIX parsing
fn bench_ipfix_memory_patterns(c: &mut Criterion) {
    let mut group = c.benchmark_group("ipfix_memory_patterns");
    
    for &record_count in &[100, 1000, 10000] {
        let test_data = create_test_ipfix_data(record_count, IpfixDataPattern::ComplexMixed);
        
        // Pre-allocated vs dynamic allocation
        group.bench_with_input(
            BenchmarkId::new("pre_allocated", record_count),
            &test_data,
            |b, data| {
                b.iter(|| {
                    let mut stats = Vec::with_capacity(black_box(data.len())); // Pre-allocated
                    for (field_spec, value, object_names) in data {
                        let stat = SAIStat::from_ipfix(
                            black_box(field_spec),
                            black_box(value),
                            black_box(object_names)
                        );
                        stats.push(stat);
                    }
                    black_box(stats)
                })
            }
        );
        
        group.bench_with_input(
            BenchmarkId::new("dynamic_allocation", record_count),
            &test_data,
            |b, data| {
                b.iter(|| {
                    let mut stats = Vec::new(); // Dynamic allocation
                    for (field_spec, value, object_names) in data {
                        let stat = SAIStat::from_ipfix(
                            black_box(field_spec),
                            black_box(value),
                            black_box(object_names)
                        );
                        stats.push(stat);
                    }
                    black_box(stats)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark IPFIX message parsing with different complexity levels
fn bench_ipfix_message_complexity(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let mut group = c.benchmark_group("ipfix_message_complexity");
    group.measurement_time(Duration::from_secs(8));
    
    for &complexity in &[10, 50, 100, 500] {
        group.throughput(Throughput::Elements(complexity as u64));
        
        group.bench_with_input(
            BenchmarkId::new("full_pipeline_simulation", complexity),
            &complexity,
            |b, &record_count| {
                b.to_async(&rt).iter(|| async {
                    // Simulate the full IPFIX processing pipeline
                    let test_data = create_test_ipfix_data(record_count, IpfixDataPattern::ComplexMixed);
                    let observation_time = 1672531200000000000u64;
                    
                    let mut all_stats = Vec::with_capacity(test_data.len());
                    
                    // Process each IPFIX record
                    for (field_spec, value, object_names) in &test_data {
                        let stat = SAIStat::from_ipfix(
                            black_box(field_spec),
                            black_box(value),
                            black_box(object_names)
                        );
                        all_stats.push(stat);
                    }
                    
                    // Create SAI stats message
                    let sai_stats = SAIStats::new(observation_time, all_stats);
                    let message = Arc::new(sai_stats);
                    
                    black_box(message)
                })
            }
        );
    }
    group.finish();
}

/// Benchmark string operations during IPFIX parsing
fn bench_ipfix_string_operations(c: &mut Criterion) {
    let mut group = c.benchmark_group("ipfix_string_operations");
    
    let object_name_patterns = [
        ("short_names", IpfixDataPattern::SimplePort),
        ("medium_names", IpfixDataPattern::ComplexMixed),
        ("long_names", IpfixDataPattern::LargeObjects),
    ];
    
    for (pattern_name, pattern) in object_name_patterns.iter() {
        let object_names = create_test_object_names(1000, *pattern);
        
        group.bench_with_input(
            BenchmarkId::new("string_clone_operations", pattern_name),
            &object_names,
            |b, names| {
                b.iter(|| {
                    let mut cloned_names = Vec::with_capacity(names.len());
                    for i in 0..100 {
                        let index = i % names.len();
                        let cloned = black_box(&names[index]).clone();
                        cloned_names.push(cloned);
                    }
                    black_box(cloned_names)
                })
            }
        );
    }
    group.finish();
}

criterion_group!(
    benches,
    bench_sai_stat_from_ipfix,
    bench_batch_ipfix_parsing,
    bench_field_size_parsing,
    bench_enterprise_number_parsing,
    bench_object_name_resolution,
    bench_time_field_extraction,
    bench_ipfix_memory_patterns,
    bench_ipfix_message_complexity,
    bench_ipfix_string_operations
);
criterion_main!(benches);
