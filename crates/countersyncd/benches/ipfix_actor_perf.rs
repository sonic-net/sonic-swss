use criterion::{black_box, criterion_group, criterion_main, Criterion, BenchmarkId, Throughput};
use countersyncd::{
    actor::ipfix::IpfixActor,
    message::{
        saistats::{SAIStats, SAIStatsMessage, SAIStatsMessageExt},
        buffer::SocketBufferMessage,
        ipfix::IPFixTemplatesMessage,
    },
};
use tokio::sync::{mpsc, oneshot};
use tokio::time::{timeout, Duration, Instant};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};
use byteorder::{ByteOrder, NetworkEndian};
use log::{info, error, debug, warn};

/// IPFIX Message Generator compatible with IpfixActor
/// 
/// This generator creates IPFIX messages in the exact format expected by the IpfixActor
/// for proper conversion to SAIStats messages. Based on the test data in ipfix.rs.
#[derive(Debug, Clone)]
struct IpfixGenerator {
    /// Current sequence number
    sequence_number: u32,
    /// Current template ID
    template_id: u16,
    /// Source ID
    source_id: u32,
    /// Maximum stats per set (to avoid IPFIX size limits)
    max_stats_per_set: usize,
}

impl Default for IpfixGenerator {
    fn default() -> Self {
        Self {
            sequence_number: 0,
            template_id: 256,
            source_id: 1001,
            max_stats_per_set: 8000, // Safe limit for IPFIX messages
        }
    }
}

impl IpfixGenerator {
    /// Creates a new IPFIX generator
    fn new(source_id: u32) -> Self {
        Self {
            sequence_number: 0,
            template_id: 256,
            source_id,
            max_stats_per_set: 8000, // Safe limit for IPFIX messages
        }
    }

    /// Generate IPFIX template message that is compatible with IpfixActor
    /// 
    /// This template defines 3 fields per record:
    /// - Field 325: observationTimeNanoseconds (8 bytes)
    /// - Field with enterprise: SAI type + stat encoded in enterprise number (8 bytes)
    /// - Field with enterprise: Counter value (8 bytes)
    fn generate_template(&mut self, records_per_message: usize) -> IPFixTemplatesMessage {
        let mut template_data = Vec::new();
        
        // IPFIX Message Header (16 bytes)
        let total_size = 16 + 4 + 4 + (3 * 8); // Header + Set header + Template header + 3 field specs
        template_data.extend_from_slice(&[
            0x00, 0x0A,                                    // Version (10)
            ((total_size >> 8) & 0xFF) as u8,
            (total_size & 0xFF) as u8,                     // Length
            0x00, 0x00, 0x00, 0x00,                        // Export time
            ((self.sequence_number >> 24) & 0xFF) as u8,
            ((self.sequence_number >> 16) & 0xFF) as u8,
            ((self.sequence_number >> 8) & 0xFF) as u8,
            (self.sequence_number & 0xFF) as u8,           // Sequence number
            ((self.source_id >> 24) & 0xFF) as u8,
            ((self.source_id >> 16) & 0xFF) as u8,
            ((self.source_id >> 8) & 0xFF) as u8,
            (self.source_id & 0xFF) as u8,                 // Observation domain ID
        ]);
        
        // Template Set Header (4 bytes)
        let set_length = 4 + 4 + (3 * 8); // Set header + Template header + 3 field specs
        template_data.extend_from_slice(&[
            0x00, 0x02,                                    // Set ID (2 = Template Set)
            ((set_length >> 8) & 0xFF) as u8,
            (set_length & 0xFF) as u8,                     // Set Length
        ]);
        
        // Template Record Header (4 bytes)
        template_data.extend_from_slice(&[
            ((self.template_id >> 8) & 0xFF) as u8,
            (self.template_id & 0xFF) as u8,               // Template ID
            0x00, 0x03,                                    // Field Count (3 fields)
        ]);
        
        // Field 1: observationTimeNanoseconds (Field ID 325, standard field)
        template_data.extend_from_slice(&[
            0x01, 0x45,                                    // Information Element ID 325
            0x00, 0x08,                                    // Field Length (8 bytes)
            // No enterprise number for standard field
        ]);
        
        // Field 2: SAI type + stat (enterprise field) 
        template_data.extend_from_slice(&[
            0x80, 0x01,                                    // Information Element ID with enterprise bit
            0x00, 0x08,                                    // Field Length (8 bytes)
            0x00, 0x01, 0x00, 0x02,                        // Enterprise Number (encodes SAI type=1, stat=2)
        ]);
        
        // Field 3: Counter value (enterprise field)
        template_data.extend_from_slice(&[
            0x80, 0x02,                                    // Information Element ID with enterprise bit
            0x00, 0x08,                                    // Field Length (8 bytes)
            0x80, 0x03, 0x80, 0x04,                        // Enterprise Number (encodes SAI extensions)
        ]);
        
        self.sequence_number += 1;
        
        // Create object names for label resolution
        let object_names = (0..records_per_message).map(|i| format!("Ethernet{}", i)).collect();
        
        IPFixTemplatesMessage::new(
            "perf_test_key".to_string(),
            Arc::new(template_data),
            Some(object_names),
        )
    }

