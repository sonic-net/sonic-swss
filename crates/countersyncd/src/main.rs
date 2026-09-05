// Application modules
mod actor;
mod message;
mod sai;
mod utilities;

// External dependencies
use clap::Parser;
use log::{error, info, warn};
use message::ipfix::RestartRequest;
use opentelemetry::ExportError;
use std::{os::unix::process::CommandExt, process::Command, time::Duration};
use tokio::{spawn, sync::mpsc::channel};

// Internal actor implementations
use crate::actor::{
    control_netlink::ControlNetlinkActor,
    counter_db::{CounterDBActor, CounterDBConfig},
    data_netlink::{get_genl_family_group, DataNetlinkActor},
    ipfix::{is_restart_required, IpfixActor, IpfixError},
    otel::{OtelActor, OtelActorConfig},
    stats_reporter::{ConsoleWriter, StatsReporterActor, StatsReporterConfig},
    swss::{SwssActor, SwssError},
};

// Internal exit codes
use crate::utilities::{set_comm_capacity, set_comm_log_interval_secs, ChannelLabel};
use countersyncd::exit_codes::{EXIT_FAILURE, EXIT_OTEL_EXPORT_RETRIES_EXHAUSTED};

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

#[derive(Debug)]
struct SupervisorExit {
    actor_name: &'static str,
    exit_code: i32,
    message: String,
    restart: Option<RestartRequest>,
}

fn describe_join_error(e: tokio::task::JoinError) -> String {
    if e.is_panic() {
        format!("task panicked: {}", e)
    } else if e.is_cancelled() {
        format!("task was cancelled: {}", e)
    } else {
        format!("task join error: {}", e)
    }
}

fn classify_join(name: &'static str, result: Result<(), tokio::task::JoinError>) -> SupervisorExit {
    match result {
        Ok(()) => {
            // Actors are expected to run indefinitely; a normal return is treated as an unexpected exit.
            SupervisorExit {
                actor_name: name,
                exit_code: EXIT_FAILURE,
                message: "exited unexpectedly".to_string(),
                restart: None,
            }
        }
        Err(e) => SupervisorExit {
            actor_name: name,
            exit_code: EXIT_FAILURE,
            message: describe_join_error(e),
            restart: None,
        },
    }
}

fn classify_otel_join(
    name: &'static str,
    result: Result<Result<(), Box<dyn ExportError>>, tokio::task::JoinError>,
) -> SupervisorExit {
    match result {
        Ok(Ok(())) => {
            // OpenTelemetry is also a long-running actor; a normal return is treated as an unexpected exit.
            SupervisorExit {
                actor_name: name,
                exit_code: EXIT_FAILURE,
                message: "exited unexpectedly".to_string(),
                restart: None,
            }
        }
        Ok(Err(e)) => SupervisorExit {
            actor_name: name,
            exit_code: EXIT_OTEL_EXPORT_RETRIES_EXHAUSTED,
            message: format!("export failed after retries: {:?}", e),
            restart: None,
        },
        Err(e) => SupervisorExit {
            actor_name: name,
            exit_code: EXIT_FAILURE,
            message: describe_join_error(e),
            restart: None,
        },
    }
}

fn classify_ipfix_join(
    name: &'static str,
    result: Result<Result<(), IpfixError>, tokio::task::JoinError>,
) -> SupervisorExit {
    match result {
        Ok(Ok(())) => classify_join(name, Ok(())),
        Ok(Err(e)) => {
            let restart = is_restart_required(&e).then(|| RestartRequest::Failure(e.to_string()));
            SupervisorExit {
                actor_name: name,
                exit_code: EXIT_FAILURE,
                message: e.to_string(),
                restart,
            }
        }
        Err(e) => classify_join(name, Err(e)),
    }
}

fn classify_swss_join(
    name: &'static str,
    result: Result<Result<(), SwssError>, tokio::task::JoinError>,
) -> SupervisorExit {
    match result {
        Ok(Ok(())) => classify_join(name, Ok(())),
        Ok(Err(SwssError::RestartRequired(message))) => SupervisorExit {
            actor_name: name,
            exit_code: EXIT_FAILURE,
            message: message.clone(),
            restart: Some(RestartRequest::Administrative(message)),
        },
        Ok(Err(SwssError::ReaderFailed(message))) => SupervisorExit {
            actor_name: name,
            exit_code: EXIT_FAILURE,
            message,
            restart: None,
        },
        Err(e) => classify_join(name, Err(e)),
    }
}

fn parse_positive_capacity(value: &str) -> Result<usize, String> {
    let capacity = value
        .parse::<usize>()
        .map_err(|e| format!("invalid channel capacity '{value}': {e}"))?;
    if capacity == 0 {
        return Err("channel capacity must be at least 1".to_string());
    }
    Ok(capacity)
}

