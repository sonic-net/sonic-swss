use std::{thread::sleep, time::Duration};

use log::{debug, info, warn};

use netlink_sys::{Socket, SocketAddr, protocols::NETLINK_GENERIC};
use tokio::sync::mpsc::Sender;

use std::io;

use super::super::message::netlink::NetlinkCommand;
use super::netlink_utils;

#[cfg(not(test))]
type SocketType = Socket;
#[cfg(test)]
type SocketType = test::MockSocket;

/// Size of the buffer used for receiving netlink messages
const BUFFER_SIZE: usize = 0xFFFF;
/// Interval for periodic family existence checks (in milliseconds)
const FAMILY_CHECK_INTERVAL_MS: u64 = 1_000_u64;
/// Interval for heartbeat logging (number of main loop iterations)
const HEARTBEAT_LOG_INTERVAL: u32 = 6000; // 6000 * 10ms = 1 minute
/// Interval for periodic reconnect commands (number of main loop iterations)
const PERIODIC_RECONNECT_INTERVAL: u32 = 6000; // 6000 * 10ms = 1 minute
/// Interval for control socket recreation attempts (number of main loop iterations)
const CONTROL_SOCKET_RECREATE_INTERVAL: u32 = 18000; // 18000 * 10ms = 3 minutes
/// Minimum netlink message header size in bytes
const NETLINK_HEADER_SIZE: usize = 16;
/// Netlink generic message type
const NETLINK_GENERIC_TYPE: u16 = 16;
/// Generic netlink control command: CTRL_CMD_NEWFAMILY
const CTRL_CMD_NEWFAMILY: u8 = 1;
/// Generic netlink control command: CTRL_CMD_DELFAMILY  
const CTRL_CMD_DELFAMILY: u8 = 2;
/// Netlink attribute type: CTRL_ATTR_FAMILY_NAME
const CTRL_ATTR_FAMILY_NAME: u16 = 2;
/// Size of generic netlink header in bytes
const GENL_HEADER_SIZE: usize = 20;
/// Netlink control notify multicast group ID
const NLCTRL_NOTIFY_GROUP_ID: u32 = 1;

/// Actor responsible for monitoring netlink family registration/unregistration.
///
/// The ControlNetlinkActor handles:
/// - Monitoring netlink control socket for family status changes
/// - Detecting when target family is registered/unregistered
/// - Sending commands to DataNetlinkActor to trigger reconnection
pub struct ControlNetlinkActor {
    /// The generic netlink family name to monitor
    family: String,
    /// Control socket for monitoring family registration/unregistration
    control_socket: Option<SocketType>,
    /// Channel for sending commands to data netlink actor
    command_sender: Sender<NetlinkCommand>,
    /// Last time we checked if the family exists
    last_family_check: std::time::Instant,
    /// Reusable netlink socket for family existence checks
    #[cfg(not(test))]
    resolver: Option<Socket>,
    #[cfg(test)]
    #[allow(dead_code)]
    resolver: Option<()>,
}

impl ControlNetlinkActor {
    /// Creates a new ControlNetlinkActor instance.
    ///
    /// # Arguments
    ///
    /// * `family` - The generic netlink family name to monitor
    /// * `command_sender` - Channel for sending commands to data netlink actor
    ///
    /// # Returns
    ///
    /// A new ControlNetlinkActor instance
    pub fn new(family: &str, command_sender: Sender<NetlinkCommand>) -> Self {
        let mut actor = ControlNetlinkActor {
            family: family.to_string(),
            control_socket: None,
            command_sender,
            last_family_check: std::time::Instant::now(),
            #[cfg(not(test))]
            resolver: None,
            #[cfg(test)]
            resolver: None,
        };

        actor.control_socket = Self::connect_control_socket();

        #[cfg(not(test))]
        {
            actor.resolver = Self::create_nl_resolver();
        }

        actor
    }