    /// Generate IPFIX data record
    fn generate_record(&mut self, stats_num: usize, base_counter_value: u64) -> SocketBufferMessage {
        let mut record_data = Vec::new();
        let mut remaining_stats = stats_num;
        let mut set_id = 256u16; // Data Set ID corresponds to Template ID
        
        // IPFIX Message Header (16 bytes)
        let header_start = record_data.len();
        record_data.extend_from_slice(&[0u8; 16]); // Reserve space for header
        
        while remaining_stats > 0 {
            set_id += 1;
            let current_batch = std::cmp::min(remaining_stats, self.max_stats_per_set);
            
            // Data Set Header (4 bytes)
            let set_header_start = record_data.len();
            record_data.extend_from_slice(&set_id.to_be_bytes()); // Set ID (matches template)
            record_data.extend_from_slice(&[0u8; 2]); // Length (to be filled later)
            
            // Data Records (8 bytes per field as defined in template)
            for stat_idx in 0..current_batch {
                // Counter value (8 bytes) - varies per statistic
                let counter_value = base_counter_value + (stat_idx as u64 * 1000);
                record_data.extend_from_slice(&counter_value.to_be_bytes());
            }
            
            // Update Set Length
            let set_length = record_data.len() - set_header_start;
            NetworkEndian::write_u16(&mut record_data[set_header_start + 2..set_header_start + 4], 
                                   set_length as u16);
            
            remaining_stats -= current_batch;
        }
        
        // Update IPFIX Message Header
        self.write_ipfix_header(&mut record_data, header_start);
        
        Arc::new(record_data)
    }

    /// Generate an 8KB IPFIX message with approximately the target size
    fn generate_8k_message(&mut self, base_counter_value: u64) -> SocketBufferMessage {
        // Calculate stats needed for ~8KB
        // Each stat: 8 bytes data + overhead
        // Target: 8192 bytes / 8 bytes per stat ≈ 1000 stats
        let target_stats = 1000; // ~8KB when including headers
        self.generate_record(target_stats, base_counter_value)
    }

    /// Write IPFIX message header at the specified position
    fn write_ipfix_header(&mut self, data: &mut [u8], header_start: usize) {
        // Calculate total length before creating mutable borrow
        let total_length = std::cmp::min(data.len(), 65535) as u16;
        
        let header = &mut data[header_start..header_start + 16];
        
        // Version Number (2 bytes) - IPFIX version 10
        NetworkEndian::write_u16(&mut header[0..2], 10);
        
        // Length (2 bytes) - Total message length
        NetworkEndian::write_u16(&mut header[2..4], total_length);
        
        // Export Time (4 bytes) - Current timestamp
        let export_time = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs() as u32;
        NetworkEndian::write_u32(&mut header[4..8], export_time);
        
        // Sequence Number (4 bytes)
        self.sequence_number += 1;
        NetworkEndian::write_u32(&mut header[8..12], self.sequence_number);
        
        // Observation Domain ID (4 bytes)
        NetworkEndian::write_u32(&mut header[12..16], self.source_id);
    }
}

/// Create mock IPFIX buffer messages for testing with realistic SAI statistics
fn create_mock_ipfix_buffer_message(
    metrics_count: usize,
    batch_id: u64,
) -> SocketBufferMessage {
    let mut message_data = Vec::new();
    
    // IPFIX Message Header (16 bytes)
    let header_start = message_data.len();
    message_data.extend_from_slice(&[0u8; 16]); // Reserve space for header
    
    // Calculate how many sets we need (max 8191 metrics per set due to 65535 byte limit)
    let max_metrics_per_set = 8191;
    let mut remaining_metrics = metrics_count;
    let mut set_id = 256u16;
    
    while remaining_metrics > 0 {
        set_id += 1;
        let current_batch = std::cmp::min(remaining_metrics, max_metrics_per_set);
        
        // Data Set Header (4 bytes)
        let set_header_start = message_data.len();
        message_data.extend_from_slice(&set_id.to_be_bytes()); // Set ID
        message_data.extend_from_slice(&[0u8; 2]); // Length (to be filled later)
        
        // Data Records - each metric is 8 bytes (counter value)
        for metric_idx in 0..current_batch {
            let counter_value = batch_id * 1000000 + metric_idx as u64;
            message_data.extend_from_slice(&counter_value.to_be_bytes());
        }
        
        // Update Set Length
        let set_length = message_data.len() - set_header_start;
        NetworkEndian::write_u16(
            &mut message_data[set_header_start + 2..set_header_start + 4], 
            set_length as u16
        );
        
        remaining_metrics -= current_batch;
    }
    
    // Update IPFIX Message Header
    let total_length = std::cmp::min(message_data.len(), 65535) as u16;
    
    // Version Number (2 bytes) - IPFIX version 10
    NetworkEndian::write_u16(&mut message_data[header_start..header_start + 2], 10);
    
    // Length (2 bytes)
    NetworkEndian::write_u16(&mut message_data[header_start + 2..header_start + 4], total_length);
    
    // Export Time (4 bytes) - Current timestamp
    let export_time = 1672531200u32 + batch_id as u32;
    NetworkEndian::write_u32(&mut message_data[header_start + 4..header_start + 8], export_time);
    
    // Sequence Number (4 bytes)
    NetworkEndian::write_u32(&mut message_data[header_start + 8..header_start + 12], batch_id as u32);
    
    // Observation Domain ID (4 bytes)
    NetworkEndian::write_u32(&mut message_data[header_start + 12..header_start + 16], 1001);
    
    Arc::new(message_data)
}