fn reconcile_restart_intent(
    exit: &mut SupervisorExit,
    receiver: &mut tokio::sync::mpsc::Receiver<RestartRequest>,
) {
    if exit.restart.is_some() {
        return;
    }
    // A notifier may have become ready after recv was polled Pending but
    // before a dependent actor's join completed in the same select poll.
    if let Ok(request) = receiver.try_recv() {
        exit.message = request.message().to_string();
        exit.restart = Some(request);
    }
}

fn otel_failure_exit(message: String) -> SupervisorExit {
    SupervisorExit {
        actor_name: "OpenTelemetry",
        exit_code: EXIT_OTEL_EXPORT_RETRIES_EXHAUSTED,
        message,
        restart: None,
    }
}

fn reconcile_otel_failure(
    exit: &mut SupervisorExit,
    receiver: &mut tokio::sync::mpsc::Receiver<String>,
) {
    if exit.restart.is_none() && exit.exit_code != EXIT_OTEL_EXPORT_RETRIES_EXHAUSTED {
        if let Ok(message) = receiver.try_recv() {
            *exit = otel_failure_exit(message);
        }
    }
}

async fn join_aborted_task<T>(handle: &mut tokio::task::JoinHandle<T>) {
    // Completed joins may already have been consumed by the supervisor select.
    // is_finished also guarantees their futures (and owned guards) were dropped.
    if !handle.is_finished() {
        let _ = handle.await;
    }
}

const SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(10);

struct ShutdownWatchdog {
    cancel: std::sync::mpsc::Sender<()>,
    thread: Option<std::thread::JoinHandle<()>>,
}

impl ShutdownWatchdog {
    fn start(
        timeout: Duration,
        on_timeout: impl FnOnce() + Send + 'static,
    ) -> std::io::Result<Self> {
        let (cancel, receiver) = std::sync::mpsc::channel();
        let started = std::time::Instant::now();
        let thread = std::thread::Builder::new()
            .name("countersyncd-shutdown".into())
            .spawn(move || {
                if matches!(
                    receiver.recv_timeout(timeout.saturating_sub(started.elapsed())),
                    Err(std::sync::mpsc::RecvTimeoutError::Timeout)
                ) {
                    on_timeout();
                }
            })?;
        Ok(Self {
            cancel,
            thread: Some(thread),
        })
    }
}

impl Drop for ShutdownWatchdog {
    fn drop(&mut self) {
        let _ = self.cancel.send(());
        if let Some(thread) = self.thread.take() {
            // Do not leave a watchdog callback/thread racing the FD sweep or exec.
            let _ = thread.join();
        }
    }
}

fn restart_backoff(request: &RestartRequest, previous_retries: u32) -> (u32, u64) {
    match request {
        RestartRequest::Administrative(_) => (0, 0),
        RestartRequest::Failure(_) => {
            let retries = previous_retries.saturating_add(1);
            (retries, 1u64 << retries.saturating_sub(1).min(6))
        }
    }
}

fn set_close_on_exec(
    fd: libc::c_int,
    mut fcntl: impl FnMut(libc::c_int, libc::c_int, libc::c_int) -> std::io::Result<libc::c_int>,
) -> std::io::Result<()> {
    let flags = match fcntl(fd, libc::F_GETFD, 0) {
        Ok(flags) => flags,
        Err(error) if error.raw_os_error() == Some(libc::EBADF) => return Ok(()),
        Err(error) => return Err(error),
    };
    match fcntl(fd, libc::F_SETFD, flags | libc::FD_CLOEXEC) {
        Ok(_) => Ok(()),
        Err(error) if error.raw_os_error() == Some(libc::EBADF) => Ok(()),
        Err(error) => Err(error),
    }
}

const MAX_BATCH_CHANNEL_CAPACITY: usize = 64;

fn clamp_batch_capacity(option: &str, capacity: usize) -> usize {
    if capacity > MAX_BATCH_CHANNEL_CAPACITY {
        warn!(
            "{option}={capacity} exceeds the effective maximum {MAX_BATCH_CHANNEL_CAPACITY}; clamping for bounded batch memory"
        );
        MAX_BATCH_CHANNEL_CAPACITY
    } else {
        capacity
    }
}