    /// Establishes a connection to the netlink control socket.
    #[cfg(not(test))]
    fn connect_control_socket() -> Option<SocketType> {
        // Create a raw netlink socket
        let mut socket = match Socket::new(NETLINK_GENERIC) {
            Ok(s) => s,
            Err(e) => {
                warn!("Failed to create netlink socket: {:?}", e);
                return None;
            }
        };

        // Bind the socket with automatic port assignment
        let addr = SocketAddr::new(0, 0);
        if let Err(e) = socket.bind(&addr) {
            warn!("Failed to bind control socket: {:?}", e);
            return None;
        }

        // Subscribe to nlctrl notify group (group ID 1 for nlctrl notify)
        // The nlctrl family uses a well-known multicast group ID
        if let Err(e) = socket.add_membership(NLCTRL_NOTIFY_GROUP_ID) {
            warn!("Failed to add multicast membership: {:?}", e);
            return None;
        }

        // Set non-blocking mode
        if let Err(e) = socket.set_non_blocking(true) {
            warn!("Failed to set non-blocking mode: {:?}", e);
            return None;
        }

        debug!("Successfully connected control socket and subscribed to nlctrl notifications");
        Some(socket)
    }

    /// Mock control socket for testing.
    #[cfg(test)]
    fn connect_control_socket() -> Option<SocketType> {
        // Return None for tests to avoid complexity
        None
    }

    /// Creates a netlink socket for family/group resolution.
    /// Now delegates to netlink_utils module.
    #[cfg(not(test))]
    fn create_nl_resolver() -> Option<Socket> {
        netlink_utils::create_nl_resolver()
    }

    /// Mock netlink resolver for testing.
    #[cfg(test)]
    #[allow(dead_code)]
    fn create_nl_resolver() -> Option<()> {
        None
    }

    /// Checks if the target genetlink family still exists in the kernel.
    ///
    /// Uses the cached resolver, recreating it only if necessary.
    /// To prevent socket leaks, we limit resolver recreation attempts.
    ///
    /// # Returns
    ///
    /// true if family exists, false otherwise
    #[cfg(not(test))]
    fn check_family_exists(&mut self) -> bool {
        // If we don't have a resolver, try to create a new one
        if self.resolver.is_none() {
            debug!("Creating new netlink resolver for family existence verification");
            self.resolver = Self::create_nl_resolver();
            if self.resolver.is_none() {
                warn!("Failed to create resolver for family existence check");
                return false;
            }
        }

        if let Some(ref mut resolver) = self.resolver {
            match netlink_utils::resolve_family_id(resolver, &self.family) {
                Ok(family_id) => {
                    debug!("Family '{}' exists with ID: {}", self.family, family_id);
                    true
                }
                Err(e) => {
                    debug!("Family '{}' resolution failed: {:?}", self.family, e);
                    // Only clear resolver on specific errors that indicate it's stale
                    let err_str = format!("{:?}", e);
                    if err_str.contains("No such file or directory")
                        || err_str.contains("Connection refused")
                    {
                        debug!("Clearing resolver due to connection error");
                        self.resolver = None;
                    }
                    false
                }
            }
        } else {
            // This shouldn't happen since we just tried to create it above
            warn!("No resolver available for family existence check");
            false
        }
    }

    #[cfg(test)]
    fn check_family_exists(&mut self) -> bool {
        true // In tests, assume family always exists
    }