/// Create an IPFIX message with 8k metrics (approximately 8KB size)
/// This is the core function for testing 5 messages/second capability
fn create_8k_metrics_ipfix_message(batch_id: u64) -> SocketBufferMessage {
    let mut generator = IpfixGenerator::new(1001);
    // 8000 metrics * 8 bytes each ≈ 64KB of data + headers ≈ 8KB total message
    generator.generate_record(8000, batch_id * 1000000)
}

/// Benchmark: IPFIX Actor throughput with 10k messages of 8KB each
/// Tests the actor's ability to process high-volume message throughput
/// following the init -> send template -> benchmark loop pattern
fn bench_ipfix_actor_10k_throughput(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let mut group = c.benchmark_group("ipfix_actor_10k_throughput");
    group.measurement_time(Duration::from_secs(30));
    
    let message_count = 10000;
    let stats_per_message = 1000; // ~8KB per message
    let total_metrics = message_count * stats_per_message;
    
    group.throughput(Throughput::Elements(total_metrics as u64));
    
    group.bench_function("10k_messages_8k_each", |b| {
        b.to_async(&rt).iter(|| async {
            let mut generator = IpfixGenerator::new(1001);
            
            // Create channels with correct types
            let (template_tx, template_rx) = mpsc::channel::<IPFixTemplatesMessage>(10);
            let (buffer_tx, buffer_rx) = mpsc::channel::<SocketBufferMessage>(1000);
            let (stats_tx, mut stats_rx) = mpsc::channel::<SAIStatsMessage>(500);
            let (shutdown_tx, shutdown_rx) = oneshot::channel();

            // Create IPFIX Actor with correct signature (template_rx, buffer_rx)
            let mut actor = IpfixActor::new(template_rx, buffer_rx);
            
            // Add recipient for receiving processed statistics
            actor.add_recipient(stats_tx.clone());

            // Start Actor using the correct syntax
            let actor_handle = tokio::spawn(async move {
                IpfixActor::run(actor).await;
            });
            
            // init - send template
            let template = generator.generate_template(stats_per_message);
            if let Err(_) = template_tx.send(template).await {
                println!("Failed to send template");
            }
            
            // Wait for template processing
            tokio::time::sleep(Duration::from_millis(100)).await;
            
            // benchmark - start_time
            let start_time = Instant::now();
            
            // for i in range(10000): saistats_sender(buffer)
            let send_handle = tokio::spawn(async move {
                let mut sent = 0;
                for i in 0..message_count {
                    let buffer = generator.generate_8k_message(i as u64 * 1000000);
                    if buffer_tx.send(buffer).await.is_err() {
                        break;
                    }
                    sent += 1;
                    
                    // Yield occasionally to prevent blocking
                    if i % 100 == 0 {
                        tokio::task::yield_now().await;
                    }
                }
                sent
            });
            
            // Receive processed statistics
            let mut processed_messages = 0;
            let mut total_stats_received = 0;
            let mut processing_errors = 0;
            
            while processed_messages < message_count {
                match timeout(Duration::from_millis(100), stats_rx.recv()).await {
                    Ok(Some(sai_stats_msg)) => {
                        total_stats_received += sai_stats_msg.stats.len();
                        processed_messages += 1;
                    }
                    Ok(None) => break,
                    Err(_) => {
                        processing_errors += 1;
                        if processing_errors > 50 { // Allow some timeouts
                            break;
                        }
                    }
                }
            }
            
            // duration = end_time - start_time
            let duration = start_time.elapsed();
            let messages_sent = send_handle.await.unwrap_or(0);
            
            // 10000 * 8k / duration ?
            let total_bytes = messages_sent * 8192; // Approximate 8KB per message
            let throughput_mbps = total_bytes as f64 / duration.as_secs_f64() / (1024.0 * 1024.0);
            let metrics_per_sec = total_stats_received as f64 / duration.as_secs_f64();
            let success_rate = (processed_messages as f64 / messages_sent as f64) * 100.0;
            
            println!("\n=== IPFIX Actor 10K Throughput Test Results ===");
            println!("Messages sent: {}", messages_sent);
            println!("Messages processed: {}", processed_messages);
            println!("Total metrics processed: {}", total_stats_received);
            println!("Success rate: {:.1}%", success_rate);
            println!("Duration: {:?}", duration);
            println!("Throughput: {:.2} MB/s", throughput_mbps);
            println!("Metrics/second: {:.0}", metrics_per_sec);

            // Cleanup
            let _ = shutdown_tx.send(());
            let _ = timeout(Duration::from_millis(1000), actor_handle).await;

            black_box((processed_messages, total_stats_received, throughput_mbps, success_rate))
        })
    });
    
    group.finish();
}

