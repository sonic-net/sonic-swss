#[cfg(test)]
mod end_to_end_tests {
    use serial_test::serial;
    use std::sync::Arc;
    use std::time::Duration;
    use tokio::{spawn, sync::mpsc::channel, time::timeout};

    use countersyncd::actor::{
        ipfix::IpfixActor,
        stats_reporter::{StatsReporterActor, StatsReporterConfig},
    };

    const TEST_TIMEOUT: Duration = Duration::from_secs(5);

    /// Mock writer for capturing stats output during testing
    #[derive(Debug)]
    pub struct TestWriter {
        pub messages: Arc<std::sync::Mutex<Vec<String>>>,
    }

    impl TestWriter {
        pub fn new() -> Self {
            Self {
                messages: Arc::new(std::sync::Mutex::new(Vec::new())),
            }
        }

        pub fn get_messages(&self) -> Vec<String> {
            self.messages.lock().unwrap().clone()
        }
    }

    impl Clone for TestWriter {
        fn clone(&self) -> Self {
            Self {
                messages: Arc::clone(&self.messages),
            }
        }
    }

    impl countersyncd::actor::stats_reporter::OutputWriter for TestWriter {
        fn write_line(&mut self, line: &str) {
            // Use std::sync::Mutex instead of tokio::sync::Mutex to avoid async issues
            if let Ok(mut guard) = self.messages.lock() {
                guard.push(line.to_string());
            }
        }
    }

    /// Creates a mock IPFIX template for testing (copied from working test in ipfix.rs)
    fn create_test_ipfix_template() -> Vec<u8> {
        vec![
            0x00, 0x0A, 0x00, 0x2C, // line 0 Packet 1 - Version 10, Length 44
            0x00, 0x00, 0x00, 0x00, // line 1 - Export time
            0x00, 0x00, 0x00, 0x01, // line 2 - Sequence number
            0x00, 0x00, 0x00, 0x00, // line 3 - Observation domain ID
            0x00, 0x02, 0x00, 0x1C, // line 4 - Set Header: Set ID=2, Length=28
            0x01, 0x00, 0x00, 0x03, // line 5 - Template ID 256, 3 fields
            0x01, 0x45, 0x00, 0x08, // line 6 - Field ID 325, 8 bytes
            0x80, 0x01, 0x00, 0x08, // line 7 - Field ID 128, 8 bytes
            0x00, 0x01, 0x00, 0x02, // line 8 - Enterprise Number 1, Field ID 1, 2 bytes
            0x80, 0x02, 0x00, 0x08, // line 9 - Field ID 129, 8 bytes
            0x80, 0x03, 0x80, 0x04, // line 10 - Enterprise Number 128, Field ID 2
        ]
    }

    /// Creates test IPFIX data records (matching the template)
    fn create_test_ipfix_data() -> Vec<u8> {
        vec![
            0x00, 0x0A, 0x00, 0x2C, // line 0 - Version 10, Length 44
            0x00, 0x00, 0x00, 0x00, // line 1 - Export time
            0x00, 0x00, 0x00, 0x02, // line 2 - Sequence number
            0x00, 0x00, 0x00, 0x00, // line 3 - Observation domain ID
            0x01, 0x00, 0x00, 0x1C, // line 4 - Data Set Header: Set ID=256, Length=28
            // Data Record (24 bytes total)
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xE8, // Field 1 (8 bytes) = 1000
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0xD0, // Field 2 (8 bytes) = 2000
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F, 0xA0, // Field 3 (8 bytes) = 4000
        ]
    }