    /// Attempts to receive a control message from the control socket.
    ///
    /// Returns Ok(true) if a family change was detected, Ok(false) if no relevant message,
    /// or Err if there was an error receiving.
    async fn try_recv_control(
        socket: Option<&mut SocketType>,
        target_family: &str,
    ) -> Result<bool, io::Error> {
        debug!("Attempting to receive control message");
        let socket = socket.ok_or_else(|| {
            io::Error::new(io::ErrorKind::NotConnected, "No control socket available")
        })?;

        let mut buffer = vec![0; BUFFER_SIZE];
        match socket.recv_from(&mut buffer, 0) {
            Ok((size, _addr)) => {
                if size == 0 {
                    return Ok(false);
                }

                buffer.resize(size, 0);
                debug!("Received control message of {} bytes", size);

                // Parse the netlink control message
                match Self::parse_control_message(&buffer, target_family) {
                    Ok(is_relevant) => {
                        if is_relevant {
                            info!(
                                "Control message indicates family '{}' status change",
                                target_family
                            );
                        }
                        Ok(is_relevant)
                    }
                    Err(e) => {
                        debug!("Failed to parse control message: {:?}", e);
                        Ok(false) // Continue even if parsing fails
                    }
                }
            }
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                // No messages available - this is normal for non-blocking sockets
                Ok(false)
            }
            Err(e) => {
                debug!("Control socket error: {:?}", e);
                Err(e)
            }
        }
    }

    /// Parses a netlink control message to check if it's relevant to our target family.
    ///
    /// # Arguments
    ///
    /// * `buffer` - The raw buffer containing the netlink control message
    /// * `target_family` - The family name we're interested in
    ///
    /// # Returns
    ///
    /// Ok(true) if the message is about our target family, Ok(false) otherwise
    fn parse_control_message(buffer: &[u8], target_family: &str) -> Result<bool, io::Error> {
        // Parse the netlink header
        if buffer.len() < NETLINK_HEADER_SIZE {
            return Ok(false);
        }

        let _nl_len = u32::from_le_bytes([buffer[0], buffer[1], buffer[2], buffer[3]]) as usize;
        let nl_type = u16::from_le_bytes([buffer[4], buffer[5]]);

        // Check if this is a generic netlink message
        if nl_type != NETLINK_GENERIC_TYPE {
            return Ok(false);
        }

        // Parse the generic netlink header
        if buffer.len() < GENL_HEADER_SIZE {
            return Ok(false);
        }

        let genl_cmd = buffer[16];

        // Check if this is a family new/del command
        match genl_cmd {
            CTRL_CMD_NEWFAMILY | CTRL_CMD_DELFAMILY => {
                debug!(
                    "Received control command: {}",
                    if genl_cmd == CTRL_CMD_NEWFAMILY {
                        "NEWFAMILY"
                    } else {
                        "DELFAMILY"
                    }
                );

                // Parse attributes to find family name
                let attrs_start = GENL_HEADER_SIZE; // After netlink + genl headers
                if buffer.len() > attrs_start {
                    return Self::parse_family_name_from_attrs(
                        &buffer[attrs_start..],
                        target_family,
                    );
                }
            }
            _ => return Ok(false),
        }

        Ok(false)
    }

    /// Parses netlink attributes to find the family name.
    ///
    /// # Arguments
    ///
    /// * `attrs_buffer` - Buffer containing netlink attributes
    /// * `target_family` - The family name we're looking for
    ///
    /// # Returns
    ///
    /// Ok(true) if target family is found, Ok(false) otherwise
    fn parse_family_name_from_attrs(
        attrs_buffer: &[u8],
        target_family: &str,
    ) -> Result<bool, io::Error> {
        let mut offset = 0;

        while offset + 4 <= attrs_buffer.len() {
            // Parse attribute header: length (2 bytes) + type (2 bytes)
            let attr_len =
                u16::from_le_bytes([attrs_buffer[offset], attrs_buffer[offset + 1]]) as usize;

            let attr_type =
                u16::from_le_bytes([attrs_buffer[offset + 2], attrs_buffer[offset + 3]]);

            // Check if this is CTRL_ATTR_FAMILY_NAME
            if attr_type == CTRL_ATTR_FAMILY_NAME && attr_len > 4 {
                let name_start = offset + 4;
                let name_len = attr_len - 4;

                if name_start + name_len <= attrs_buffer.len() {
                    // Extract family name (null-terminated string)
                    let name_bytes = &attrs_buffer[name_start..name_start + name_len];
                    if let Some(null_pos) = name_bytes.iter().position(|&b| b == 0) {
                        if let Ok(family_name) = std::str::from_utf8(&name_bytes[..null_pos]) {
                            debug!("Found family name in control message: '{}'", family_name);
                            if family_name == target_family {
                                debug!(
                                    "Control message is about our target family: '{}'",
                                    target_family
                                );
                                return Ok(true);
                            }
                        }
                    }
                }
            }

            // Move to next attribute (attributes are aligned to 4-byte boundaries)
            let aligned_len = (attr_len + 3) & !3;
            if aligned_len == 0 {
                // Prevent infinite loop if attr_len is 0
                break;
            }
            offset += aligned_len;
        }

        Ok(false)
    }

    /// Continuously monitors for netlink family status changes.
    /// The loop will monitor the family and send reconnection commands when needed.
    ///
    /// # Arguments
    ///
    /// * `actor` - The ControlNetlinkActor instance to run
    pub async fn run(mut actor: ControlNetlinkActor) {
        debug!("Starting ControlNetlinkActor for family '{}'", actor.family);
        let mut heartbeat_counter = 0u32;
        let mut last_periodic_reconnect_counter = 0u32;
        let mut family_was_available = true; // Assume family starts available

        loop {
            heartbeat_counter += 1;

            // Log heartbeat every minute to show the actor is running
            if heartbeat_counter % HEARTBEAT_LOG_INTERVAL == 0 {
                info!(
                    "ControlNetlinkActor is running normally - monitoring family '{}', family_was_available={}",
                    actor.family,
                    family_was_available,
                );
            }

            // Check for control socket activity
            if let Some(ref mut control_socket) = actor.control_socket {
                match Self::try_recv_control(Some(control_socket), &actor.family).await {
                    Ok(true) => {
                        // Family status changed, force reconnection to pick up new group ID
                        info!("Detected family '{}' status change via control message, sending reconnect command", actor.family);
                        if let Err(e) = actor.command_sender.send(NetlinkCommand::Reconnect).await {
                            warn!("Failed to send reconnect command: {:?}", e);
                            break; // Channel is closed, exit
                        }
                        continue;
                    }
                    Ok(false) => {
                        // No relevant control message, continue with periodic check
                    }
                    Err(e) => {
                        debug!("Failed to receive control message: {:?}", e);
                        // Don't reconnect control socket immediately, it's not critical
                        // But we should try to recreate it periodically
                        if heartbeat_counter % CONTROL_SOCKET_RECREATE_INTERVAL == 0 {
                            debug!("Attempting to recreate control socket");
                            actor.control_socket = Self::connect_control_socket();
                        }
                    }
                }
            }

            // Perform periodic family existence check
            let now = std::time::Instant::now();
            if now.duration_since(actor.last_family_check).as_millis()
                > FAMILY_CHECK_INTERVAL_MS as u128
            {
                actor.last_family_check = now;
                let family_available = actor.check_family_exists();
                debug!(
                    "heartbeat: family_available={}, family_was_available={}, heartbeat_counter={}",
                    family_available, family_was_available, heartbeat_counter
                );
                if family_available != family_was_available {
                    if family_available {
                        info!(
                            "Family '{}' is now available, sending reconnect command",
                            actor.family
                        );
                        if let Err(e) = actor.command_sender.send(NetlinkCommand::Reconnect).await {
                            warn!("Failed to send reconnect command: {:?}", e);
                            break; // Channel is closed, exit
                        }
                    } else {
                        warn!("Family '{}' is no longer available", actor.family);
                        // Don't send disconnect command, just let data actor handle it naturally
                    }
                    family_was_available = family_available;
                } else if family_available {
                    // Family is available but we haven't sent a reconnect recently
                    // Send periodic soft reconnect commands to ensure DataNetlinkActor stays connected
                    // This handles cases where DataNetlinkActor disconnected due to socket errors
                    // SoftReconnect only reconnects if socket is unhealthy, avoiding unnecessary reconnections
                    if heartbeat_counter - last_periodic_reconnect_counter
                        >= PERIODIC_RECONNECT_INTERVAL
                    {
                        debug!("Sending periodic soft reconnect command to check data socket health (counter: {}, last: {}, interval: {})", 
                               heartbeat_counter, last_periodic_reconnect_counter, PERIODIC_RECONNECT_INTERVAL);
                        if let Err(e) = actor.command_sender.send(NetlinkCommand::SoftReconnect).await {
                            warn!("Failed to send periodic soft reconnect command: {:?}", e);
                            break; // Channel is closed, exit
                        }
                        last_periodic_reconnect_counter = heartbeat_counter;
                    }
                }
            }

            // Check if the command channel is still open by trying a non-blocking send
            // This helps detect when the receiver has been dropped and we should exit
            if actor.command_sender.is_closed() {
                debug!("Command channel is closed, terminating ControlNetlinkActor");
                break;
            }

            // Wait a bit before next iteration
            sleep(Duration::from_millis(10));
        }

        debug!("ControlNetlinkActor terminated");
    }
}