/// Benchmark: 5 messages per second with 8K metrics each (target test)
fn bench_5_messages_per_second_8k_metrics(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let mut group = c.benchmark_group("5_messages_per_second_8k_metrics");
    group.measurement_time(Duration::from_secs(20)); // 20 seconds test duration
    group.sample_size(10); // Fewer samples due to longer test duration
    
    let metrics_per_message = 8000;
    let messages_per_second = 5;
    let test_duration_seconds = 10;
    let total_messages = messages_per_second * test_duration_seconds;
    let total_metrics = total_messages * metrics_per_message;
    
    group.throughput(Throughput::Elements(total_metrics as u64));
    
    group.bench_function("sustained_5_msg_sec_8k_metrics", |b| {
        b.to_async(&rt).iter(|| async {
            let mut generator = IpfixGenerator::new(1001);
            
            // Create channels with sufficient buffer and correct types
            let (template_tx, template_rx) = mpsc::channel::<IPFixTemplatesMessage>(10);
            let (buffer_tx, buffer_rx) = mpsc::channel::<SocketBufferMessage>(total_messages * 2);
            let (stats_tx, mut stats_rx) = mpsc::channel::<SAIStatsMessage>(total_messages);
            let (shutdown_tx, shutdown_rx) = oneshot::channel();

            // Create IPFIX Actor with correct signature (template_rx, buffer_rx)
            let mut actor = IpfixActor::new(template_rx, buffer_rx);
            
            // Add recipient for receiving processed statistics
            actor.add_recipient(stats_tx);

            // Start Actor using the correct syntax
            let actor_handle = tokio::spawn(async move {
                IpfixActor::run(actor).await;
            });
            
            // Send template first (following pseudocode init pattern)
            let template = generator.generate_template(metrics_per_message);
            let _ = template_tx.send(template).await;
            tokio::time::sleep(Duration::from_millis(100)).await;

            let start_time = Instant::now();
            
            // Generate and send messages at controlled rate (5 per second)
            let send_handle = tokio::spawn(async move {
                let mut messages_sent = 0;
                let generation_start = Instant::now();
                
                while messages_sent < total_messages && generation_start.elapsed().as_secs() < test_duration_seconds as u64 {
                    let msg = generator.generate_record(metrics_per_message, messages_sent as u64 * 1000000);
                    
                    if buffer_tx.send(msg).await.is_err() {
                        break;
                    }
                    
                    messages_sent += 1;
                    
                    // Control timing: wait to maintain exactly 5 messages/second
                    let expected_elapsed = Duration::from_millis(((messages_sent * 1000) / messages_per_second).try_into().unwrap());
                    let actual_elapsed = generation_start.elapsed();
                    
                    if expected_elapsed > actual_elapsed {
                        tokio::time::sleep(expected_elapsed - actual_elapsed).await;
                    }
                }
                
                messages_sent
            });

            // Receive and measure processed statistics
            let mut processed_messages = 0;
            let mut total_stats_received = 0;
            let mut processing_times = Vec::new();
            
            while processed_messages < total_messages {
                let msg_start = Instant::now();
                
                match timeout(Duration::from_millis(1000), stats_rx.recv()).await {
                    Ok(Some(sai_stats_msg)) => {
                        let msg_processing_time = msg_start.elapsed();
                        total_stats_received += sai_stats_msg.stats.len();
                        processed_messages += 1;
                        processing_times.push(msg_processing_time);
                    }
                    Ok(None) => break,
                    Err(_) => {
                        println!("Timeout waiting for message {}/{}", processed_messages + 1, total_messages);
                        break;
                    }
                }
            }

            let total_processing_time = start_time.elapsed();
            let messages_sent = send_handle.await.unwrap_or(0);
            
            // Calculate performance metrics
            let actual_message_rate = processed_messages as f64 / total_processing_time.as_secs_f64();
            let actual_metrics_rate = total_stats_received as f64 / total_processing_time.as_secs_f64();
            let success_rate = (processed_messages as f64 / messages_sent as f64) * 100.0;
            let avg_processing_time = if !processing_times.is_empty() {
                processing_times.iter().sum::<Duration>().as_millis() as f64 / processing_times.len() as f64
            } else {
                0.0
            };
            
            // Print detailed results
            println!("\n=== 5 Messages/Second 8K Metrics Test Results ===");
            println!("Messages sent: {}", messages_sent);
            println!("Messages processed: {}", processed_messages);
            println!("Total metrics processed: {}", total_stats_received);
            println!("Success rate: {:.1}%", success_rate);
            println!("Actual message rate: {:.2} msg/sec (target: 5.0)", actual_message_rate);
            println!("Actual metrics rate: {:.0} metrics/sec (target: 40,000)", actual_metrics_rate);
            println!("Average processing time per message: {:.2} ms", avg_processing_time);
            println!("Total test duration: {:.2} seconds", total_processing_time.as_secs_f64());
            
            // Test passes if we can handle at least 95% of messages at target rate
            let test_passed = success_rate >= 95.0 && actual_message_rate >= 4.75;
            println!("Test result: {}", if test_passed { "PASS" } else { "FAIL" });

            // Cleanup
            let _ = shutdown_tx.send(());
            let _ = timeout(Duration::from_millis(500), actor_handle).await;

            black_box((processed_messages, total_stats_received, actual_message_rate, success_rate))
        })
    });
    
    group.finish();
}