fn mark_open_fds_close_on_exec() -> std::io::Result<()> {
    for entry in std::fs::read_dir("/proc/self/fd")? {
        let entry = entry?;
        let Ok(fd) = entry.file_name().to_string_lossy().parse::<libc::c_int>() else {
            continue;
        };
        if fd < 3 {
            continue;
        }
        set_close_on_exec(fd, |fd, operation, argument| {
            // fcntl does not transfer ownership; a concurrent close is benign.
            let result = unsafe { libc::fcntl(fd, operation, argument) };
            if result < 0 {
                Err(std::io::Error::last_os_error())
            } else {
                Ok(result)
            }
        })?;
    }
    Ok(())
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

    /// Interval (seconds) between periodic comm stats log lines (channel queue stats)
    #[arg(
        long,
        default_value = "600",
        value_parser = clap::value_parser!(u64).range(1..),
        help = "Interval in seconds for logging comm stats (channel lengths). Use a shorter value (e.g. 60) when verifying HFT processing slowness. Minimum 1"
    )]
    comm_stats_interval: u64,

    /// Netlink socket receive buffer size in bytes (0 = OS default). Increase to reduce ENOBUFS under high HFT load.
    #[arg(
        long,
        default_value = "4194304",
        help = "Netlink SO_RCVBUF size in bytes (0 = default). Use 4MB or higher if you see 'Netlink receive buffer full (ENOBUFS)'"
    )]
    netlink_rcvbuf: usize,

    /// Deprecated compatibility option. Netlink reads are now driven by fd readiness.
    #[arg(
        long,
        value_parser = clap::value_parser!(u64).range(1..),
        help = "Deprecated and ignored; netlink reads are event-driven"
    )]
    socket_readiness_timeout_ms: Option<u64>,

    /// Channel capacity for data_netlink to ipfix communication (IPFIX records)
    #[arg(
        long,
        default_value = "1024",
        value_parser = parse_positive_capacity,
        help = "Set the channel capacity for IPFIX records from data_netlink to ipfix actor"
    )]
    data_netlink_capacity: usize,

    /// Channel capacity for stats_reporter communication  
    #[arg(
        long,
        default_value = "32",
        value_parser = parse_positive_capacity,
        help = "Set the SAI stats batch channel capacity for stats_reporter; values above 64 are accepted and clamped to 64"
    )]
    stats_reporter_capacity: usize,

    /// Channel capacity for counter_db communication  
    #[arg(
        long,
        default_value = "32",
        value_parser = parse_positive_capacity,
        help = "Set the SAI stats batch channel capacity for counter_db; values above 64 are accepted and clamped to 64"
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

    /// Channel capacity for otel communication
    #[arg(
        long,
        default_value = "32",
        value_parser = parse_positive_capacity,
        help = "Set the SAI stats batch channel capacity for otel; values above 64 are accepted and clamped to 64"
    )]
    otel_capacity: usize,

    /// Max counters to batch before exporting to OTLP
    #[arg(
        long,
        default_value = "10000",
        help = "Max counters to accumulate before forcing an OTLP export"
    )]
    otel_max_counters_per_export: usize,

    /// Flush timeout for OTLP export in milliseconds
    #[arg(
        long,
        default_value = "1000",
        help = "Flush timeout (ms) for OTLP export batch"
    )]
    otel_flush_timeout_ms: u64,
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Parse command line arguments
    let mut args = Args::parse();

    // Initialize logging based on command line arguments
    init_logging(&args.log_level, &args.log_format);
    args.stats_reporter_capacity =
        clamp_batch_capacity("--stats-reporter-capacity", args.stats_reporter_capacity);
    args.counter_db_capacity =
        clamp_batch_capacity("--counter-db-capacity", args.counter_db_capacity);
    args.otel_capacity = clamp_batch_capacity("--otel-capacity", args.otel_capacity);

    if let Some(value) = args.socket_readiness_timeout_ms {
        warn!(
            "--socket-readiness-timeout-ms={} is deprecated and ignored; netlink reads are event-driven",
            value
        );
    }

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
        info!(
            "OpenTelemetry batching: max_counters_per_export={}, flush_timeout_ms={}",
            args.otel_max_counters_per_export, args.otel_flush_timeout_ms
        );
    }
    info!(
        "Comm stats log interval: {} seconds",
        args.comm_stats_interval
    );
    info!("Netlink data socket uses event-driven readiness notifications");
    info!(
        "Channel capacities - ipfix_records: {}, stats_reporter: {}, counter_db: {}, otel: {}",
        args.data_netlink_capacity,
        args.stats_reporter_capacity,
        args.counter_db_capacity,
        args.otel_capacity
    );

    set_comm_log_interval_secs(args.comm_stats_interval);

    // Create communication channels between actors with configurable capacities
    let (command_sender, command_receiver) = channel(10); // Keep small buffer for commands
    let (ipfix_record_sender, ipfix_record_receiver) = channel(args.data_netlink_capacity);
    let (ipfix_template_sender, ipfix_template_receiver) = channel(10); // Fixed capacity for templates
    let (stats_report_sender, stats_report_receiver) = channel(args.stats_reporter_capacity);
    let (counter_db_sender, counter_db_receiver) = channel(args.counter_db_capacity);
    let (otel_sender, otel_receiver) = channel(args.otel_capacity);
    let (otel_shutdown_sender, _otel_shutdown_receiver) = tokio::sync::oneshot::channel();

    set_comm_capacity(ChannelLabel::ControlNetlinkToDataNetlink, 10);
    set_comm_capacity(
        ChannelLabel::DataNetlinkToIpfixRecords,
        args.data_netlink_capacity,
    );
    set_comm_capacity(ChannelLabel::SwssToIpfixTemplates, 10);
    set_comm_capacity(
        ChannelLabel::IpfixToStatsReporter,
        args.stats_reporter_capacity,
    );
    set_comm_capacity(ChannelLabel::IpfixToCounterDb, args.counter_db_capacity);
    set_comm_capacity(ChannelLabel::IpfixToOtel, args.otel_capacity);

    // Get netlink family and group configuration from SONiC constants
    let (family, group) = get_genl_family_group();
    info!("Using netlink family: '{}', group: '{}'", family, group);

    // Initialize and configure actors
    let mut data_netlink = DataNetlinkActor::new(
        family.as_str(),
        group.as_str(),
        command_receiver,
        args.netlink_rcvbuf,
    );
    data_netlink.add_recipient(ipfix_record_sender);

    let control_netlink = ControlNetlinkActor::new(family.as_str(), group.as_str(), command_sender);

    let mut ipfix = IpfixActor::new(ipfix_template_receiver, ipfix_record_receiver);
    let (restart_sender, mut restart_receiver) = channel::<RestartRequest>(1);
    ipfix.set_restart_notifier(restart_sender.clone());

    // Initialize SwssActor to monitor SONiC orchestrator messages
    let swss = match SwssActor::new(ipfix_template_sender) {
        Ok(mut actor) => {
            actor.set_restart_notifier(restart_sender);
            actor
        }
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
    let (otel_failure_sender, mut otel_failure_receiver) = channel(1);
    let otel_actor = if args.enable_otel {
        let otel_config = OtelActorConfig {
            collector_endpoint: args.otel_endpoint.clone(),
            max_counters_per_export: args.otel_max_counters_per_export,
            flush_timeout: std::time::Duration::from_millis(args.otel_flush_timeout_ms),
        };

        // Add OTEL to ipfix recipients only when enabled
        ipfix.add_recipient(otel_sender.clone());
        match OtelActor::new(otel_receiver, otel_config, otel_shutdown_sender).await {
            Ok(mut actor) => {
                actor.set_failure_notifier(otel_failure_sender);
                Some(actor)
            }
            Err(e) => {
                error!("Failed to initialize OtelActor: {}", e);
                return Err(e.into());
            }
        }
    } else {
        // Drop the receiver if OTEL export is disabled
        drop(otel_receiver);
        drop(otel_shutdown_sender);
        drop(otel_failure_sender);
        None
    };

    info!("Starting actor tasks...");

    // Spawn actor tasks
    let mut data_netlink_handle = spawn(async move {
        info!("Data netlink actor started");
        DataNetlinkActor::run(data_netlink).await;
        info!("Data netlink actor terminated");
    });

    let mut control_netlink_handle = spawn(async move {
        info!("Control netlink actor started");
        ControlNetlinkActor::run(control_netlink).await;
        info!("Control netlink actor terminated");
    });

    let mut ipfix_handle = spawn(async move {
        info!("IPFIX actor started");
        let result = IpfixActor::run(ipfix).await;
        info!("IPFIX actor terminated");
        result
    });

    let mut swss_handle = spawn(async move {
        info!("SWSS actor started");
        let result = SwssActor::run(swss).await;
        info!("SWSS actor terminated");
        result
    });

    // Only spawn stats reporter if enabled
    let mut reporter_handle = if let Some(stats_reporter) = stats_reporter {
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
    let mut counter_db_handle = if let Some(counter_db) = counter_db {
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
    let mut otel_handle = if let Some(otel_actor) = otel_actor {
        Some(spawn(async move {
            info!("OpenTelemetry actor started");
            let result = OtelActor::run(otel_actor).await;
            info!("OpenTelemetry actor terminated");
            result
        }))
    } else {
        info!("OpenTelemetry export disabled - not starting OpenTelemetry actor");
        None
    };

    // All actors are treated as critical. If any actor exits, abort the rest and terminate.
    let mut first_exit = tokio::select! {
        biased;
        Some(reason) = restart_receiver.recv() => {
            SupervisorExit {
                actor_name: "HFT lifecycle",
                exit_code: EXIT_FAILURE,
                message: reason.message().to_string(),
                restart: Some(reason),
            }
        }
        Some(message) = otel_failure_receiver.recv() => {
            otel_failure_exit(message)
        }
        res = &mut swss_handle => {
            classify_swss_join("SWSS", res)
        }
        res = &mut data_netlink_handle => {
            classify_join("Data netlink", res)
        }
        res = &mut control_netlink_handle => {
            classify_join("Control netlink", res)
        }
        res = &mut ipfix_handle => {
            classify_ipfix_join("IPFIX", res)
        }
        res = async { reporter_handle.as_mut().unwrap().await }, if reporter_handle.is_some() => {
            classify_join("Stats reporter", res)
        }
        res = async { counter_db_handle.as_mut().unwrap().await }, if counter_db_handle.is_some() => {
            classify_join("Counter DB", res)
        }
        res = async { otel_handle.as_mut().unwrap().await }, if otel_handle.is_some() => {
            classify_otel_join("OpenTelemetry", res)
        }
    };

    reconcile_restart_intent(&mut first_exit, &mut restart_receiver);
    reconcile_otel_failure(&mut first_exit, &mut otel_failure_receiver);

    error!(
        "Critical actor '{}' triggered daemon shutdown: {}",
        first_exit.actor_name, first_exit.message
    );

    // A synchronous SWSS Redis operation can block its join guard on a runtime
    // worker. A Tokio timeout cannot guarantee progress here. Never exec past an
    // incomplete barrier. A timeout exits even if the deployment does not
    // automatically restart failed services.
    let exit_code = first_exit.exit_code;
    let shutdown_watchdog = match ShutdownWatchdog::start(SHUTDOWN_TIMEOUT, move || {
        error!("Actor shutdown exceeded {SHUTDOWN_TIMEOUT:?}; exiting with status {exit_code} without exec");
        std::process::exit(exit_code);
    }) {
        Ok(watchdog) => watchdog,
        Err(error) => {
            error!("Cannot start shutdown watchdog: {error}; exiting with status {exit_code}");
            std::process::exit(exit_code);
        }
    };

    data_netlink_handle.abort();
    control_netlink_handle.abort();
    ipfix_handle.abort();
    swss_handle.abort();

    if let Some(handle) = reporter_handle.as_mut() {
        handle.abort();
    }
    if let Some(handle) = counter_db_handle.as_mut() {
        handle.abort();
    }
    if let Some(handle) = otel_handle.as_mut() {
        handle.abort();
    }

    // Abort only requests cancellation. Wait for owned sockets and the SWSS
    // reader's stop/join guard before enumerating descriptors for exec.
    join_aborted_task(&mut data_netlink_handle).await;
    join_aborted_task(&mut control_netlink_handle).await;
    join_aborted_task(&mut ipfix_handle).await;
    join_aborted_task(&mut swss_handle).await;
    if let Some(handle) = reporter_handle.as_mut() {
        join_aborted_task(handle).await;
    }
    if let Some(handle) = counter_db_handle.as_mut() {
        join_aborted_task(handle).await;
    }
    if let Some(handle) = otel_handle.as_mut() {
        join_aborted_task(handle).await;
    }
    drop(shutdown_watchdog);

    if let Some(request) = &first_exit.restart {
        warn!("Restarting countersyncd in place to establish a clean HFT generation boundary");
        let started_at = std::env::var("COUNTERSYNCD_STARTED_AT")
            .ok()
            .and_then(|value| value.parse::<u64>().ok());
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs();
        let previous_retries =
            if started_at.is_some_and(|started| now.saturating_sub(started) >= 60) {
                0
            } else {
                std::env::var("COUNTERSYNCD_RESTART_RETRIES")
                    .ok()
                    .and_then(|value| value.parse::<u32>().ok())
                    .unwrap_or(0)
            };
        let (retries, delay) = restart_backoff(request, previous_retries);
        if delay > 0 {
            warn!("Delaying restart by {delay}s after persistent invalid HFT configuration");
            std::thread::sleep(Duration::from_secs(delay));
        }
        mark_open_fds_close_on_exec()?;
        let error = Command::new(std::env::current_exe()?)
            .args(std::env::args_os().skip(1))
            .env("COUNTERSYNCD_RESTART_RETRIES", retries.to_string())
            .env(
                "COUNTERSYNCD_STARTED_AT",
                std::time::SystemTime::now()
                    .duration_since(std::time::UNIX_EPOCH)
                    .unwrap_or_default()
                    .as_secs()
                    .to_string(),
            )
            .exec();
        return Err(error.into());
    }
    std::process::exit(first_exit.exit_code);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(args: &[&str]) -> Result<Args, clap::Error> {
        Args::try_parse_from(args)
    }

    #[tokio::test(flavor = "current_thread")]
    async fn shutdown_watchdog_preserves_status_while_runtime_is_blocked() {
        use std::sync::{
            atomic::{AtomicI32, Ordering},
            mpsc, Arc,
        };

        struct BlockOnDrop {
            entered: mpsc::Sender<()>,
            timeout: mpsc::Receiver<i32>,
            status: Arc<AtomicI32>,
        }
        impl Drop for BlockOnDrop {
            fn drop(&mut self) {
                self.entered.send(()).unwrap();
                // This blocks the only runtime thread, including Tokio timers.
                let status = self.timeout.recv_timeout(Duration::from_secs(2)).unwrap();
                self.status.store(status, Ordering::SeqCst);
            }
        }

        for exit in [
            otel_failure_exit("retry exhaustion".into()),
            classify_swss_join("SWSS", Ok(Err(SwssError::RestartRequired("delete".into())))),
        ] {
            let (entered_sender, entered_receiver) = mpsc::channel();
            let (timeout_sender, timeout_receiver) = mpsc::channel();
            let status = Arc::new(AtomicI32::new(-1));
            let guard = BlockOnDrop {
                entered: entered_sender,
                timeout: timeout_receiver,
                status: status.clone(),
            };
            let (started, ready) = tokio::sync::oneshot::channel();
            let mut task = spawn(async move {
                let _guard = guard;
                started.send(()).unwrap();
                std::future::pending::<()>().await;
            });
            tokio::time::timeout(Duration::from_secs(2), ready)
                .await
                .unwrap()
                .unwrap();
            let expected = exit.exit_code;
            let watchdog = ShutdownWatchdog::start(Duration::from_millis(20), move || {
                entered_receiver
                    .recv_timeout(Duration::from_secs(2))
                    .unwrap();
                timeout_sender.send(expected).unwrap();
            })
            .unwrap();
            task.abort();
            join_aborted_task(&mut task).await;
            drop(watchdog);
            assert_eq!(status.load(Ordering::SeqCst), expected);
        }
    }

    #[test]
    fn shutdown_watchdog_cancellation_joins_without_firing() {
        let (sender, receiver) = std::sync::mpsc::channel();
        let watchdog = ShutdownWatchdog::start(Duration::from_secs(2), move || {
            sender.send(()).unwrap();
        })
        .unwrap();
        drop(watchdog);
        assert_eq!(
            receiver.try_recv(),
            Err(std::sync::mpsc::TryRecvError::Disconnected)
        );
    }

    #[test]
    fn shutdown_barrier_sweep_and_exec_close_late_open_descriptors() {
        use std::io::Read;
        use std::os::fd::AsRawFd;
        use wait_timeout::ChildExt;

        const STAGE: &str = "COUNTERSYNCD_TEST_SHUTDOWN_EXEC_STAGE";
        const TARGET: &str = "COUNTERSYNCD_TEST_SHUTDOWN_EXEC_TARGET";
        const TEST: &str = "tests::shutdown_barrier_sweep_and_exec_close_late_open_descriptors";
        match std::env::var(STAGE).as_deref() {
            Ok("barrier") => {
                let watchdog = ShutdownWatchdog::start(Duration::from_secs(2), || {
                    std::process::exit(99);
                })
                .unwrap();
                let runtime = tokio::runtime::Builder::new_current_thread()
                    .enable_all()
                    .build()
                    .unwrap();
                let mut peer =
                    runtime.block_on(crate::actor::swss::tests::late_open_reader_barrier());
                assert_eq!(
                    peer.read(&mut [0]).unwrap(),
                    0,
                    "reader FD must close before exec"
                );
                drop(watchdog);
                let fd = peer.as_raw_fd();
                let target = std::fs::read_link(format!("/proc/self/fd/{fd}")).unwrap();
                // Retain the late-open peer as well, so exec must exercise the sweep.
                assert_eq!(unsafe { libc::fcntl(fd, libc::F_SETFD, 0) }, 0);
                mark_open_fds_close_on_exec().unwrap();
                let error = Command::new(std::env::current_exe().unwrap())
                    .args(["--exact", TEST, "--nocapture"])
                    .env(STAGE, "probe")
                    .env(TARGET, target)
                    .exec();
                panic!("exec failed: {error}");
            }
            Ok("probe") => {
                let target = std::path::PathBuf::from(std::env::var_os(TARGET).unwrap());
                for entry in std::fs::read_dir("/proc/self/fd").unwrap() {
                    if let Ok(link) = std::fs::read_link(entry.unwrap().path()) {
                        assert_ne!(link, target, "late-open socket survived exec");
                    }
                }
            }
            _ => {
                let mut child = Command::new(std::env::current_exe().unwrap())
                    .args(["--exact", TEST, "--nocapture"])
                    .env(STAGE, "barrier")
                    .spawn()
                    .unwrap();
                let status = child.wait_timeout(Duration::from_secs(10)).unwrap();
                if status.is_none() {
                    let _ = child.kill();
                    let _ = child.wait();
                    panic!("shutdown/exec subprocess timed out");
                }
                assert!(status.unwrap().success());
            }
        }
    }

    #[tokio::test]
    async fn otel_failure_has_priority_over_ready_dependent_exit() {
        let (sender, mut receiver) = channel(1);
        sender.try_send("retry exhaustion".into()).unwrap();
        let exit = tokio::select! {
            biased;
            Some(message) = receiver.recv() => otel_failure_exit(message),
            exit = std::future::ready(classify_join("IPFIX", Ok(()))) => exit,
        };
        assert_eq!(exit.exit_code, EXIT_OTEL_EXPORT_RETRIES_EXHAUSTED);
        assert_eq!(exit.actor_name, "OpenTelemetry");
    }

    #[tokio::test]
    async fn otel_failure_between_select_polls_overrides_dependent_exit() {
        let (sender, mut receiver) = channel(1);
        let dependency = std::future::poll_fn(move |_| {
            sender.try_send("retry exhaustion".into()).unwrap();
            std::task::Poll::Ready(classify_join("IPFIX", Ok(())))
        });
        let mut exit = tokio::select! {
            biased;
            _ = receiver.recv() => panic!("the empty receiver must be polled first"),
            exit = dependency => exit,
        };
        assert_eq!(exit.exit_code, EXIT_FAILURE);
        reconcile_otel_failure(&mut exit, &mut receiver);
        assert_eq!(exit.exit_code, EXIT_OTEL_EXPORT_RETRIES_EXHAUSTED);
        assert_eq!(exit.actor_name, "OpenTelemetry");
        assert!(exit.restart.is_none());
    }

    #[test]
    fn otel_reconciliation_preserves_restart_and_unnotified_exit() {
        let (sender, mut receiver) = channel(1);
        let mut exit = classify_join("IPFIX", Ok(()));
        reconcile_otel_failure(&mut exit, &mut receiver);
        assert_eq!(exit.exit_code, EXIT_FAILURE);
        sender.try_send("retry exhaustion".into()).unwrap();
        exit.restart = Some(RestartRequest::Administrative("delete".into()));
        reconcile_otel_failure(&mut exit, &mut receiver);
        assert!(exit.restart.is_some());
        assert_eq!(exit.exit_code, EXIT_FAILURE);
    }

    #[tokio::test]
    async fn aborted_task_join_waits_for_drop_and_skips_consumed_join() {
        struct NotifyDrop(Option<tokio::sync::oneshot::Sender<()>>);
        impl Drop for NotifyDrop {
            fn drop(&mut self) {
                self.0.take().unwrap().send(()).unwrap();
            }
        }
        let (started, ready) = tokio::sync::oneshot::channel();
        let (dropped, mut drop_receiver) = tokio::sync::oneshot::channel();
        let mut handle = spawn(async move {
            let _guard = NotifyDrop(Some(dropped));
            started.send(()).unwrap();
            std::future::pending::<()>().await;
        });
        ready.await.unwrap();
        handle.abort();
        join_aborted_task(&mut handle).await;
        drop_receiver.try_recv().unwrap();
        join_aborted_task(&mut handle).await;

        let mut completed = spawn(async {});
        (&mut completed).await.unwrap();
        completed.abort();
        join_aborted_task(&mut completed).await;
    }

    #[tokio::test]
    async fn restart_arriving_between_select_polls_overrides_dependent_exit() {
        let (sender, mut receiver) = channel(1);
        let request = RestartRequest::Administrative("delete".into());
        let expected = request.clone();
        let mut dependency = std::future::poll_fn(move |_| {
            sender.try_send(request.clone()).unwrap();
            std::task::Poll::Ready(classify_join("dependent", Ok(())))
        });
        let mut exit = tokio::select! {
            biased;
            _ = receiver.recv() => panic!("the empty receiver must be polled first"),
            exit = &mut dependency => exit,
        };
        assert!(exit.restart.is_none());
        reconcile_restart_intent(&mut exit, &mut receiver);
        assert_eq!(exit.restart, Some(expected));
    }

    #[test]
    fn ordinary_exit_without_restart_intent_is_preserved() {
        let (_sender, mut receiver) = channel(1);
        let mut exit = classify_join("dependent", Ok(()));
        reconcile_restart_intent(&mut exit, &mut receiver);
        assert!(exit.restart.is_none());
    }

    #[test]
    fn selected_restart_cause_is_not_overwritten_by_a_later_request() {
        let (sender, mut receiver) = channel(1);
        sender
            .try_send(RestartRequest::Failure("later failure".into()))
            .unwrap();
        let mut exit =
            classify_swss_join("SWSS", Ok(Err(SwssError::RestartRequired("delete".into()))));
        reconcile_restart_intent(&mut exit, &mut receiver);
        assert_eq!(
            exit.restart,
            Some(RestartRequest::Administrative("delete".into()))
        );
    }

    #[test]
    fn administrative_restarts_never_accumulate_failure_backoff() {
        let request = RestartRequest::Administrative("delete".into());
        for previous in [0, 1, 2, 6, 100, u32::MAX] {
            assert_eq!(restart_backoff(&request, previous), (0, 0));
        }
        let exit = classify_swss_join("SWSS", Ok(Err(SwssError::RestartRequired("delete".into()))));
        assert_eq!(exit.restart, Some(request));
    }

    #[test]
    fn failure_restarts_retain_capped_exponential_backoff() {
        let request = RestartRequest::Failure("invalid config".into());
        for (previous, delay) in [(0, 1), (1, 2), (2, 4), (5, 32), (6, 64), (100, 64)] {
            assert_eq!(restart_backoff(&request, previous), (previous + 1, delay));
        }
    }

    #[test]
    fn close_on_exec_tolerates_concurrent_close_at_either_fcntl() {
        for failed_operation in [libc::F_GETFD, libc::F_SETFD] {
            set_close_on_exec(123, |_, operation, _| {
                if operation == failed_operation {
                    Err(std::io::Error::from_raw_os_error(libc::EBADF))
                } else {
                    Ok(0)
                }
            })
            .unwrap();
        }
    }

    #[test]
    fn close_on_exec_propagates_other_errors_and_preserves_flags() {
        for failed_operation in [libc::F_GETFD, libc::F_SETFD] {
            let error = set_close_on_exec(123, |_, operation, _| {
                if operation == failed_operation {
                    Err(std::io::Error::from_raw_os_error(libc::EPERM))
                } else {
                    Ok(0)
                }
            })
            .unwrap_err();
            assert_eq!(error.raw_os_error(), Some(libc::EPERM));
        }
        let mut calls = 0;
        set_close_on_exec(123, |fd, operation, argument| {
            assert_eq!(fd, 123);
            calls += 1;
            if operation == libc::F_GETFD {
                Ok(0x10)
            } else {
                assert_eq!(operation, libc::F_SETFD);
                assert_eq!(argument, 0x10 | libc::FD_CLOEXEC);
                Ok(0)
            }
        })
        .unwrap();
        assert_eq!(calls, 2);
    }

    #[test]
    fn test_defaults() {
        let args = parse(&["countersyncd"]).unwrap();
        assert_eq!(args.socket_readiness_timeout_ms, None);
        assert_eq!(args.netlink_rcvbuf, 4194304);
        assert_eq!(args.comm_stats_interval, 600);
        assert_eq!(args.stats_interval, 10);
        assert_eq!(args.stats_reporter_capacity, 32);
        assert_eq!(args.counter_db_capacity, 32);
        assert_eq!(args.otel_capacity, 32);
        assert!(!args.enable_stats);
        assert!(!args.enable_counter_db);
        assert!(!args.enable_otel);
    }

    #[test]
    fn test_socket_readiness_timeout_zero_rejected() {
        assert!(parse(&["countersyncd", "--socket-readiness-timeout-ms", "0"]).is_err());
    }

    #[test]
    fn test_deprecated_socket_readiness_timeout_is_accepted() {
        let args = parse(&["countersyncd", "--socket-readiness-timeout-ms", "10"]).unwrap();
        assert_eq!(args.socket_readiness_timeout_ms, Some(10));
    }

    #[test]
    fn test_netlink_rcvbuf_zero_accepted() {
        let args = parse(&["countersyncd", "--netlink-rcvbuf", "0"]).unwrap();
        assert_eq!(args.netlink_rcvbuf, 0);
    }

    #[test]
    fn test_comm_stats_interval_custom() {
        let args = parse(&["countersyncd", "--comm-stats-interval", "60"]).unwrap();
        assert_eq!(args.comm_stats_interval, 60);
    }

    #[test]
    fn test_batch_channel_capacity_legacy_values_are_accepted() {
        for option in [
            "--stats-reporter-capacity",
            "--counter-db-capacity",
            "--otel-capacity",
        ] {
            assert!(parse(&["countersyncd", option, "0"]).is_err());
            assert!(parse(&["countersyncd", option, "65"]).is_ok());
            assert!(parse(&["countersyncd", option, "1024"]).is_ok());
            assert!(parse(&["countersyncd", option, "64"]).is_ok());
        }
        assert!(parse(&["countersyncd", "--data-netlink-capacity", "0"]).is_err());
    }

    #[test]
    fn test_batch_channel_capacity_is_clamped() {
        assert_eq!(clamp_batch_capacity("--test", 1), 1);
        assert_eq!(clamp_batch_capacity("--test", 64), 64);
        assert_eq!(clamp_batch_capacity("--test", 65), 64);
        assert_eq!(clamp_batch_capacity("--test", 1024), 64);
    }

    #[test]
    #[serial_test::serial]
    fn test_restart_marks_open_descriptors_close_on_exec() {
        use std::os::fd::AsRawFd;

        let file = std::fs::File::open("/dev/null").unwrap();
        let fd = file.as_raw_fd();
        unsafe {
            libc::fcntl(fd, libc::F_SETFD, 0);
        }
        mark_open_fds_close_on_exec().unwrap();
        let flags = unsafe { libc::fcntl(fd, libc::F_GETFD) };
        assert_ne!(flags & libc::FD_CLOEXEC, 0);
    }

    #[test]
    fn test_unknown_flag_rejected() {
        assert!(parse(&["countersyncd", "--unknown-flag"]).is_err());
    }
}
