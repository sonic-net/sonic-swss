// Application modules
mod message;
mod actor;

// External dependencies
use log::{error, info};
use std::time::Duration;
use tokio::{spawn, sync::mpsc::channel};

// Internal actor implementations
use actor::{
    netlink::{NetlinkActor, get_genl_family_group}, 
    ipfix::IpfixActor,
    stats_reporter::{StatsReporterActor, StatsReporterConfig, ConsoleWriter},
};

/// SONiC High Frequency Telemetry Counter Sync Daemon
/// 
/// This application processes high-frequency telemetry data from SONiC switches,
/// converting netlink messages through IPFIX format to SAI statistics.
/// 
/// The application consists of two main actors:
/// - NetlinkActor: Receives raw netlink messages from the kernel
/// - IpfixActor: Processes IPFIX templates and data records to extract SAI stats

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Initialize logging - in production, this should be configurable
    env_logger::init();
    
    info!("Starting SONiC High Frequency Telemetry Counter Sync Daemon");

    // Create communication channels between actors
    let (_command_sender, command_receiver) = channel(1);
    let (socket_sender, socket_receiver) = channel(1);
    let (_ipfix_template_sender, ipfix_template_receiver) = channel(1);
    let (saistats_sender, saistats_receiver) = channel(100); // Increased buffer for stats

    // Get netlink family and group configuration from SONiC constants
    let (family, group) = get_genl_family_group();
    info!("Using netlink family: '{}', group: '{}'", family, group);

    // Initialize and configure actors
    let mut netlink = NetlinkActor::new(family.as_str(), group.as_str(), command_receiver);
    netlink.add_recipient(socket_sender);
    
    let mut ipfix = IpfixActor::new(ipfix_template_receiver, socket_receiver);
    ipfix.add_recipient(saistats_sender);

    // Configure stats reporter with custom settings
    let reporter_config = StatsReporterConfig {
        interval: Duration::from_secs(10), // Report every 10 seconds
        detailed: true, // Show detailed statistics
        max_stats_per_report: Some(20), // Limit to 20 stats per report for readability
    };
    let stats_reporter = StatsReporterActor::new(saistats_receiver, reporter_config, ConsoleWriter);

    info!("Starting actor tasks...");
    
    // Spawn actor tasks
    let netlink_handle = spawn(async move {
        info!("Netlink actor started");
        NetlinkActor::run(netlink).await;
        info!("Netlink actor terminated");
    });
    
    let ipfix_handle = spawn(async move {
        info!("IPFIX actor started");
        IpfixActor::run(ipfix).await;
        info!("IPFIX actor terminated");
    });

    let reporter_handle = spawn(async move {
        info!("Stats reporter actor started");
        StatsReporterActor::run(stats_reporter).await;
        info!("Stats reporter actor terminated");
    });

    // Wait for all actors to complete and handle any errors
    let netlink_result = netlink_handle.await;
    let ipfix_result = ipfix_handle.await;
    let reporter_result = reporter_handle.await;

    match (netlink_result, ipfix_result, reporter_result) {
        (Ok(()), Ok(()), Ok(())) => {
            info!("All actors completed successfully");
            Ok(())
        }
        (Err(e), _, _) => {
            error!("Netlink actor failed: {:?}", e);
            Err(e.into())
        }
        (_, Err(e), _) => {
            error!("IPFIX actor failed: {:?}", e);
            Err(e.into())
        }
        (_, _, Err(e)) => {
            error!("Stats reporter actor failed: {:?}", e);
            Err(e.into())
        }
    }
}
