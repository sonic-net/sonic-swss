// Application modules
mod actor;
mod message;
mod sai;

// External dependencies
use clap::Parser;
use log::{error, info};
use std::time::Duration;
use tokio::{spawn, sync::mpsc::channel};

// Internal actor implementations
use crate::actor::{
    control_netlink::ControlNetlinkActor,
    counter_db::{CounterDBActor, CounterDBConfig},
    data_netlink::{get_genl_family_group, DataNetlinkActor},
    ipfix::IpfixActor,
    stats_reporter::{ConsoleWriter, StatsReporterActor, StatsReporterConfig},
    swss::SwssActor,
    otel::{OtelActor, OtelActorConfig},
};

/// Initialize logging based on command line arguments
fn init_logging(log_level: &str, log_format: &str) {
    use env_logger::{Builder, Target, WriteStyle};
    use log::LevelFilter;
    use std::io::Write;

    let level = match log_level.to_lowercase().as_str() {
        "trace" => LevelFilter::Trace,
        "debug" => LevelFilter::Debug,
        "info" => LevelFilter::Info,
        "warn" => LevelFilter::Warn,
        "error" => LevelFilter::Error,
        _ => {
            eprintln!("Invalid log level '{}', using 'info'", log_level);
            LevelFilter::Info
        }
    };

    let mut builder = Builder::new();
    builder.filter_level(level);
    builder.target(Target::Stdout);
    builder.write_style(WriteStyle::Auto);

    match log_format.to_lowercase().as_str() {
        "simple" => {
            builder.format(|buf, record| writeln!(buf, "[{}] {}", record.level(), record.args()));
        }
        "full" => {
            builder.format(|buf, record| {
                writeln!(
                    buf,
                    "[{}] [{}:{}] [{}] {}",
                    chrono::Utc::now().format("%Y-%m-%d %H:%M:%S%.3f"),
                    record.file().unwrap_or("unknown"),
                    record.line().unwrap_or(0),
                    record.level(),
                    record.args()
                )
            });
        }
        _ => {
            eprintln!("Invalid log format '{}', using 'full'", log_format);
            builder.format(|buf, record| {
                writeln!(
                    buf,
                    "[{}] [{}:{}] [{}] {}",
                    chrono::Utc::now().format("%Y-%m-%d %H:%M:%S%.3f"),
                    record.file().unwrap_or("unknown"),
                    record.line().unwrap_or(0),
                    record.level(),
                    record.args()
                )
            });
        }
    }

    builder.init();
}

/// SONiC High Frequency Telemetry Counter Sync Daemon
///
/// This application processes high-frequency telemetry data from SONiC switches,
/// converting netlink messages and SWSS state database updates through IPFIX format to SAI statistics.
///
/// The application consists of six main actors:
/// - DataNetlinkActor: Receives raw netlink messages from the kernel and handles data socket
/// - ControlNetlinkActor: Monitors netlink family registration/unregistration and triggers reconnections
/// - SwssActor: Monitors SONiC orchestrator messages via state database for IPFIX templates
/// - IpfixActor: Processes IPFIX templates and data records to extract SAI stats  
/// - StatsReporterActor: Reports processed statistics to the console
/// - CounterDBActor: Writes processed statistics to the Counter Database in Redis
#[derive(Parser)]
#[command(author, version, about, long_about = None)]
struct Args {
    /// Enable stats reporting to console
    #[arg(short, long, default_value = "false")]
    enable_stats: bool,

    /// Stats reporting interval in seconds
    #[arg(short = 'i', long, default_value = "10")]
    stats_interval: u64,

    /// Show detailed statistics in reports
    #[arg(short = 'd', long, default_value = "true")]
    detailed_stats: bool,

    /// Maximum number of stats per report (0 for unlimited)
    #[arg(short = 'm', long, default_value = "20")]
    max_stats_per_report: u32,

    /// Enable counter database writing
    #[arg(short = 'c', long, default_value = "false")]
    enable_counter_db: bool,

    /// Counter database write frequency in seconds
    #[arg(short = 'f', long, default_value = "3")]
    counter_db_frequency: u64,

