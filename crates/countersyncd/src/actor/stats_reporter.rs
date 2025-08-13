use std::collections::HashMap;
use std::time::Duration;

use log::{debug, info};
use tokio::{
    select,
    sync::mpsc::Receiver,
    time::{interval, Interval},
};

use super::super::message::saistats::SAIStatsMessage;

/// Unique key for identifying a specific counter based on the triplet
/// (object_name, type_id, stat_id)
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct CounterKey {
    pub object_name: String,
    pub type_id: u32,
    pub stat_id: u32,
}

impl CounterKey {
    pub fn new(object_name: String, type_id: u32, stat_id: u32) -> Self {
        Self {
            object_name,
            type_id,
            stat_id,
        }
    }
}

/// Counter information including the latest value and associated metadata
#[derive(Debug, Clone)]
pub struct CounterInfo {
    pub counter: u64,
    pub last_observation_time: u64,
}

/// Trait for output writing to enable testing
pub trait OutputWriter: Send + Sync {
    fn write_line(&mut self, line: &str);
}

/// Console writer implementation
pub struct ConsoleWriter;

impl OutputWriter for ConsoleWriter {
    fn write_line(&mut self, line: &str) {
        println!("{}", line);
    }
}

/// Test writer that captures output
#[cfg(test)]
pub struct TestWriter {
    pub lines: Vec<String>,
}

#[cfg(test)]
impl TestWriter {
    pub fn new() -> Self {
        Self { lines: Vec::new() }
    }

    #[allow(dead_code)]
    pub fn get_output(&self) -> &[String] {
        &self.lines
    }
}

#[cfg(test)]
impl OutputWriter for TestWriter {
    fn write_line(&mut self, line: &str) {
        self.lines.push(line.to_string());
    }
}

/// Configuration for the StatsReporterActor
#[derive(Debug, Clone)]
pub struct StatsReporterConfig {
    /// Reporting interval - how often to print the latest statistics
    pub interval: Duration,
    /// Whether to print detailed statistics or summary only
    pub detailed: bool,
    /// Maximum number of statistics to display per report
    pub max_stats_per_report: Option<usize>,
}

impl Default for StatsReporterConfig {
    fn default() -> Self {
        Self {
            interval: Duration::from_secs(10),
            detailed: true,
            max_stats_per_report: None,
        }
    }
}

/// Actor responsible for consuming SAI statistics messages and reporting them to the terminal.
/// 
/// The StatsReporterActor handles:
/// - Receiving SAI statistics messages from IPFIX processor
/// - Maintaining the latest statistics state per counter key (object_name, type_id, stat_id)
/// - Tracking message counts per counter key for each reporting period
/// - Periodic reporting based on configured interval
/// - Formatted output to terminal with optional detail levels
pub struct StatsReporterActor<W: OutputWriter> {
    /// Channel for receiving SAI statistics messages
    stats_receiver: Receiver<SAIStatsMessage>,
    /// Configuration for reporting behavior
    config: StatsReporterConfig,
    /// Timer for periodic reporting
    report_timer: Interval,
    /// Latest counter values indexed by (object_name, type_id, stat_id) key
    latest_counters: HashMap<CounterKey, CounterInfo>,
    /// Message count per counter key for current reporting period
    messages_per_counter: HashMap<CounterKey, u64>,
    /// Total messages received across all counters
    total_messages_received: u64,
    /// Counter for total reports generated
    reports_generated: u64,
    /// Output writer for dependency injection
    writer: W,
}

