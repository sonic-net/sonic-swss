use std::time::Duration;

use log::{debug, info};
use tokio::{
    select,
    sync::mpsc::Receiver,
    time::{interval, Interval},
};

use super::super::message::saistats::{SAIStats, SAIStatsMessage};

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
/// - Maintaining the latest statistics state
/// - Periodic reporting based on configured interval
/// - Formatted output to terminal with optional detail levels
pub struct StatsReporterActor<W: OutputWriter> {
    /// Channel for receiving SAI statistics messages
    stats_receiver: Receiver<SAIStatsMessage>,
    /// Configuration for reporting behavior
    config: StatsReporterConfig,
    /// Timer for periodic reporting
    report_timer: Interval,
    /// Latest received statistics (None if no data received yet)
    latest_stats: Option<SAIStats>,
    /// Counter for total messages received
    messages_received: u64,
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
            latest_stats: None,
            messages_received: 0,
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
    /// # Arguments
    /// 
    /// * `stats` - New SAI statistics message to process
    fn update_stats(&mut self, stats: SAIStatsMessage) {
        self.messages_received += 1;
        
        // Convert Arc<SAIStats> to SAIStats for storage
        match std::sync::Arc::try_unwrap(stats) {
            Ok(stats) => {
                debug!("Received SAI stats with {} entries, observation_time: {}", 
                       stats.stats.len(), stats.observation_time);
                self.latest_stats = Some(stats);
            }
            Err(arc_stats) => {
                // If Arc::try_unwrap fails, clone the data
                debug!("Cloning SAI stats due to shared ownership");
                self.latest_stats = Some((*arc_stats).clone());
            }
        }
    }

    /// Generates and prints a statistics report to the terminal.
    fn generate_report(&mut self) {
        self.reports_generated += 1;

        if let Some(stats) = self.latest_stats.clone() {
            self.print_stats_report(&stats);
        } else {
            self.writer.write_line(&format!("📊 [Report #{}] No statistics data available yet", self.reports_generated));
            self.writer.write_line(&format!("   Messages received: {}", self.messages_received));
        }
        
        self.writer.write_line(""); // Add blank line for readability
    }

    /// Prints formatted statistics report to terminal.
    /// 
    /// # Arguments
    /// 
    /// * `stats` - SAI statistics to display
    fn print_stats_report(&mut self, stats: &SAIStats) {
        self.writer.write_line(&format!("📊 [Report #{}] SAI Statistics Report", self.reports_generated));
        self.writer.write_line(&format!("   Timestamp: {} (observation time)", stats.observation_time));
        self.writer.write_line(&format!("   Total Statistics: {}", stats.stats.len()));
        self.writer.write_line(&format!("   Messages Received: {}", self.messages_received));

        if self.config.detailed && !stats.stats.is_empty() {
            self.writer.write_line("   📈 Detailed Statistics:");
            
            let stats_to_show = if let Some(max) = self.config.max_stats_per_report {
                &stats.stats[..std::cmp::min(max, stats.stats.len())]
            } else {
                &stats.stats
            };

            for (index, stat) in stats_to_show.iter().enumerate() {
                self.writer.write_line(&format!(
                    "      [{:3}] Object: {:15}, Type: {:10}, Stat: {:10}, Counter: {:15}",
                    index + 1,
                    stat.object_name,
                    stat.type_id,
                    stat.stat_id,
                    stat.counter
                ));
            }

            if let Some(max) = self.config.max_stats_per_report {
                if stats.stats.len() > max {
                    self.writer.write_line(&format!(
                        "      ... and {} more statistics (use max_stats_per_report: None to show all)",
                        stats.stats.len() - max
                    ));
                }
            }
        } else if !self.config.detailed && !stats.stats.is_empty() {
            // Summary mode - show some aggregate information
            let total_counter: u64 = stats.stats.iter().map(|s| s.counter).sum();
            let unique_types = stats.stats.iter().map(|s| s.type_id).collect::<std::collections::HashSet<_>>().len();
            let unique_labels = stats.stats.iter().map(|s| &s.object_name).collect::<std::collections::HashSet<_>>().len();
            
            self.writer.write_line("   📊 Summary:");
            self.writer.write_line(&format!("      Total Counter Value: {}", total_counter));
            self.writer.write_line(&format!("      Unique Types: {}", unique_types));
            self.writer.write_line(&format!("      Unique Objects: {}", unique_labels));
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
              self.reports_generated, self.messages_received);
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
        
        // Verify report header is present
        let has_report_header = output.iter().any(|line| line.contains("SAI Statistics Report"));
        assert!(has_report_header, "Should contain report header");

        // Verify latest timestamp is present
        let has_latest_timestamp = output.iter().any(|line| line.contains("Timestamp: 67890"));
        assert!(has_latest_timestamp, "Should contain the latest timestamp");

        // Verify statistics count for latest data
        let has_stats_count = output.iter().any(|line| line.contains("Total Statistics: 2"));
        assert!(has_stats_count, "Should show correct stats count for latest data");

        // Verify detailed output
        let has_detailed = output.iter().any(|line| line.contains("Detailed Statistics:"));
        assert!(has_detailed, "Should show detailed statistics");

        // Verify individual stat entries (now looking for "Object:" instead of "Label:")
        let has_stat_entry = output.iter().any(|line| line.contains("Object:") && line.contains("Type:"));
        assert!(has_stat_entry, "Should show individual stat entries");

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

        // Should NOT have detailed statistics
        let has_detailed = output.iter().any(|line| line.contains("Detailed Statistics:"));
        assert!(!has_detailed, "Should NOT show detailed statistics in summary mode");

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
        let has_zero_messages = output.iter().any(|line| line.contains("Messages received: 0"));
        assert!(has_zero_messages, "Should show 0 messages received");

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

        // Find the first detailed statistics section
        let mut in_detailed_section = false;
        let mut stat_entries = Vec::new();
        
        for line in &output {
            if line.contains("📈 Detailed Statistics:") {
                in_detailed_section = true;
                continue;
            }
            
            if in_detailed_section {
                if line.contains("] Object:") && line.contains("Type:") {
                    stat_entries.push(line);
                } else if line.contains("📊") || line.trim().is_empty() {
                    // End of this detailed section
                    break;
                }
            }
        }
        
        // Should show exactly 2 stat entries in the first report
        assert_eq!(stat_entries.len(), 2, "Should show exactly 2 stat entries due to limit");

        // Verify "more statistics" message
        let has_more_msg = output.iter().any(|line| line.contains("and 3 more statistics"));
        assert!(has_more_msg, "Should show 'more statistics' message");

        // Verify total count is still correct
        let has_total_count = output.iter().any(|line| line.contains("Total Statistics: 5"));
        assert!(has_total_count, "Should show correct total count");

        println!("✅ Max stats limit test passed - captured {} output lines", output.len());
    }
}