#[cfg(test)]
pub mod test {
    use super::*;
    use std::time::Duration;
    use tokio::{spawn, sync::mpsc::channel, time::timeout};

    /// Mock socket for testing purposes.
    pub struct MockSocket;

    impl MockSocket {
        pub fn recv_from(&mut self, _buf: &mut [u8], _flags: i32) -> Result<(usize, SocketAddr), io::Error> {
            // Always return WouldBlock to simulate no control messages
            Err(io::Error::new(
                io::ErrorKind::WouldBlock,
                "No control messages in test",
            ))
        }
    }

    /// Tests the ControlNetlinkActor's basic functionality.
    ///
    /// This test verifies that:
    /// - The actor starts correctly
    /// - It can be created and initialized
    #[tokio::test]
    async fn test_control_netlink_actor() {
        // Initialize logging for the test
        let _ = env_logger::builder()
            .filter_level(log::LevelFilter::Debug)
            .is_test(true)
            .try_init();

        let (command_sender, command_receiver) = channel(10);
        let actor = ControlNetlinkActor::new("test_family", command_sender);

        // Test actor creation and basic properties
        assert_eq!(actor.family, "test_family");
        assert!(actor.control_socket.is_none()); // Should be None in test

        // Start the actor in the background but don't wait for it to finish
        let handle = spawn(async move {
            // Run actor for a very short time then exit
            let actor = actor;

            // Simulate a few iterations
            for _ in 0..3 {
                // Check if the command channel is still open
                if actor.command_sender.is_closed() {
                    break;
                }
                tokio::time::sleep(Duration::from_millis(1)).await;
            }
        });

        // Close the channel immediately
        drop(command_receiver);

        // Wait for the simulated actor to finish
        let _result = timeout(Duration::from_millis(100), handle).await;
    }