impl<W: OutputWriter> StatsReporterActor<W> {
    /// Creates a new StatsReporterActor instance.
    /// 
    /// # Arguments
    /// 
    /// * `stats_receiver` - Channel for receiving SAI statistics messages
    /// * `config` - Configuration for reporting behavior
    /// * `writer` - Output writer for dependency injection
    /// 
    /// # Returns
    /// 
    /// A new StatsReporterActor instance
    pub fn new(stats_receiver: Receiver<SAIStatsMessage>, config: StatsReporterConfig, writer: W) -> Self {
        let report_timer = interval(config.interval);
        
        info!(
            "StatsReporter initialized with interval: {:?}, detailed: {}", 
            config.interval, config.detailed
        );

        Self {
            stats_receiver,
            config,
            report_timer,
            latest_counters: HashMap::new(),
            messages_per_counter: HashMap::new(),
            total_messages_received: 0,
            reports_generated: 0,
            writer,
        }
    }

    /// Creates a new StatsReporterActor with default configuration and console writer.
    /// 
    /// # Arguments
    /// 
    /// * `stats_receiver` - Channel for receiving SAI statistics messages
    /// 
    /// # Returns
    /// 
    /// A new StatsReporterActor instance with default settings
    #[allow(dead_code)]
    pub fn new_with_defaults(stats_receiver: Receiver<SAIStatsMessage>) -> StatsReporterActor<ConsoleWriter> {
        StatsReporterActor::new(stats_receiver, StatsReporterConfig::default(), ConsoleWriter)
    }

    /// Updates the internal state with new statistics data.
    /// 
    /// For each statistic in the message, updates:
    /// - The latest counter value for the (object_name, type_id, stat_id) key
    /// - The message count for that key in the current reporting period
    /// 
    /// # Arguments
    /// 
    /// * `stats_msg` - New SAI statistics message to process
    fn update_stats(&mut self, stats_msg: SAIStatsMessage) {
        self.total_messages_received += 1;
        
        // Extract SAIStats from Arc
        let stats = match std::sync::Arc::try_unwrap(stats_msg) {
            Ok(stats) => stats,
            Err(arc_stats) => (*arc_stats).clone(),
        };
        
        debug!("Received SAI stats with {} entries, observation_time: {}", 
               stats.stats.len(), stats.observation_time);

        // Process each statistic in the message
        for stat in stats.stats {
            let key = CounterKey::new(
                stat.object_name,
                stat.type_id,
                stat.stat_id,
            );

            // Update latest counter value
            let counter_info = CounterInfo {
                counter: stat.counter,
                last_observation_time: stats.observation_time,
            };
            self.latest_counters.insert(key.clone(), counter_info);

            // Increment message count for this counter key
            *self.messages_per_counter.entry(key).or_insert(0) += 1;
        }
    }

    /// Generates and prints a statistics report to the terminal.
    /// 
    /// Reports all current counter values and their triplets, as well as
    /// message counts for the current reporting period. After reporting,
    /// clears the per-period message counters.
    fn generate_report(&mut self) {
        self.reports_generated += 1;

        if self.latest_counters.is_empty() {
            self.writer.write_line(&format!("📊 [Report #{}] No statistics data available yet", self.reports_generated));
            self.writer.write_line(&format!("   Total Messages Received: {}", self.total_messages_received));
        } else {
            self.print_counters_report();
        }
        
        // Clear per-period message counters for next reporting period
        self.messages_per_counter.clear();
        
        self.writer.write_line(""); // Add blank line for readability
    }