    /// End-to-end system test that validates the IPFIX processing pipeline:
    /// 1. Send IPFIX templates to IpfixActor
    /// 2. Send IPFIX data through simulated netlink
    /// 3. Verify that SAI statistics are generated and reported
    #[tokio::test]
    #[serial] // Ensure this test runs in isolation
    async fn test_end_to_end_ipfix_processing() {
        // Setup logging for the test
        let _ = env_logger::builder().is_test(true).try_init();

        // Create communication channels
        let (ipfix_template_sender, ipfix_template_receiver) = channel(1);
        let (socket_sender, socket_receiver) = channel(10);
        let (saistats_sender, mut saistats_receiver) = channel(100);
        let (reporter_sender, reporter_receiver) = channel(100);

        // Create test writer to capture output
        let test_writer = TestWriter::new();
        let test_writer_clone = test_writer.clone();

        // Initialize actors
        let mut ipfix = IpfixActor::new(ipfix_template_receiver, socket_receiver);
        ipfix.add_recipient(saistats_sender);
        ipfix.add_recipient(reporter_sender);

        let reporter_config = StatsReporterConfig {
            interval: Duration::from_millis(100), // Fast reporting for test
            detailed: true,
            max_stats_per_report: Some(10),
        };
        let stats_reporter =
            StatsReporterActor::new(reporter_receiver, reporter_config, test_writer_clone);

        // Spawn actor tasks
        let ipfix_handle = spawn(IpfixActor::run(ipfix));

        let stats_handle = spawn(async move {
            StatsReporterActor::run(stats_reporter).await;
        });

        // Step 1: Send IPFIX template (simulating SwssActor -> IpfixActor)
        let template_data = create_test_ipfix_template();
        let template_message = countersyncd::message::ipfix::IPFixTemplatesMessage::new(
            "test_session|PORT".to_string(),
            Arc::new(template_data),
            Some(vec!["Ethernet0".to_string(), "Ethernet1".to_string()]),
            Some(vec![1, 2]),
        );

        ipfix_template_sender
            .send(template_message)
            .await
            .expect("Failed to send template message");

        // Capacity one makes this wait for template receipt; the actor handles it before data.
        let template_permit = timeout(TEST_TIMEOUT, ipfix_template_sender.reserve())
            .await
            .expect("template receipt timed out")
            .expect("template channel should remain open");
        drop(template_permit);

        println!("Sent IPFIX template to IpfixActor");

        // Step 2: Send IPFIX data (simulating NetlinkActor -> IpfixActor)
        let ipfix_data_packets = vec![
            create_test_ipfix_data(),
            create_test_ipfix_data(), // Send multiple packets to see more stats
        ];

        for data in ipfix_data_packets {
            socket_sender
                .send(Arc::new(data))
                .await
                .expect("Failed to send test netlink data");
        }

        let stats_collector = spawn(async move {
            let mut logical_records = 0usize;
            let mut counters = 0usize;
            let mut observation_times = Vec::new();
            while logical_records < 2 {
                let batch = timeout(TEST_TIMEOUT, saistats_receiver.recv())
                    .await
                    .expect("SAI stats receipt timed out")
                    .expect("SAI stats channel closed early");
                for record in batch.iter() {
                    logical_records += 1;
                    counters += record.stats.len();
                    observation_times.push(record.observation_time);
                }
            }
            assert_eq!(counters, 4);
            assert_eq!(observation_times, vec![1000, 1000]);
        });
        timeout(TEST_TIMEOUT, stats_collector)
            .await
            .expect("stats collection timed out")
            .expect("stats collector should join");

        drop(socket_sender);
        drop(ipfix_template_sender);
        timeout(TEST_TIMEOUT, ipfix_handle)
            .await
            .expect("IPFIX shutdown timed out")
            .expect("IPFIX task should join")
            .expect_err("IPFIX actor should report closed input channels");
        timeout(TEST_TIMEOUT, stats_handle)
            .await
            .expect("stats reporter shutdown timed out")
            .expect("stats reporter should join");

        // Step 3: Check that stats were generated and reported
        let messages = test_writer.get_messages();

        // Validate the test results
        println!("Captured {} messages from stats reporter", messages.len());
        for (i, msg) in messages.iter().enumerate() {
            println!("Message {}: {}", i, msg);
        }

        // For a complete test, we should see:
        // 1. Template processing messages
        // 2. Data processing messages
        // 3. SAI stats generation
        assert!(
            !messages.is_empty(),
            "Should have received some stats messages"
        );

        println!("End-to-end test completed successfully");
    }

    /// Test helper to create a mock IPFIX data stream for direct injection
    #[tokio::test]
    async fn test_direct_ipfix_data_injection() {
        // This test focuses on the IPFIX -> SAI stats portion of the pipeline
        let (ipfix_template_sender, ipfix_template_receiver) = channel(1);
        let (socket_sender, socket_receiver) = channel(10);
        let (saistats_sender, mut saistats_receiver) = channel(100);
        let (reporter_sender, reporter_receiver) = channel(100);

        let test_writer = TestWriter::new();
        let test_writer_clone = test_writer.clone();

        // Setup IPFIX actor
        let mut ipfix = IpfixActor::new(ipfix_template_receiver, socket_receiver);
        ipfix.add_recipient(saistats_sender);
        ipfix.add_recipient(reporter_sender);

        // Setup stats reporter
        let reporter_config = StatsReporterConfig {
            interval: Duration::from_millis(50),
            detailed: true,
            max_stats_per_report: Some(5),
        };
        let stats_reporter =
            StatsReporterActor::new(reporter_receiver, reporter_config, test_writer_clone);

        // Spawn actors
        let ipfix_handle = spawn(IpfixActor::run(ipfix));

        let stats_handle = spawn(async move {
            StatsReporterActor::run(stats_reporter).await;
        });

        // Step 1: Send IPFIX template
        let template_data = create_test_ipfix_template();
        let template_message = countersyncd::message::ipfix::IPFixTemplatesMessage::new(
            "direct_test".to_string(),
            Arc::new(template_data),
            Some(vec!["Ethernet0".to_string(), "Ethernet1".to_string()]),
            Some(vec![1, 2]),
        );

        ipfix_template_sender
            .send(template_message)
            .await
            .expect("Failed to send template message");

        // Unknown-template data is dropped, so wait for template receipt before sending it.
        let template_permit = timeout(TEST_TIMEOUT, ipfix_template_sender.reserve())
            .await
            .expect("template receipt timed out")
            .expect("template channel should remain open");
        drop(template_permit);

        // Step 2: Send IPFIX data
        let data = create_test_ipfix_data();
        socket_sender
            .send(Arc::new(data))
            .await
            .expect("IPFIX data should be sent");

        let batch = timeout(TEST_TIMEOUT, saistats_receiver.recv())
            .await
            .expect("SAI stats receipt timed out")
            .expect("should receive a SAI stats batch");
        let records: Vec<_> = batch.iter().collect();
        assert_eq!(records.len(), 1);
        assert_eq!(records[0].stats.len(), 2);
        assert_eq!(records[0].observation_time, 1000);

        drop(socket_sender);
        drop(ipfix_template_sender);
        timeout(TEST_TIMEOUT, ipfix_handle)
            .await
            .expect("IPFIX shutdown timed out")
            .expect("IPFIX task should join")
            .expect_err("IPFIX actor should report closed input channels");
        timeout(TEST_TIMEOUT, stats_handle)
            .await
            .expect("stats reporter shutdown timed out")
            .expect("stats reporter should join");

        // Step 3: Verify results
        let messages = test_writer.get_messages();
        println!("Direct injection test captured {} messages", messages.len());
        for (i, msg) in messages.iter().enumerate() {
            println!("Message {}: {}", i, msg);
        }

        // We should have received some stats output
        assert!(
            messages.len() > 0,
            "Should have received stats messages from direct injection"
        );

        println!("Direct injection test completed successfully");
    }
}