/// Benchmark: IPFIX Actor throughput test with 8k metrics messages
fn bench_ipfix_actor_basic_throughput(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let mut group = c.benchmark_group("ipfix_actor_basic_throughput");
    group.measurement_time(Duration::from_secs(5));

    // Only test the scenario we care about: 8k metrics messages
    let metrics_count = 8000;
    let message_count = 10;  // Small number for basic throughput test
    let total_metrics = metrics_count * message_count;
    
    group.throughput(Throughput::Elements(total_metrics as u64));
    
    group.bench_function("8k_metrics_messages", |b| {
        b.to_async(&rt).iter(|| async {
            // Create channels with correct types
            let (template_tx, template_rx) = mpsc::channel::<IPFixTemplatesMessage>(10);
            let (buffer_tx, buffer_rx) = mpsc::channel::<SocketBufferMessage>(message_count * 2);
            let (stats_tx, mut stats_rx) = mpsc::channel::<SAIStatsMessage>(message_count);
            let (shutdown_tx, shutdown_rx) = oneshot::channel();

            // Create IPFIX Actor
            let mut actor = IpfixActor::new(template_rx, buffer_rx);
            
            // Add recipient for receiving processed statistics
            actor.add_recipient(stats_tx);

            // Start Actor using the correct syntax
            let actor_handle = tokio::spawn(async move {
                IpfixActor::run(actor).await;
            });

            // Generate and send messages
            let send_handle = tokio::spawn(async move {
                for i in 0..message_count {
                    let msg = create_8k_metrics_ipfix_message(i as u64);
                    if buffer_tx.send(msg).await.is_err() {
                        break;
                    }
                }
            });

            // Receive processed statistics
            let start_time = Instant::now();
            let mut processed_messages = 0;
            let mut total_stats_received = 0;

            while processed_messages < message_count {
                match timeout(Duration::from_millis(100), stats_rx.recv()).await {
                    Ok(Some(sai_stats_msg)) => {
                        total_stats_received += sai_stats_msg.stats.len();
                        processed_messages += 1;
                    }
                    Ok(None) => break,
                    Err(_) => break, // Timeout
                }
            }

            let processing_time = start_time.elapsed();

            // Cleanup
            let _ = shutdown_tx.send(());
            let _ = send_handle.await;
            let _ = timeout(Duration::from_millis(100), actor_handle).await;

            black_box((processed_messages, total_stats_received, processing_time))
        })
    });
    
    group.finish();
}