    /// Prints formatted counters report to terminal.
    /// 
    /// Shows all current counters with their triplet keys and the number of
    /// messages received for each counter in the current reporting period.
    fn print_counters_report(&mut self) {
        self.writer.write_line(&format!("📊 [Report #{}] SAI Counters Report", self.reports_generated));
        self.writer.write_line(&format!("   Total Unique Counters: {}", self.latest_counters.len()));
        self.writer.write_line(&format!("   Total Messages Received: {}", self.total_messages_received));

        if self.config.detailed && !self.latest_counters.is_empty() {
            self.writer.write_line("   📈 Detailed Counters:");
            
            // Sort counters by key for consistent output
            let mut sorted_counters: Vec<_> = self.latest_counters.iter().collect();
            sorted_counters.sort_by(|a, b| {
                a.0.object_name.cmp(&b.0.object_name)
                    .then_with(|| a.0.type_id.cmp(&b.0.type_id))
                    .then_with(|| a.0.stat_id.cmp(&b.0.stat_id))
            });

            let counters_to_show = if let Some(max) = self.config.max_stats_per_report {
                &sorted_counters[..std::cmp::min(max, sorted_counters.len())]
            } else {
                &sorted_counters
            };

            for (index, (key, counter_info)) in counters_to_show.iter().enumerate() {
                let messages_in_period = self.messages_per_counter.get(key).unwrap_or(&0);
                let messages_per_second = *messages_in_period as f64 / self.config.interval.as_secs_f64();
                self.writer.write_line(&format!(
                    "      [{:3}] Object: {:15}, Type: {:10}, Stat: {:10}, Counter: {:15}, Msg/s: {:6.1}, LastTime: {}",
                    index + 1,
                    key.object_name,
                    key.type_id,
                    key.stat_id,
                    counter_info.counter,
                    messages_per_second,
                    counter_info.last_observation_time
                ));
            }

            if let Some(max) = self.config.max_stats_per_report {
                if self.latest_counters.len() > max {
                    self.writer.write_line(&format!(
                        "      ... and {} more counters (use max_stats_per_report: None to show all)",
                        self.latest_counters.len() - max
                    ));
                }
            }
        } else if !self.config.detailed && !self.latest_counters.is_empty() {
            // Summary mode - show aggregate information
            let total_counter_value: u64 = self.latest_counters.values().map(|info| info.counter).sum();
            let unique_types = self.latest_counters.keys().map(|k| k.type_id).collect::<std::collections::HashSet<_>>().len();
            let unique_objects = self.latest_counters.keys().map(|k| &k.object_name).collect::<std::collections::HashSet<_>>().len();
            let total_messages_in_period: u64 = self.messages_per_counter.values().sum();
            let messages_per_second = total_messages_in_period as f64 / self.config.interval.as_secs_f64();
            
            self.writer.write_line("   📊 Summary:");
            self.writer.write_line(&format!("      Total Counter Value: {}", total_counter_value));
            self.writer.write_line(&format!("      Unique Types: {}", unique_types));
            self.writer.write_line(&format!("      Unique Objects: {}", unique_objects));
            self.writer.write_line(&format!("      Messages per Second: {:.1}", messages_per_second));
        }
    }

    /// Main event loop for the StatsReporterActor.
    /// 
    /// Continuously processes incoming statistics messages and generates periodic reports.
    /// The loop will exit when the statistics channel is closed.
    /// 
    /// # Arguments
    /// 
    /// * `actor` - The StatsReporterActor instance to run
    pub async fn run(mut actor: StatsReporterActor<W>) {
        info!("StatsReporter actor started");

        loop {
            select! {
                // Handle incoming statistics messages
                stats_msg = actor.stats_receiver.recv() => {
                    match stats_msg {
                        Some(stats) => {
                            actor.update_stats(stats);
                        }
                        None => {
                            info!("Stats receiver channel closed, shutting down reporter");
                            break;
                        }
                    }
                }
                
                // Handle periodic reporting
                _ = actor.report_timer.tick() => {
                    actor.generate_report();
                }
            }
        }

        // Generate final report before shutdown
        info!("Generating final report before shutdown...");
        actor.generate_report();
        info!("StatsReporter actor terminated. Total reports generated: {}", actor.reports_generated);
    }
}