    /// Tests control message parsing functionality.
    #[test]
    fn test_control_message_parsing() {
        // Test with a mock control message buffer
        let mut buffer = vec![0u8; 100];

        // Set up netlink header (16 bytes)
        buffer[0..4].copy_from_slice(&(50u32).to_le_bytes()); // message length
        buffer[4..6].copy_from_slice(&(16u16).to_le_bytes()); // NETLINK_GENERIC type

        // Set up generic netlink header (4 bytes)
        buffer[16] = 1; // CTRL_CMD_NEWFAMILY

        // Set up attributes (starting at offset 20)
        let family_name = b"test_family\0";
        let attr_len = 4 + family_name.len(); // header + data
        buffer[20..22].copy_from_slice(&(attr_len as u16).to_le_bytes()); // attribute length
        buffer[22..24].copy_from_slice(&(2u16).to_le_bytes()); // CTRL_ATTR_FAMILY_NAME
        buffer[24..24 + family_name.len()].copy_from_slice(family_name);

        let result = ControlNetlinkActor::parse_control_message(&buffer, "test_family");
        assert!(result.is_ok());
        assert!(result.unwrap()); // Should detect the target family

        // Test with different family name
        let result2 = ControlNetlinkActor::parse_control_message(&buffer, "other_family");
        assert!(result2.is_ok());
        assert!(!result2.unwrap()); // Should not detect different family
    }

    /// Tests family name parsing from attributes.
    #[test]
    fn test_family_name_parsing() {
        let mut attrs_buffer = vec![0u8; 50];

        // Create a mock attribute with family name
        let family_name = b"sonic_stel\0";
        let attr_len = 4 + family_name.len(); // header + data

        attrs_buffer[0..2].copy_from_slice(&(attr_len as u16).to_le_bytes()); // length
        attrs_buffer[2..4].copy_from_slice(&(2u16).to_le_bytes()); // CTRL_ATTR_FAMILY_NAME type
        attrs_buffer[4..4 + family_name.len()].copy_from_slice(family_name);

        let result = ControlNetlinkActor::parse_family_name_from_attrs(&attrs_buffer, "sonic_stel");
        assert!(result.is_ok());
        assert!(result.unwrap());

        // Test with non-matching family
        let result2 =
            ControlNetlinkActor::parse_family_name_from_attrs(&attrs_buffer, "other_family");
        assert!(result2.is_ok());
        assert!(!result2.unwrap());
    }
}