    /// Log level (trace, debug, info, warn, error)
    #[arg(
        short = 'l',
        long,
        default_value = "info",
        help = "Set the logging level"
    )]
    log_level: String,

    /// Log format (simple, full)
    #[arg(
        long,
        default_value = "full",
        help = "Set the log output format: 'simple' for level and message only, 'full' for timestamp, file, line, level, and message"
    )]
    log_format: String,

    /// Channel capacity for data_netlink to ipfix communication (IPFIX records)
    #[arg(
        long,
        default_value = "1024",
        help = "Set the channel capacity for IPFIX records from data_netlink to ipfix actor"
    )]
    data_netlink_capacity: usize,

    /// Channel capacity for stats_reporter communication  
    #[arg(
        long,
        default_value = "1024",
        help = "Set the channel capacity for stats_reporter actor"
    )]
    stats_reporter_capacity: usize,

    /// Channel capacity for counter_db communication  
    #[arg(
        long,
        default_value = "1024",
        help = "Set the channel capacity for counter_db actor"
    )]
    counter_db_capacity: usize,

    /// Enable OpenTelemetry metrics export
    #[arg(short = 'o', long, default_value = "false")]
    enable_otel: bool,

    /// OpenTelemetry collector endpoint
    #[arg(
        long,
        default_value = "http://localhost:4317",
        help = "OpenTelemetry collector endpoint URL"
    )]
    otel_endpoint: String,

    /// Enable OpenTelemetry console output
    #[arg(
        long,
        default_value = "true",
        help = "Print OpenTelemetry metrics to console"
    )]
    otel_console: bool,

    /// Channel capacity for otel communication
    #[arg(
        long,
        default_value = "1024",
        help = "Set the channel capacity for otel actor"
    )]
    otel_capacity: usize,
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Parse command line arguments
    let args = Args::parse();

    // Initialize logging based on command line arguments
    init_logging(&args.log_level, &args.log_format);

    info!("Starting SONiC High Frequency Telemetry Counter Sync Daemon");
    info!("Stats reporting enabled: {}", args.enable_stats);
    if args.enable_stats {
        info!("Stats reporting interval: {} seconds", args.stats_interval);
        info!("Detailed stats: {}", args.detailed_stats);
        info!("Max stats per report: {}", args.max_stats_per_report);
    }
    info!("Counter DB writing enabled: {}", args.enable_counter_db);
    if args.enable_counter_db {
        info!(
            "Counter DB write frequency: {} seconds",
            args.counter_db_frequency
        );
    }
    info!("OpenTelemetry export enabled: {}", args.enable_otel);
    if args.enable_otel {
        info!("OpenTelemetry endpoint: {}", args.otel_endpoint);
        info!("OpenTelemetry console output: {}", args.otel_console);
    }
    info!(
        "Channel capacities - ipfix_records: {}, stats_reporter: {}, counter_db: {}, otel: {}",
        args.data_netlink_capacity, args.stats_reporter_capacity, args.counter_db_capacity, args.otel_capacity
    );

    // Create communication channels between actors with configurable capacities
    let (command_sender, command_receiver) = channel(10); // Keep small buffer for commands
    let (ipfix_record_sender, ipfix_record_receiver) = channel(args.data_netlink_capacity);
    let (ipfix_template_sender, ipfix_template_receiver) = channel(10); // Fixed capacity for templates
    let (stats_report_sender, stats_report_receiver) = channel(args.stats_reporter_capacity);
    let (counter_db_sender, counter_db_receiver) = channel(args.counter_db_capacity);
    let (otel_sender, otel_receiver) = channel(args.otel_capacity);
    let (otel_shutdown_sender, otel_shutdown_receiver) = tokio::sync::oneshot::channel();

    // Get netlink family and group configuration from SONiC constants
    let (family, group) = get_genl_family_group();
    info!("Using netlink family: '{}', group: '{}'", family, group);

    // Initialize and configure actors
    let mut data_netlink = DataNetlinkActor::new(family.as_str(), group.as_str(), command_receiver);
    data_netlink.add_recipient(ipfix_record_sender);

    let control_netlink = ControlNetlinkActor::new(family.as_str(), command_sender);

    let mut ipfix = IpfixActor::new(ipfix_template_receiver, ipfix_record_receiver);

    // Initialize SwssActor to monitor SONiC orchestrator messages
    let swss = match SwssActor::new(ipfix_template_sender) {
        Ok(actor) => actor,
        Err(e) => {
            error!("Failed to initialize SwssActor: {}", e);
            return Err(e.into());
        }
    };

    // Configure stats reporter with settings from command line arguments
    let stats_reporter = if args.enable_stats {
        let reporter_config = StatsReporterConfig {
            interval: Duration::from_secs(args.stats_interval),
            detailed: args.detailed_stats,
            max_stats_per_report: if args.max_stats_per_report == 0 {
                None
            } else {
                Some(args.max_stats_per_report as usize)
            },
        };

        // Add stats reporter to ipfix recipients only when enabled
        ipfix.add_recipient(stats_report_sender.clone());
        Some(StatsReporterActor::new(
            stats_report_receiver,
            reporter_config,
            ConsoleWriter,
        ))
    } else {
        // Drop the receiver if stats reporting is disabled
        drop(stats_report_receiver);
        None
    };

    // Configure counter database writer with settings from command line arguments
    let counter_db = if args.enable_counter_db {
        let counter_db_config = CounterDBConfig {
            interval: Duration::from_secs(args.counter_db_frequency),
        };

        // Add counter DB to ipfix recipients only when enabled
        ipfix.add_recipient(counter_db_sender.clone());
        match CounterDBActor::new(counter_db_receiver, counter_db_config) {
            Ok(actor) => Some(actor),
            Err(e) => {
                error!("Failed to initialize CounterDBActor: {}", e);
                return Err(e.into());
            }
        }
    } else {
        // Drop the receiver if counter DB writing is disabled
        drop(counter_db_receiver);
        None
    };

    // Configure OpenTelemetry export with settings from command line arguments
    let otel_actor = if args.enable_otel {
        let otel_config = OtelActorConfig {
            print_to_console: args.otel_console,
            collector_endpoint: args.otel_endpoint.clone(),
        };

        // Add OTEL to ipfix recipients only when enabled
        ipfix.add_recipient(otel_sender.clone());
        match OtelActor::new(otel_receiver, otel_config, otel_shutdown_sender).await {
            Ok(actor) => Some(actor),
            Err(e) => {
                error!("Failed to initialize OtelActor: {}", e);
                return Err(e.into());
            }
        }
    } else {
        // Drop the receiver if OTEL export is disabled
        drop(otel_receiver);
        drop(otel_shutdown_sender);
        None
    };

    info!("Starting actor tasks...");

    // Spawn actor tasks
    let data_netlink_handle = spawn(async move {
        info!("Data netlink actor started");
        DataNetlinkActor::run(data_netlink).await;
        info!("Data netlink actor terminated");
    });

    let control_netlink_handle = spawn(async move {
        info!("Control netlink actor started");
        ControlNetlinkActor::run(control_netlink).await;
        info!("Control netlink actor terminated");
    });

    // Use spawn_blocking to ensure IPFIX actor runs on a dedicated thread
    // This is important for thread-local variables
    let ipfix_handle = tokio::task::spawn_blocking(move || {
        info!("IPFIX actor started on dedicated thread");
        // Create a new runtime for async operations within this blocking thread
        let rt = tokio::runtime::Runtime::new().expect("Failed to create runtime for IPFIX actor");
        rt.block_on(async move {
            IpfixActor::run(ipfix).await;
        });
        info!("IPFIX actor terminated");
    });

    let swss_handle = spawn(async move {
        info!("SWSS actor started");
        SwssActor::run(swss).await;
        info!("SWSS actor terminated");
    });

    // Only spawn stats reporter if enabled
    let reporter_handle = if let Some(stats_reporter) = stats_reporter {
        Some(spawn(async move {
            info!("Stats reporter actor started");
            StatsReporterActor::run(stats_reporter).await;
            info!("Stats reporter actor terminated");
        }))
    } else {
        info!("Stats reporting disabled - not starting stats reporter actor");
        None
    };

    // Only spawn counter DB writer if enabled
    let counter_db_handle = if let Some(counter_db) = counter_db {
        Some(spawn(async move {
            info!("Counter DB actor started");
            CounterDBActor::run(counter_db).await;
            info!("Counter DB actor terminated");
        }))
    } else {
        info!("Counter DB writing disabled - not starting counter DB actor");
        None
    };

    // Only spawn OpenTelemetry actor if enabled
    let otel_handle = if let Some(otel_actor) = otel_actor {
        Some(spawn(async move {
            info!("OpenTelemetry actor started");
            OtelActor::run(otel_actor).await;
            info!("OpenTelemetry actor terminated");
        }))
    } else {
        info!("OpenTelemetry export disabled - not starting OpenTelemetry actor");
        None
    };

    // Wait for all actors to complete and handle any errors
    let data_netlink_result = data_netlink_handle.await;
    let control_netlink_result = control_netlink_handle.await;
    let ipfix_result = ipfix_handle.await.map_err(|e| {
        error!("IPFIX blocking task join error: {:?}", e);
        e
    });
    let swss_result = swss_handle.await;
    let reporter_result = if let Some(handle) = reporter_handle {
        Some(handle.await)
    } else {
        None
    };
    let counter_db_result = if let Some(handle) = counter_db_handle {
        Some(handle.await)
    } else {
        None
    };
    let otel_result = if let Some(handle) = otel_handle {
        Some(handle.await)
    } else {
        None
    };

    // Handle results based on what actors were enabled
    let all_successful = match (reporter_result.is_some(), counter_db_result.is_some(), otel_result.is_some()) {
        (true, true, true) => {
            // All optional actors enabled
            matches!(
                (
                    &data_netlink_result,
                    &control_netlink_result,
                    &ipfix_result,
                    &swss_result,
                    reporter_result.as_ref().unwrap(),
                    counter_db_result.as_ref().unwrap(),
                    otel_result.as_ref().unwrap()
                ),
                (Ok(()), Ok(()), Ok(()), Ok(()), Ok(()), Ok(()), Ok(()))
            )
        }
        (true, true, false) => {
            // Stats reporter and counter DB enabled, OTEL disabled
            matches!(
                (
                    &data_netlink_result,
                    &control_netlink_result,
                    &ipfix_result,
                    &swss_result,
                    reporter_result.as_ref().unwrap(),
                    counter_db_result.as_ref().unwrap()
                ),
                (Ok(()), Ok(()), Ok(()), Ok(()), Ok(()), Ok(()))
            )
        }
        (true, false, true) => {
            // Stats reporter and OTEL enabled, counter DB disabled
            matches!(
                (
                    &data_netlink_result,
                    &control_netlink_result,
                    &ipfix_result,
                    &swss_result,
                    reporter_result.as_ref().unwrap(),
                    otel_result.as_ref().unwrap()
                ),
                (Ok(()), Ok(()), Ok(()), Ok(()), Ok(()), Ok(()))
            )
        }
        (false, true, true) => {
            // Counter DB and OTEL enabled, stats reporter disabled
            matches!(
                (
                    &data_netlink_result,
                    &control_netlink_result,
                    &ipfix_result,
                    &swss_result,
                    counter_db_result.as_ref().unwrap(),
                    otel_result.as_ref().unwrap()
                ),
                (Ok(()), Ok(()), Ok(()), Ok(()), Ok(()), Ok(()))
            )
        }
        (true, false, false) => {
            // Only stats reporter enabled
            matches!(
                (
                    &data_netlink_result,
                    &control_netlink_result,
                    &ipfix_result,
                    &swss_result,
                    reporter_result.as_ref().unwrap()
                ),
                (Ok(()), Ok(()), Ok(()), Ok(()), Ok(()))
            )
        }
        (false, true, false) => {
            // Only counter DB enabled
            matches!(
                (
                    &data_netlink_result,
                    &control_netlink_result,
                    &ipfix_result,
                    &swss_result,
                    counter_db_result.as_ref().unwrap()
                ),
                (Ok(()), Ok(()), Ok(()), Ok(()), Ok(()))
            )
        }
        (false, false, true) => {
            // Only OTEL enabled
            matches!(
                (
                    &data_netlink_result,
                    &control_netlink_result,
                    &ipfix_result,
                    &swss_result,
                    otel_result.as_ref().unwrap()
                ),
                (Ok(()), Ok(()), Ok(()), Ok(()), Ok(()))
            )
        }
        (false, false, false) => {
            // None of the optional actors enabled
            matches!(
                (
                    &data_netlink_result,
                    &control_netlink_result,
                    &ipfix_result,
                    &swss_result
                ),
                (Ok(()), Ok(()), Ok(()), Ok(()))
            )
        }
    };

    if all_successful {
        let status_msg = match (reporter_result.is_some(), counter_db_result.is_some(), otel_result.is_some()) {
            (true, true, true) => "All actors completed successfully",
            (true, true, false) => "All actors completed successfully (OpenTelemetry disabled)",
            (true, false, true) => "All actors completed successfully (counter DB disabled)",
            (false, true, true) => "All actors completed successfully (stats reporting disabled)",
            (true, false, false) => "All actors completed successfully (counter DB and OpenTelemetry disabled)",
            (false, true, false) => "All actors completed successfully (stats reporting and OpenTelemetry disabled)",
            (false, false, true) => "All actors completed successfully (stats reporting and counter DB disabled)",
            (false, false, false) => {
                "All actors completed successfully (stats reporting, counter DB, and OpenTelemetry disabled)"
            }
        };
        info!("{}", status_msg);
        Ok(())
    } else {
        // Check which actor failed
        if let Err(e) = data_netlink_result {
            error!("Data netlink actor failed: {:?}", e);
            Err(e.into())
        } else if let Err(e) = control_netlink_result {
            error!("Control netlink actor failed: {:?}", e);
            Err(e.into())
        } else if let Err(e) = ipfix_result {
            error!("IPFIX actor failed: {:?}", e);
            Err(e.into())
        } else if let Err(e) = swss_result {
            error!("SWSS actor failed: {:?}", e);
            Err(e.into())
        } else if let Some(Err(e)) = reporter_result {
            error!("Stats reporter actor failed: {:?}", e);
            Err(e.into())
        } else if let Some(Err(e)) = counter_db_result {
            error!("Counter DB actor failed: {:?}", e);
            Err(e.into())
        } else if let Some(Err(e)) = otel_result {
            error!("OpenTelemetry actor failed: {:?}", e);
            Err(e.into())
        } else {
            error!("Unknown actor failure");
            Err("Unknown actor failure".into())
        }
    }
}