impl<W: OutputWriter> Drop for StatsReporterActor<W> {
    fn drop(&mut self) {
        info!("StatsReporter dropped after {} reports and {} messages", 
              self.reports_generated, self.total_messages_received);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    use tokio::{spawn, sync::mpsc::channel, time::sleep};
    
    use crate::message::saistats::{SAIStat, SAIStats};

    /// Helper function to create test SAI statistics
    fn create_test_stats(observation_time: u64, stat_count: usize) -> SAIStats {
        let stats = (0..stat_count)
            .map(|i| SAIStat {
                object_name: format!("Ethernet{}", i),
                type_id: (i * 100) as u32,
                stat_id: (i * 10) as u32,
                counter: (i * 1000) as u64,
            })
            .collect();

        SAIStats {
            observation_time,
            stats,
        }
    }

    #[tokio::test]
    async fn test_stats_reporter_basic_functionality() {
        let (sender, receiver) = channel(10);
        let test_writer = TestWriter::new();
        
        let config = StatsReporterConfig {
            interval: Duration::from_millis(200),
            detailed: true,
            max_stats_per_report: Some(3),
        };
        
        // Create actor with test writer
        let actor = StatsReporterActor::new(receiver, config, test_writer);
        let handle = spawn(StatsReporterActor::run(actor));

        // Send test statistics
        let test_stats = create_test_stats(12345, 5);
        sender.send(Arc::new(test_stats)).await.unwrap();

        // Wait for processing
        sleep(Duration::from_millis(50)).await;

        // Wait for at least one report
        sleep(Duration::from_millis(250)).await;

        // Send another set of statistics
        let test_stats2 = create_test_stats(67890, 2);
        sender.send(Arc::new(test_stats2)).await.unwrap();

        // Wait for processing
        sleep(Duration::from_millis(50)).await;

        // Close the channel to terminate the actor
        drop(sender);
        
        // Wait for actor to finish
        let _finished_actor = handle.await.expect("Actor should complete successfully");
    }

    #[tokio::test]
    async fn test_stats_reporter_with_shared_writer() {
        use std::sync::{Arc, Mutex};

        // Shared writer that can be accessed from multiple places
        #[derive(Clone)]
        struct SharedTestWriter {
            lines: Arc<Mutex<Vec<String>>>,
        }

        impl SharedTestWriter {
            fn new() -> Self {
                Self {
                    lines: Arc::new(Mutex::new(Vec::new())),
                }
            }

            fn get_lines(&self) -> Vec<String> {
                self.lines.lock().unwrap().clone()
            }
        }

        impl OutputWriter for SharedTestWriter {
            fn write_line(&mut self, line: &str) {
                self.lines.lock().unwrap().push(line.to_string());
            }
        }

        let (sender, receiver) = channel(10);
        let shared_writer = SharedTestWriter::new();
        let writer_clone = shared_writer.clone();
        
        let config = StatsReporterConfig {
            interval: Duration::from_millis(200),
            detailed: true,
            max_stats_per_report: Some(3),
        };
        
        // Create actor with shared writer
        let actor = StatsReporterActor::new(receiver, config, shared_writer);
        let handle = spawn(StatsReporterActor::run(actor));

        // Send test statistics
        let test_stats = create_test_stats(12345, 5);
        sender.send(Arc::new(test_stats)).await.unwrap();

        // Wait for processing
        sleep(Duration::from_millis(50)).await;

        // Wait for at least one report
        sleep(Duration::from_millis(250)).await;

        // Send another set of statistics
        let test_stats2 = create_test_stats(67890, 2);
        sender.send(Arc::new(test_stats2)).await.unwrap();

        // Wait for processing
        sleep(Duration::from_millis(50)).await;

        // Close the channel to terminate the actor
        drop(sender);
        
        // Wait for actor to finish
        handle.await.expect("Actor should complete successfully");
        
        // Now we can check the output
        let output = writer_clone.get_lines();

        // Verify we have some output
        assert!(!output.is_empty(), "Should have captured some output");
        
        // Verify report header is present (now "SAI Counters Report")
        let has_report_header = output.iter().any(|line| line.contains("SAI Counters Report"));
        assert!(has_report_header, "Should contain counters report header");

        // Verify counter count for all unique counters (first 5 + 2 overlapping = 5 unique)
        let has_counter_count = output.iter().any(|line| line.contains("Total Unique Counters: 5"));
        assert!(has_counter_count, "Should show correct unique counters count");

        // Verify detailed output
        let has_detailed = output.iter().any(|line| line.contains("Detailed Counters:"));
        assert!(has_detailed, "Should show detailed counters");

        // Verify individual counter entries with new format
        let has_counter_entry = output.iter().any(|line| 
            line.contains("Object:") && line.contains("Type:") && line.contains("Msg/s:")
        );
        assert!(has_counter_entry, "Should show individual counter entries with message counts");

        println!("✅ Basic functionality test passed - captured {} output lines", output.len());
    }

    #[tokio::test]
    async fn test_stats_reporter_summary_mode() {
        use std::sync::{Arc, Mutex};

        #[derive(Clone)]
        struct SharedTestWriter {
            lines: Arc<Mutex<Vec<String>>>,
        }

        impl SharedTestWriter {
            fn new() -> Self {
                Self {
                    lines: Arc::new(Mutex::new(Vec::new())),
                }
            }

            fn get_lines(&self) -> Vec<String> {
                self.lines.lock().unwrap().clone()
            }
        }

        impl OutputWriter for SharedTestWriter {
            fn write_line(&mut self, line: &str) {
                self.lines.lock().unwrap().push(line.to_string());
            }
        }

        let (sender, receiver) = channel(10);
        let shared_writer = SharedTestWriter::new();
        let writer_clone = shared_writer.clone();
        
        let config = StatsReporterConfig {
            interval: Duration::from_millis(100),
            detailed: false, // Summary mode
            max_stats_per_report: None,
        };
        
        let actor = StatsReporterActor::new(receiver, config, shared_writer);
        let handle = spawn(StatsReporterActor::run(actor));

        // Send test statistics with known values
        let test_stats = create_test_stats(99999, 3);
        sender.send(Arc::new(test_stats)).await.unwrap();

        // Wait for processing and one report
        sleep(Duration::from_millis(150)).await;

        // Close and finish
        drop(sender);
        handle.await.expect("Actor should complete successfully");

        // Verify captured output
        let output = writer_clone.get_lines();

        // Verify we have output
        assert!(!output.is_empty(), "Should have captured some output");

        // Verify summary mode elements
        let has_summary_header = output.iter().any(|line| line.contains("📊 Summary:"));
        assert!(has_summary_header, "Should contain summary header");

        // Verify total counter calculation (0 + 1000 + 2000 = 3000)
        let has_total_counter = output.iter().any(|line| line.contains("Total Counter Value: 3000"));
        assert!(has_total_counter, "Should show correct total counter value");

        // Verify unique counts
        let has_unique_types = output.iter().any(|line| line.contains("Unique Types: 3"));
        assert!(has_unique_types, "Should show correct unique types count");

        let has_unique_labels = output.iter().any(|line| line.contains("Unique Objects: 3"));
        assert!(has_unique_labels, "Should show correct unique objects count");

        // Should NOT have detailed counters
        let has_detailed = output.iter().any(|line| line.contains("Detailed Counters:"));
        assert!(!has_detailed, "Should NOT show detailed counters in summary mode");

        // Should show messages per second
        let has_messages_per_second = output.iter().any(|line| line.contains("Messages per Second:"));
        assert!(has_messages_per_second, "Should show messages per second in summary mode");

        println!("✅ Summary mode test passed - captured {} output lines", output.len());
    }

    #[tokio::test]
    async fn test_stats_reporter_no_data() {
        use std::sync::{Arc, Mutex};

        #[derive(Clone)]
        struct SharedTestWriter {
            lines: Arc<Mutex<Vec<String>>>,
        }

        impl SharedTestWriter {
            fn new() -> Self {
                Self {
                    lines: Arc::new(Mutex::new(Vec::new())),
                }
            }

            fn get_lines(&self) -> Vec<String> {
                self.lines.lock().unwrap().clone()
            }
        }

        impl OutputWriter for SharedTestWriter {
            fn write_line(&mut self, line: &str) {
                self.lines.lock().unwrap().push(line.to_string());
            }
        }

        let (sender, receiver) = channel(10);
        let shared_writer = SharedTestWriter::new();
        let writer_clone = shared_writer.clone();
        
        let config = StatsReporterConfig {
            interval: Duration::from_millis(50),
            detailed: true,
            max_stats_per_report: None,
        };
        
        let actor = StatsReporterActor::new(receiver, config, shared_writer);
        let handle = spawn(StatsReporterActor::run(actor));

        // Don't send any data, just wait for a report
        sleep(Duration::from_millis(100)).await;

        // Close the channel
        drop(sender);
        handle.await.expect("Actor should complete successfully");

        // Verify captured output
        let output = writer_clone.get_lines();

        // Verify we have output
        assert!(!output.is_empty(), "Should have captured some output");

        // Verify "no data" message
        let has_no_data_msg = output.iter().any(|line| line.contains("No statistics data available yet"));
        assert!(has_no_data_msg, "Should show 'no data available' message");

        // Verify message count is 0
        let has_zero_messages = output.iter().any(|line| line.contains("Total Messages Received: 0"));
        assert!(has_zero_messages, "Should show 0 total messages received");

        println!("✅ No data test passed - captured {} output lines", output.len());
    }

    #[tokio::test]
    async fn test_stats_reporter_max_stats_limit() {
        use std::sync::{Arc, Mutex};

        #[derive(Clone)]
        struct SharedTestWriter {
            lines: Arc<Mutex<Vec<String>>>,
        }

        impl SharedTestWriter {
            fn new() -> Self {
                Self {
                    lines: Arc::new(Mutex::new(Vec::new())),
                }
            }

            fn get_lines(&self) -> Vec<String> {
                self.lines.lock().unwrap().clone()
            }
        }

        impl OutputWriter for SharedTestWriter {
            fn write_line(&mut self, line: &str) {
                self.lines.lock().unwrap().push(line.to_string());
            }
        }

        let (sender, receiver) = channel(10);
        let shared_writer = SharedTestWriter::new();
        let writer_clone = shared_writer.clone();
        
        let config = StatsReporterConfig {
            interval: Duration::from_millis(500), // Longer interval to avoid multiple reports
            detailed: true,
            max_stats_per_report: Some(2), // Limit to 2 stats
        };
        
        let actor = StatsReporterActor::new(receiver, config, shared_writer);
        let handle = spawn(StatsReporterActor::run(actor));

        // Send stats with more entries than the limit
        let test_stats = create_test_stats(55555, 5);
        sender.send(Arc::new(test_stats)).await.unwrap();

        // Wait for processing but not long enough for multiple reports
        sleep(Duration::from_millis(50)).await;

        // Close and finish quickly to avoid multiple timer ticks
        drop(sender);
        handle.await.expect("Actor should complete successfully");

        // Verify captured output
        let output = writer_clone.get_lines();

        // Find the first detailed counters section
        let mut in_detailed_section = false;
        let mut counter_entries = Vec::new();
        
        for line in &output {
            if line.contains("📈 Detailed Counters:") {
                in_detailed_section = true;
                continue;
            }
            
            if in_detailed_section {
                if line.contains("] Object:") && line.contains("Type:") {
                    counter_entries.push(line);
                } else if line.contains("📊") || line.trim().is_empty() {
                    // End of this detailed section
                    break;
                }
            }
        }
        
        // Should show exactly 2 counter entries in the first report
        assert_eq!(counter_entries.len(), 2, "Should show exactly 2 counter entries due to limit");

        // Verify "more counters" message
        let has_more_msg = output.iter().any(|line| line.contains("and 3 more counters"));
        assert!(has_more_msg, "Should show 'more counters' message");

        // Verify total count is still correct
        let has_total_count = output.iter().any(|line| line.contains("Total Unique Counters: 5"));
        assert!(has_total_count, "Should show correct total unique counters count");

        println!("✅ Max stats limit test passed - captured {} output lines", output.len());
    }
}