/// Benchmark: Time measurement for each IPFIX message processing
/// Generates 10,000 messages, measures total time, calculates time per message
fn bench_each_ipfix_message_timing(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let mut group = c.benchmark_group("each_ipfix_message_timing");
    group.measurement_time(Duration::from_secs(30));
    group.sample_size(10);
    
    let message_count = 10000;
    let metrics_per_message = 8000; // 8k metrics per message as requested
    
    group.bench_function("each_message_processing_time", |b| {
        b.to_async(&rt).iter(|| async {
            let mut generator = IpfixGenerator::new(1001);
            
            // Create channels with sufficient buffer
            let (template_tx, template_rx) = mpsc::channel::<IPFixTemplatesMessage>(10);
            let (buffer_tx, buffer_rx) = mpsc::channel::<SocketBufferMessage>(message_count + 100);
            let (stats_tx, mut stats_rx) = mpsc::channel::<SAIStatsMessage>(message_count + 100);

            // Create and start IPFIX Actor
            let mut actor = IpfixActor::new(template_rx, buffer_rx);
            actor.add_recipient(stats_tx);

            let actor_handle = tokio::spawn(async move {
                IpfixActor::run(actor).await;
            });
            
            // Send template first
            let template = generator.generate_template(metrics_per_message);
            let _ = template_tx.send(template).await;
            tokio::time::sleep(Duration::from_millis(100)).await; // Wait for template processing

            // *** START TIME ***
            let start_time = Instant::now();
            
            // Generate and send 10,000 IPFIX messages
            let send_handle = tokio::spawn(async move {
                for i in 0..message_count {
                    let msg = generator.generate_record(metrics_per_message, i as u64 * 1000000);
                    if buffer_tx.send(msg).await.is_err() {
                        println!("Failed to send message {}", i);
                        break;
                    }
                }
                message_count
            });

            // Receive all processed messages
            let mut processed_messages = 0;
            let mut total_stats_received = 0;
            
            while processed_messages < message_count {
                match timeout(Duration::from_millis(100), stats_rx.recv()).await {
                    Ok(Some(sai_stats_msg)) => {
                        total_stats_received += sai_stats_msg.stats.len();
                        processed_messages += 1;
                    }
                    Ok(None) => {
                        println!("Channel closed after {} messages", processed_messages);
                        break;
                    }
                    Err(_) => {
                        println!("Timeout after processing {} messages", processed_messages);
                        break;
                    }
                }
            }
            
            // *** END TIME ***
            let end_time = Instant::now();
            let duration = end_time.duration_since(start_time);
            
            // Calculate timing statistics
            let messages_sent = send_handle.await.unwrap_or(0);
            let total_duration_ms = duration.as_millis() as f64;
            let total_duration_secs = duration.as_secs_f64();
            let time_per_message_ms = total_duration_ms / processed_messages as f64;
            let time_per_message_us = (total_duration_secs * 1_000_000.0) / processed_messages as f64;
            let messages_per_second = processed_messages as f64 / total_duration_secs;
            let metrics_per_second = total_stats_received as f64 / total_duration_secs;
            
            // Print detailed timing results
            println!("\n=== Each IPFIX Message Timing Results ===");
            println!("Messages sent: {}", messages_sent);
            println!("Messages processed: {}", processed_messages);
            println!("Total metrics processed: {}", total_stats_received);
            println!("Total duration: {:.3} seconds ({:.1} ms)", total_duration_secs, total_duration_ms);
            println!("Time per message: {:.3} ms ({:.1} μs)", time_per_message_ms, time_per_message_us);
            println!("Processing rate: {:.1} messages/second", messages_per_second);
            println!("Metrics rate: {:.0} metrics/second", metrics_per_second);
            println!("Success rate: {:.1}%", (processed_messages as f64 / messages_sent as f64) * 100.0);
            
            // Determine if performance meets your requirements
            let meets_5_msg_per_sec = messages_per_second >= 5.0;
            println!("Can handle 5+ msg/sec? {}", if meets_5_msg_per_sec { "YES" } else { "NO" });

            // Cleanup
            let _ = timeout(Duration::from_millis(500), actor_handle).await;

            black_box((processed_messages, time_per_message_ms, messages_per_second, total_stats_received))
        })
    });
    
    group.finish();
}

/// Simple test to verify IPFIX actor can process basic messages
fn bench_simple_ipfix_test(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let mut group = c.benchmark_group("simple_ipfix_test");
    group.measurement_time(Duration::from_secs(10));
    group.sample_size(10);
    
    group.bench_function("basic_conversion_test", |b| {
        b.to_async(&rt).iter(|| async {
            println!("=== SIMPLE IPFIX TEST STARTING ===");
            
            // Create channels
            let (template_tx, template_rx) = mpsc::channel::<IPFixTemplatesMessage>(10);
            let (buffer_tx, buffer_rx) = mpsc::channel::<SocketBufferMessage>(100);
            let (stats_tx, mut stats_rx) = mpsc::channel::<SAIStatsMessage>(100);

            // Create and start IPFIX Actor
            let mut actor = IpfixActor::new(template_rx, buffer_rx);
            actor.add_recipient(stats_tx);

            println!("Starting IPFIX Actor...");
            let actor_handle = tokio::spawn(async move {
                IpfixActor::run(actor).await;
            });
            
            // Try using the existing mock function instead of custom generator
            println!("Testing with mock IPFIX buffer message...");
            
            // Send a simple test message using the existing mock function
            let test_message = create_mock_ipfix_buffer_message(100, 1); // 100 metrics, batch_id=1
            println!("Generated test message, size: {} bytes", test_message.len());
            
            match buffer_tx.send(test_message).await {
                Ok(_) => println!("Test message sent successfully"),
                Err(e) => println!("Failed to send test message: {:?}", e),
            }
            
            // Try to receive the processed result
            let mut received_any = false;
            println!("Waiting for processed result...");
            
            for attempt in 1..=5 {
                match timeout(Duration::from_millis(1000), stats_rx.recv()).await {
                    Ok(Some(sai_stats_msg)) => {
                        println!("SUCCESS! Received SAI stats with {} metrics", sai_stats_msg.stats.len());
                        received_any = true;
                        break;
                    }
                    Ok(None) => {
                        println!("Channel closed on attempt {}", attempt);
                        break;
                    }
                    Err(_) => {
                        println!("Timeout on attempt {} of 5", attempt);
                    }
                }
            }
            
            if !received_any {
                println!("FAILED: No messages were processed by the actor");
                println!("This suggests the IPFIX format might be incorrect");
            }
            
            // Cleanup
            let _ = timeout(Duration::from_millis(500), actor_handle).await;
            
            black_box(received_any)
        })
    });
    
    group.finish();
}

/// Test different IPFIX message formats to see which one works
fn bench_ipfix_format_debug(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let mut group = c.benchmark_group("ipfix_format_debug");
    group.measurement_time(Duration::from_secs(10));
    group.sample_size(10);
    
    group.bench_function("format_comparison", |b| {
        b.to_async(&rt).iter(|| async {
            println!("=== IPFIX FORMAT DEBUG TEST ===");
            
            // Test 1: Using create_mock_ipfix_buffer_message
            println!("\n--- Test 1: Mock IPFIX Buffer ---");
            let mock_msg = create_mock_ipfix_buffer_message(10, 1);
            println!("Mock message size: {} bytes", mock_msg.len());
            println!("First 32 bytes: {:02x?}", &mock_msg[..std::cmp::min(32, mock_msg.len())]);
            
            // Test 2: Using IpfixGenerator
            println!("\n--- Test 2: IPFIX Generator ---");
            let mut generator = IpfixGenerator::new(1001);
            let generator_msg = generator.generate_record(10, 1000);
            println!("Generator message size: {} bytes", generator_msg.len());
            println!("First 32 bytes: {:02x?}", &generator_msg[..std::cmp::min(32, generator_msg.len())]);
            
            // Test 3: Using create_8k_metrics_ipfix_message
            println!("\n--- Test 3: 8K Metrics Message ---");
            let metrics_8k_msg = create_8k_metrics_ipfix_message(1);
            println!("8K metrics message size: {} bytes", metrics_8k_msg.len());
            println!("First 32 bytes: {:02x?}", &metrics_8k_msg[..std::cmp::min(32, metrics_8k_msg.len())]);
            
            // Now test which one actually gets processed by the actor
            let (template_tx, template_rx) = mpsc::channel::<IPFixTemplatesMessage>(10);
            let (buffer_tx, buffer_rx) = mpsc::channel::<SocketBufferMessage>(100);
            let (stats_tx, mut stats_rx) = mpsc::channel::<SAIStatsMessage>(100);

            let mut actor = IpfixActor::new(template_rx, buffer_rx);
            actor.add_recipient(stats_tx);

            let actor_handle = tokio::spawn(async move {
                IpfixActor::run(actor).await;
            });
            
            // Test each message format
            let test_messages = vec![
                ("Mock Buffer", mock_msg),
                ("IPFIX Generator", generator_msg),
                ("8K Metrics", metrics_8k_msg),
            ];
            
            for (name, msg) in test_messages {
                println!("\n--- Testing {} ---", name);
                
                match buffer_tx.send(msg).await {
                    Ok(_) => println!("{}: Message sent successfully", name),
                    Err(e) => {
                        println!("{}: Failed to send - {:?}", name, e);
                        continue;
                    }
                }
                
                // Wait for processing
                match timeout(Duration::from_millis(2000), stats_rx.recv()).await {
                    Ok(Some(sai_stats_msg)) => {
                        println!("{}: SUCCESS! Processed {} metrics", name, sai_stats_msg.stats.len());
                    }
                    Ok(None) => {
                        println!("{}: Channel closed", name);
                    }
                    Err(_) => {
                        println!("{}: Timeout - not processed", name);
                    }
                }
            }
            
            // Cleanup
            let _ = timeout(Duration::from_millis(500), actor_handle).await;
            
            black_box(true)
        })
    });
    
    group.finish();
}

/// Benchmark: IPFIX to SAIStats conversion timing with properly formatted messages
/// This measures how long the IpfixActor takes to convert IPFIX messages to SAIStats
fn bench_ipfix_to_saistats_conversion_timing(c: &mut Criterion) {
    let rt = tokio::runtime::Runtime::new().unwrap();
    let mut group = c.benchmark_group("ipfix_to_saistats_conversion_timing");
    group.measurement_time(Duration::from_secs(10));
    
    // Test with 8000 bytes of metrics per message (500 records × 16 bytes metrics each)
    let target_metrics_bytes = 8000;
    let records_per_message = target_metrics_bytes / 16; // 16 bytes metrics per record
    
    group.throughput(Throughput::Bytes(target_metrics_bytes as u64));
    
    group.bench_function("8000_bytes_metrics_conversion", |b| {
        b.to_async(&rt).iter(|| async {
            // Create properly formatted IPFIX messages
            let mut generator = IpfixGenerator::new(1001);
            
            // Create channels with appropriate types
            let (template_tx, template_rx) = mpsc::channel::<IPFixTemplatesMessage>(1);
            let (buffer_tx, buffer_rx) = mpsc::channel::<SocketBufferMessage>(1);
            let (stats_tx, mut stats_rx) = mpsc::channel::<SAIStatsMessage>(1);

            // Create IPFIX Actor
            let mut actor = IpfixActor::new(template_rx, buffer_rx);
            actor.add_recipient(stats_tx.clone());

            // Start IPFIX Actor
            let actor_handle = tokio::spawn(async move {
                IpfixActor::run(actor).await;
            });
            
            // Send template first (required for proper IPFIX processing)
            let template = generator.generate_template(records_per_message);
            let _ = template_tx.send(template).await;
            
            // Wait a moment for template processing
            tokio::time::sleep(Duration::from_millis(10)).await;
            
            // Generate IPFIX data record with 8000 bytes of metrics
            let base_timestamp = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos() as u64;
                
            let ipfix_message = generator.generate_record(target_metrics_bytes, base_timestamp);
            
            // Measure conversion timing
            let conversion_start = Instant::now();
            
            // Send IPFIX message to actor
            if buffer_tx.send(ipfix_message).await.is_err() {
                panic!("Failed to send IPFIX message to actor");
            }
            
            // Wait for converted SAIStats message
            let saistats_result = match timeout(Duration::from_secs(5), stats_rx.recv()).await {
                Ok(Some(sai_stats_msg)) => {
                    let conversion_time = conversion_start.elapsed();
                    
                    // Verify the conversion worked correctly
                    let metrics_count = sai_stats_msg.stats.len();
                    let expected_metrics = records_per_message; // Each record produces 1 SAI stat
                    
                    if metrics_count != expected_metrics {
                        warn!("Metrics count mismatch: expected {}, got {}", expected_metrics, metrics_count);
                    }
                    
                    Some((conversion_time, metrics_count, sai_stats_msg))
                },
                Ok(None) => {
                    error!("SAIStats channel closed without result");
                    None
                },
                Err(_) => {
                    error!("Timeout waiting for SAIStats conversion");
                    None
                }
            };
            
            // Cleanup
            drop(template_tx);
            drop(buffer_tx);
            drop(stats_tx);
            let _ = timeout(Duration::from_millis(500), actor_handle).await;
            
            // Return conversion metrics
            if let Some((conversion_time, metrics_count, sai_stats_msg)) = saistats_result {
                let conversion_time_micros = conversion_time.as_micros();
                let metrics_per_second = if conversion_time.as_secs_f64() > 0.0 {
                    metrics_count as f64 / conversion_time.as_secs_f64()
                } else {
                    0.0
                };
                
                // Print performance details
                debug!("IPFIX->SAIStats conversion: {} μs for {} metrics ({:.0} metrics/sec)", 
                       conversion_time_micros, metrics_count, metrics_per_second);
                debug!("Observation time: {}, First metric: {:?}", 
                       sai_stats_msg.observation_time, 
                       sai_stats_msg.stats.first());
                
                black_box(conversion_time_micros)
            } else {
                // Return a very high value to indicate failure
                black_box(u128::MAX)
            }
        })
    });
    
    group.finish();
}

// Focus on measuring time per IPFIX message
criterion_group!(
    benches,
    bench_simple_ipfix_test, 
    bench_ipfix_format_debug,
    bench_each_ipfix_message_timing,         // Time per IPFIX message processing
    bench_ipfix_to_saistats_conversion_timing, // IPFIX Actor conversion timing
    bench_5_messages_per_second_8k_metrics,  // Can actor handle 5 msgs/sec with 8k metrics
    bench_ipfix_actor_basic_throughput,      // Basic throughput validation
);
criterion_main!(benches);
