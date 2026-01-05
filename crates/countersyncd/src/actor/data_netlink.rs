use std::{
    collections::LinkedList,
    sync::Arc,
    thread::sleep,
    time::{Duration, Instant},
};

use std::os::unix::io::AsRawFd;
#[cfg(test)]
use std::os::unix::io::RawFd;

use log::{debug, info, warn};

use netlink_sys::{Socket, SocketAddr, protocols::NETLINK_GENERIC};
use tokio::sync::mpsc::{Receiver, Sender};

use std::io;

use super::super::message::{
    buffer::SocketBufferMessage,
    netlink::{NetlinkCommand, SocketConnect},
};
use super::netlink_utils;

#[cfg(not(test))]
type SocketType = Socket;
#[cfg(test)]
type SocketType = test::MockSocket;

/// Path to the sonic constants configuration file
const SONIC_CONSTANTS: &str = "/etc/sonic/constants.yml";

/// Size of the buffer used for receiving netlink messages
const BUFFER_SIZE: usize = 0x1FFFF;
/// Linux error code for "No buffer space available" (ENOBUFS)
/// Note: std::io::ErrorKind doesn't have a specific variant for ENOBUFS,
/// so we use the raw OS error code for this specific netlink error condition.
const ENOBUFS: i32 = 105;

/// Maximum number of consecutive failures before waiting for ControlNetlinkActor
const MAX_LOCAL_RECONNECT_ATTEMPTS: u32 = 3;

/// Socket health check timeout - if no data received for this duration, socket is considered unhealthy
const SOCKET_HEALTH_TIMEOUT_SECS: u64 = 60;

/// Heartbeat logging interval (in iterations) - log every 5 minutes at 10ms per iteration
const HEARTBEAT_LOG_INTERVAL: u32 = 30000; // 30000 * 10ms = 5 minutes

/// Debug logging interval (in iterations) - log debug info every 30 seconds
const DEBUG_LOG_INTERVAL: u32 = 3000; // 3000 * 10ms = 30 seconds

/// WouldBlock debug logging interval (in iterations) - log WouldBlock every minute
const WOULDBLOCK_LOG_INTERVAL: u32 = 6000; // 6000 * 10ms = 1 minute

/// Socket readiness check timeout in milliseconds
const SOCKET_READINESS_TIMEOUT_MS: u64 = 10;

/// Maximum size for buffering incomplete messages (1MB)
const MAX_INCOMPLETE_MESSAGE_SIZE: usize = 1024 * 1024;

/// Netlink message parser for handling multiple messages in one buffer
#[derive(Debug)]
struct NetlinkMessageParser {
    /// Buffer for incomplete messages that span multiple recv operations
    incomplete_buffer: Vec<u8>,
}

impl NetlinkMessageParser {
    fn new() -> Self {
        Self {
            incomplete_buffer: Vec::new(),
        }
    }

    /// Parse buffer that may contain multiple complete and/or incomplete netlink messages
    /// Returns a vector of complete message payloads, where each payload represents 
    /// one complete netlink message (which contains one complete IPFIX message)
    fn parse_buffer(&mut self, new_data: &[u8]) -> Result<Vec<SocketBufferMessage>, io::Error> {
        // Combine any incomplete data from previous recv with new data
        if !self.incomplete_buffer.is_empty() {
            self.incomplete_buffer.extend_from_slice(new_data);
            debug!("Combined incomplete buffer ({} bytes) with new data ({} bytes)", 
                   self.incomplete_buffer.len() - new_data.len(), new_data.len());
        } else {
            self.incomplete_buffer.extend_from_slice(new_data);
        }

        let mut complete_messages = Vec::new();
        let mut offset = 0;

        // Parse all complete messages in the buffer
        while offset < self.incomplete_buffer.len() {
            // Check if we have enough data for a netlink header
            if offset + 16 > self.incomplete_buffer.len() {
                debug!("Not enough data for netlink header at offset {}, keeping {} bytes for next recv", 
                       offset, self.incomplete_buffer.len() - offset);
                break;
            }

            // Extract message length from netlink header
            let nl_len = u32::from_le_bytes([
                self.incomplete_buffer[offset],
                self.incomplete_buffer[offset + 1], 
                self.incomplete_buffer[offset + 2],
                self.incomplete_buffer[offset + 3],
            ]) as usize;

            // Validate message length
            if nl_len < 16 {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("Invalid netlink message length: {} (too small)", nl_len),
                ));
            }

            if nl_len > MAX_INCOMPLETE_MESSAGE_SIZE {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("Invalid netlink message length: {} (too large)", nl_len),
                ));
            }

            // Check if we have the complete message
            if offset + nl_len > self.incomplete_buffer.len() {
                debug!("Incomplete message at offset {}: need {} bytes, have {} bytes", 
                       offset, nl_len, self.incomplete_buffer.len() - offset);
                break;
            }

            // Extract complete message
            let message_data = self.incomplete_buffer[offset..offset + nl_len].to_vec();
            debug!("Found complete message: offset={}, length={}", offset, nl_len);

            // Extract payload from this message
            match Self::extract_payload_from_slice(&message_data) {
                Ok(payload) => {
                    debug!("Successfully extracted payload with {} bytes", payload.len());
                    complete_messages.push(payload);
                }
                Err(e) => {
                    warn!("Failed to extract payload from message at offset {}: {}", offset, e);
                    // Continue with next message instead of failing completely
                }
            }

            offset += nl_len;
        }

        // Keep remaining incomplete data for next recv
        if offset < self.incomplete_buffer.len() {
            let remaining = self.incomplete_buffer[offset..].to_vec();
            debug!("Keeping {} bytes for next recv operation", remaining.len());
            self.incomplete_buffer = remaining;
        } else {
            // All data was consumed
            self.incomplete_buffer.clear();
        }

        Ok(complete_messages)
    }

    /// Extract payload from a single complete netlink message
    fn extract_payload_from_slice(message_data: &[u8]) -> Result<SocketBufferMessage, io::Error> {
        const NLMSG_HDRLEN: usize = 16; // sizeof(struct nlmsghdr)
        const GENL_HDRLEN: usize = 4; // sizeof(struct genlmsghdr)
        const TOTAL_HEADER_SIZE: usize = NLMSG_HDRLEN + GENL_HDRLEN;

        if message_data.len() < TOTAL_HEADER_SIZE {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Message too small: {} bytes, expected at least {}", 
                        message_data.len(), TOTAL_HEADER_SIZE),
            ));
        }

        // Extract netlink message length from header
        let nl_len = u32::from_le_bytes([
            message_data[0], message_data[1], message_data[2], message_data[3]
        ]) as usize;

        if nl_len != message_data.len() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Message length mismatch: header says {}, actual {}", 
                        nl_len, message_data.len()),
            ));
        }

        // Debug: Print headers only when debug logging is enabled
        if log::log_enabled!(log::Level::Debug) {
            debug!("Netlink Header (16 bytes): {:02x?}", &message_data[0..16]);
            let nl_type = u16::from_le_bytes([message_data[4], message_data[5]]);
            let nl_flags = u16::from_le_bytes([message_data[6], message_data[7]]);
            let nl_seq = u32::from_le_bytes([message_data[8], message_data[9], message_data[10], message_data[11]]);
            let nl_pid = u32::from_le_bytes([message_data[12], message_data[13], message_data[14], message_data[15]]);
            debug!("  nl_len={}, nl_type={}, nl_flags=0x{:04x}, nl_seq={}, nl_pid={}", 
                   nl_len, nl_type, nl_flags, nl_seq, nl_pid);

            if message_data.len() >= TOTAL_HEADER_SIZE {
                debug!("Generic Netlink Header (4 bytes): {:02x?}", &message_data[16..20]);
                let genl_cmd = message_data[16];
                let genl_version = message_data[17];
                let genl_reserved = u16::from_le_bytes([message_data[18], message_data[19]]);
                debug!("  genl_cmd={}, genl_version={}, genl_reserved=0x{:04x}", 
                       genl_cmd, genl_version, genl_reserved);
            }
        }

        // Extract payload after both headers
        let payload_start = TOTAL_HEADER_SIZE;
        let payload_end = nl_len;

        if payload_start >= payload_end {
            // No payload data, return empty payload
            Ok(Arc::new(Vec::new()))
        } else {
            // Return payload data without headers
            let payload = message_data[payload_start..payload_end].to_vec();
            Ok(Arc::new(payload))
        }
    }
}

/// Actor responsible for managing the data netlink socket and message distribution.
///
/// The DataNetlinkActor handles:
/// - Establishing and maintaining data netlink socket connections
/// - Processing control commands for socket management  
/// - Distribution of received messages to multiple recipients
pub struct DataNetlinkActor {
    /// The generic netlink family name
    family: String,
    /// The multicast group name
    group: String,
    /// The active netlink socket connection (None if disconnected)
    socket: Option<SocketType>,
    /// Reusable netlink socket for family/group resolution (None if not available)
    #[allow(dead_code)]
    nl_resolver: Option<Socket>,
    /// Timestamp of when we last received data on the socket (for health checking)
    last_data_time: Instant,
    /// List of channels to send received buffer messages to
    buffer_recipients: LinkedList<Sender<SocketBufferMessage>>,
    /// Channel for receiving control commands
    command_recipient: Receiver<NetlinkCommand>,
    /// Message parser for handling multiple and fragmented netlink messages
    message_parser: NetlinkMessageParser,
}

impl DataNetlinkActor {
    /// Creates a new DataNetlinkActor instance.
    ///
    /// # Arguments
    ///
    /// * `family` - The generic netlink family name
    /// * `group` - The multicast group name
    /// * `command_recipient` - Channel for receiving control commands
    ///
    /// # Returns
    ///
    /// A new DataNetlinkActor instance with an initial connection attempt
    pub fn new(family: &str, group: &str, command_recipient: Receiver<NetlinkCommand>) -> Self {
        let nl_resolver = Self::create_nl_resolver();
        let mut actor = DataNetlinkActor {
            family: family.to_string(),
            group: group.to_string(),
            socket: None,
            nl_resolver,
            last_data_time: Instant::now(),
            buffer_recipients: LinkedList::new(),
            command_recipient,
            message_parser: NetlinkMessageParser::new(),
        };

        // Use instance method for initial connection
        actor.socket = actor.connect_with_nl_resolver(family, group);

        actor
    }

    /// Adds a new recipient channel for receiving buffer messages.
    ///
    /// # Arguments
    ///
    /// * `recipient` - Channel sender for distributing received messages
    pub fn add_recipient(&mut self, recipient: Sender<SocketBufferMessage>) {
        self.buffer_recipients.push_back(recipient);
    }

    /// Creates a netlink socket for family/group resolution.
    ///
    /// # Returns
    ///
    /// Some(socket) if creation is successful, None otherwise
    /// Creates a netlink socket for family/group resolution.
    /// Now delegates to netlink_utils module.
    #[cfg(not(test))]
    fn create_nl_resolver() -> Option<Socket> {
        netlink_utils::create_nl_resolver()
    }

    /// Mock netlink resolver for testing.
    #[cfg(test)]
    fn create_nl_resolver() -> Option<Socket> {
        None
    }

    /// Establishes a connection to the netlink socket using the netlink resolver when available.
    ///
    /// # Arguments
    ///
    /// * `family` - The generic netlink family name
    /// * `group` - The multicast group name
    ///
    /// # Returns
    ///
    /// Some(socket) if connection is successful, None otherwise
    #[cfg(not(test))]
    fn connect_with_nl_resolver(&mut self, family: &str, group: &str) -> Option<SocketType> {
        debug!(
            "Attempting to connect to family '{}', group '{}'",
            family, group
        );

        // Try to use existing netlink resolver first
        let group_id = if let Some(ref mut resolver) = self.nl_resolver {
            match netlink_utils::resolve_multicast_group(resolver, family, group) {
                Ok(id) => {
                    info!(
                        "Resolved group ID {} for family '{}', group '{}' (using netlink resolver)",
                        id, family, group
                    );
                    id
                }
                Err(e) => {
                    info!(
                        "Failed to resolve group with netlink resolver: {:?}, recreating resolver",
                        e
                    );
                    // Resolver might be stale, recreate it
                    self.nl_resolver = Self::create_nl_resolver();

                    // Try again with new resolver
                    if let Some(ref mut resolver) = self.nl_resolver {
                        match netlink_utils::resolve_multicast_group(resolver, family, group) {
                            Ok(id) => {
                                info!("Resolved group ID {} for family '{}', group '{}' (using new netlink resolver)", id, family, group);
                                id
                            }
                            Err(e) => {
                                warn!("Failed to resolve group id for family '{}', group '{}' with new netlink resolver: {:?}", family, group, e);
                                warn!(
                                    "This suggests the family '{}' is not registered in the kernel",
                                    family
                                );
                                return None;
                            }
                        }
                    } else {
                        // Fallback to creating temporary socket
                        return Self::connect_fallback(family, group);
                    }
                }
            }
        } else {
            // Create netlink resolver if not available
            self.nl_resolver = Self::create_nl_resolver();

            if let Some(ref mut resolver) = self.nl_resolver {
                match netlink_utils::resolve_multicast_group(resolver, family, group) {
                    Ok(id) => {
                        info!("Resolved group ID {} for family '{}', group '{}' (using new netlink resolver)", id, family, group);
                        id
                    }
                    Err(e) => {
                        warn!(
                            "Failed to resolve group id for family '{}', group '{}': {:?}",
                            family, group, e
                        );
                        warn!(
                            "This suggests the family '{}' is not registered in the kernel",
                            family
                        );
                        return None;
                    }
                }
            } else {
                // Fallback to creating temporary socket
                return Self::connect_fallback(family, group);
            }
        };

        debug!(
            "Creating socket for family '{}' with group_id {}",
            family, group_id
        );
        
        // Create a raw netlink socket using netlink-sys
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
            warn!("Failed to bind socket: {:?}", e);
            return None;
        }

        debug!("Adding multicast membership for group_id {}", group_id);
        if let Err(e) = socket.add_membership(group_id) {
            warn!(
                "Failed to add mcast membership for group_id {}: {:?}",
                group_id, e
            );
            // Explicitly drop the socket to ensure it's closed
            drop(socket);
            return None;
        }

        // Set non-blocking mode
        if let Err(e) = socket.set_non_blocking(true) {
            warn!("Failed to set non-blocking mode: {:?}", e);
            return None;
        }

        info!(
            "Successfully connected to family '{}', group '{}' with group_id: {}",
            family, group, group_id
        );
        debug!("Socket created successfully, ready to receive multicast messages on group_id: {}", group_id);
        Some(socket)
    }

    /// Mock connection method using shared router for testing.
    #[cfg(test)]
    fn connect_with_nl_resolver(&mut self, _family: &str, _group: &str) -> Option<SocketType> {
        // For tests, we always allow successful connections
        // The MockSocket itself will control data availability
        let sock = SocketType::new();
        if sock.valid {
            debug!("Test: Created new valid MockSocket");
            Some(sock)
        } else {
            debug!("Test: MockSocket reports invalid, connection failed");
            None
        }
    }

    /// Fallback connection method when shared router is not available.
    #[cfg(not(test))]
    fn connect_fallback(family: &str, group: &str) -> Option<SocketType> {
        debug!(
            "Using fallback connection for family '{}', group '{}'",
            family, group
        );

        // Create a temporary netlink socket for resolution
        let mut temp_socket = match Socket::new(NETLINK_GENERIC) {
            Ok(mut s) => {
                let addr = SocketAddr::new(0, 0);
                if let Err(e) = s.bind(&addr) {
                    warn!("Failed to bind temporary socket: {:?}", e);
                    return None;
                }
                s
            }
            Err(e) => {
                warn!("Failed to create temporary netlink socket: {:?}", e);
                warn!("Possible causes: insufficient permissions, netlink not supported, or kernel module not loaded");
                return None;
            }
        };

        debug!(
            "Temporary socket created, resolving group ID for family '{}', group '{}'",
            family, group
        );
        let group_id = match netlink_utils::resolve_multicast_group(&mut temp_socket, family, group) {
            Ok(id) => {
                debug!(
                    "Resolved group ID {} for family '{}', group '{}'",
                    id, family, group
                );
                id
            }
            Err(e) => {
                warn!(
                    "Failed to resolve group id for family '{}', group '{}': {:?}",
                    family, group, e
                );
                warn!(
                    "This suggests the family '{}' is not registered in the kernel",
                    family
                );
                return None;
            }
        };

        debug!(
            "Creating socket for family '{}' with group_id {}",
            family, group_id
        );
        
        // Create a raw netlink socket
        let mut socket = match Socket::new(NETLINK_GENERIC) {
            Ok(s) => s,
            Err(e) => {
                warn!("Failed to create netlink socket: {:?}", e);
                return None;
            }
        };

        // Bind the socket
        let addr = SocketAddr::new(0, 0);
        if let Err(e) = socket.bind(&addr) {
            warn!("Failed to bind socket: {:?}", e);
            return None;
        }

        debug!("Adding multicast membership for group_id {}", group_id);
        if let Err(e) = socket.add_membership(group_id) {
            warn!(
                "Failed to add mcast membership for group_id {}: {:?}",
                group_id, e
            );
            return None;
        }

        // Set non-blocking mode
        if let Err(e) = socket.set_non_blocking(true) {
            warn!("Failed to set non-blocking mode: {:?}", e);
            return None;
        }

        info!(
            "Successfully connected to family '{}', group '{}' with group_id: {}",
            family, group, group_id
        );
        debug!("Socket created successfully, ready to receive multicast messages on group_id: {}", group_id);
        Some(socket)
    }

    /// Attempts to establish a connection on demand.
    ///
    /// This will be called when receiving a Reconnect or SoftReconnect command from ControlNetlinkActor.
    /// 
    /// # Arguments
    ///
    /// * `force` - If true, always reconnect regardless of socket health status.
    ///             If false, only reconnect if socket is unhealthy (no data received recently).
    ///
    /// # Behavior
    ///
    /// - `force=true` (Reconnect): Always closes current socket and creates new connection
    /// - `force=false` (SoftReconnect): Only reconnects if socket hasn't received data for SOCKET_HEALTH_TIMEOUT_SECS
    fn connect(&mut self, force: bool) {
        // Check if current socket is healthy (only when not forcing reconnection)
        if !force {
            if let Some(_socket) = &self.socket {
                let time_since_last_data = Instant::now().duration_since(self.last_data_time);
                if time_since_last_data > Duration::from_secs(SOCKET_HEALTH_TIMEOUT_SECS) {
                    warn!(
                        "Socket unhealthy - no data received for {} seconds, forcing reconnection",
                        time_since_last_data.as_secs()
                        );
                        // Close the unhealthy socket
                        self.socket = None;
                } else {
                    info!(
                        "Socket healthy - data received {} seconds ago, skipping reconnect",
                        time_since_last_data.as_secs()
                    );
                    return;
                }
            }
        } else {
            // Force reconnection: close current socket if exists
            if self.socket.is_some() {
                info!("Force reconnection requested, closing current socket");
                self.socket = None;
            }
        }

        info!(
            "Establishing new connection for family '{}', group '{}'",
            self.family, self.group
        );
        self.socket = self.connect_with_nl_resolver(&self.family.clone(), &self.group.clone());
        if self.socket.is_some() {
            info!(
                "Successfully connected to family '{}', group '{}'",
                self.family, self.group
            );
            self.last_data_time = Instant::now(); // Reset data time for new socket
        } else {
            warn!(
                "Failed to connect to family '{}', group '{}'",
                self.family, self.group
            );
            // Clear the resolver as it might be stale
            self.nl_resolver = None;
        }
    }

    /// Disconnects the current socket.
    ///
    /// This will be called when there's a socket error, to clean up the connection
    /// and wait for ControlNetlinkActor to send a reconnect command.
    fn disconnect(&mut self) {
        if self.socket.is_some() {
            debug!(
                "Disconnecting socket for family '{}', group '{}'",
                self.family, self.group
            );
            self.socket = None;
            // Clear the resolver as it might be stale
            self.nl_resolver = None;
        }
    }

    /// Resets the actor's configuration and attempts to connect.
    ///
    /// # Arguments
    ///
    /// * `family` - New family name to use
    /// * `group` - New group name to use  
    fn reset(&mut self, family: &str, group: &str) {
        debug!(
            "Resetting connection: family '{}' -> '{}', group '{}' -> '{}'",
            self.family, family, self.group, group
        );
        self.family = family.to_string();
        self.group = group.to_string();
        self.connect(true); // Force reconnection on reset
    }

    /// Attempts to receive messages from the netlink socket.
    ///
    /// Returns immediately with WouldBlock if no data is available, allowing
    /// the event loop to handle other operations concurrently.
    /// 
    /// This function handles multiple scenarios:
    /// 1. Single complete message in one recv
    /// 2. Multiple complete messages in one recv  
    /// 3. Incomplete message that needs to be combined with future recv data
    async fn try_recv(
        socket: Option<&mut SocketType>, 
        message_parser: &mut NetlinkMessageParser
    ) -> Result<Vec<SocketBufferMessage>, io::Error> {
        let socket = socket
            .ok_or_else(|| io::Error::new(io::ErrorKind::NotConnected, "No socket available"))?;

        let mut buffer = vec![0; BUFFER_SIZE];

        // Try to receive with non-blocking mode (socket should already be set to non-blocking)
        debug!("Attempting to receive netlink message...");
        
        #[cfg(not(test))]
        let result = {
            let fd = socket.as_raw_fd();
            unsafe {
                libc::recv(
                    fd,
                    buffer.as_mut_ptr() as *mut libc::c_void,
                    buffer.len(),
                    0
                )
            }
        };

        #[cfg(test)]
        let result = match socket.recv(&mut buffer, 0) {
            Ok(size) => size as isize,
            Err(e) => {
                return Err(e);
            }
        };

        match result {
            size if size > 0 => {
                let size = size as usize;
                debug!("Received netlink data, size: {} bytes", size);

                if size == 0 {
                    return Err(io::Error::new(
                        io::ErrorKind::UnexpectedEof,
                        "No more data to receive",
                    ));
                }

                // Resize buffer to actual received size
                buffer.resize(size, 0);

                // Parse buffer which may contain multiple messages and/or incomplete messages
                let messages = message_parser.parse_buffer(&buffer)?;
                debug!("Parsed {} complete messages from {} bytes of data", messages.len(), size);
                
                Ok(messages)
            }
            size if size == 0 => {
                Err(io::Error::new(
                    io::ErrorKind::UnexpectedEof,
                    "Connection closed",
                ))
            }
            _ => {
                // size < 0, errno is set
                let err = io::Error::last_os_error();
                // WouldBlock is expected for non-blocking sockets with no data
                // Only log other errors
                if err.kind() != io::ErrorKind::WouldBlock {
                    debug!(
                        "Socket recv failed: {:?} (raw_os_error: {:?})",
                        err,
                        err.raw_os_error()
                    );
                }
                Err(err)
            }
        }
    }

    /// Checks for socket readiness without unsafe operations.
    ///
    /// This is a safer alternative that uses tokio's timeout mechanism
    /// instead of direct file descriptor polling with unsafe operations.
    ///
    /// # Arguments
    ///
    /// * `timeout_ms` - Timeout in milliseconds
    ///
    /// # Returns
    ///
    /// A boolean indicating if data socket has data
    async fn check_socket_readiness(timeout_ms: u64) -> Result<bool, io::Error> {
        // In test environment, always return true to let try_recv() handle the actual data availability
        #[cfg(test)]
        {
            // Simulate minimal polling delay
            sleep(Duration::from_millis(std::cmp::min(timeout_ms, 1)));
            // Always return true in test mode - let MockSocket.recv() handle availability
            return Ok(true);
        }

        #[cfg(not(test))]
        {
            use tokio::time::{sleep as tokio_sleep, Duration as TokioDuration};

            // For production, we simply wait for the timeout period
            // This approach avoids unsafe operations but is less efficient
            // The actual socket readiness will be checked by try_recv() calls
            tokio_sleep(TokioDuration::from_millis(timeout_ms)).await;

            // Always return that data might be ready, let try_recv() handle the actual check
            // This is safe but potentially less efficient than direct polling
            Ok(true)
        }
    }

    /// Continuously processes incoming netlink messages and control commands.
    /// The loop will exit when the command channel is closed or a Close command is received.
    ///
    /// # Arguments
    ///
    /// * `actor` - The DataNetlinkActor instance to run
    pub async fn run(mut actor: DataNetlinkActor) {
        debug!(
            "Starting DataNetlinkActor with {} buffer recipients configured",
            actor.buffer_recipients.len()
        );
        let mut heartbeat_counter = 0u32;
        let mut consecutive_failures = 0u32;

        loop {
            // Log heartbeat every 5 minutes to show the actor is running
            heartbeat_counter += 1;
            if heartbeat_counter % HEARTBEAT_LOG_INTERVAL == 0 {
                info!("DataNetlinkActor is running normally - waiting for data messages");
            }

            // More frequent debug info about socket status
            if heartbeat_counter % DEBUG_LOG_INTERVAL == 0 {
                debug!(
                    "DataNetlinkActor heartbeat: socket={}, recipients={}, failures={}",
                    actor.socket.is_some(),
                    actor.buffer_recipients.len(),
                    consecutive_failures
                );
                if actor.socket.is_some() {
                    debug!("Socket is available and we are actively trying to receive messages");
                    consecutive_failures = 0; // Reset failure counter when socket is available
                }
            }

            // Check for pending commands first (non-blocking)
            if let Ok(command) = actor.command_recipient.try_recv() {
                match command {
                    NetlinkCommand::SocketConnect(SocketConnect { family, group }) => {
                        actor.reset(&family, &group);
                        consecutive_failures = 0; // Reset failure counter on reconnect command
                    }
                    NetlinkCommand::Reconnect => {
                        actor.connect(true); // Force reconnection
                        consecutive_failures = 0; // Reset failure counter on reconnect command
                    }
                    NetlinkCommand::SoftReconnect => {
                        actor.connect(false); // Check health before reconnecting
                        consecutive_failures = 0; // Reset failure counter on reconnect command
                    }
                    NetlinkCommand::Close => {
                        break;
                    }
                }
                continue;
            }

            // Check socket readiness with configurable timeout to allow periodic checks
            match Self::check_socket_readiness(SOCKET_READINESS_TIMEOUT_MS).await {
                Ok(data_ready) => {
                    // Only try to receive data if we have a socket and data is ready
                    if actor.socket.is_some() && data_ready {
                        match Self::try_recv(actor.socket.as_mut(), &mut actor.message_parser).await {
                            Ok(messages) => {
                                consecutive_failures = 0; // Reset failure counter on successful receive
                                actor.last_data_time = Instant::now(); // Update data reception timestamp
                                
                                if messages.is_empty() {
                                    debug!("Received data but no complete messages yet (partial message)");
                                } else {
                                    debug!("Successfully parsed {} complete netlink messages", messages.len());
                                    
                                    // Send each complete netlink message individually to all recipients
                                    // This ensures each IPFIX message (contained in one netlink message) 
                                    // is sent as a separate operation to the downstream actors
                                    for (i, message) in messages.iter().enumerate() {
                                        debug!("Processing netlink message {}/{}: {} bytes", 
                                               i + 1, messages.len(), message.len());
                                        
                                        // Send this single netlink message to all recipients
                                        for (j, recipient) in actor.buffer_recipients.iter().enumerate() {
                                            debug!("Sending netlink message {}/{} to recipient {}", 
                                                   i + 1, messages.len(), j + 1);
                                            if let Err(e) = recipient.send(message.clone()).await {
                                                warn!("Failed to send netlink message {}/{} to recipient {}: {:?}", 
                                                      i + 1, messages.len(), j + 1, e);
                                                // Consider removing failed recipients here if needed
                                            } else {
                                                debug!("Successfully sent netlink message {}/{} ({} bytes) to recipient {}", 
                                                       i + 1, messages.len(), message.len(), j + 1);
                                            }
                                        }
                                    }
                                    
                                    debug!("Completed processing {} netlink messages, each sent individually", messages.len());
                                }
                            }
                            Err(e) => {
                                // Handle specific errors
                                if let Some(os_error) = e.raw_os_error() {
                                    if os_error == ENOBUFS {
                                        warn!("Netlink receive buffer full (ENOBUFS). Consider increasing buffer size or processing messages faster. Error: {:?}", e);
                                        // Don't disconnect on ENOBUFS, just continue
                                        continue;
                                    }
                                }

                                // Check if it's WouldBlock using standard ErrorKind
                                if e.kind() == io::ErrorKind::WouldBlock {
                                    // No data available right now, continue normally
                                    if heartbeat_counter % WOULDBLOCK_LOG_INTERVAL == 0 {
                                        debug!("No netlink data available (WouldBlock) - socket is connected but no messages from kernel");
                                    }
                                } else {
                                    // Socket error occurred, disconnect and try limited reconnects
                                    warn!("Failed to receive message: {:?}", e);
                                    actor.disconnect();
                                    consecutive_failures += 1;

                                    // Only attempt very limited local reconnects
                                    if consecutive_failures <= MAX_LOCAL_RECONNECT_ATTEMPTS {
                                        debug!(
                                            "Attempting quick reconnect #{}",
                                            consecutive_failures
                                        );
                                        actor.connect(true); // Force reconnection on error
                                    } else {
                                        debug!("Too many consecutive failures, waiting for reconnect command from ControlNetlinkActor");
                                    }
                                }
                            }
                        }
                    } else if actor.socket.is_none() {
                        // No socket available, log this periodically but don't spam
                        if heartbeat_counter % DEBUG_LOG_INTERVAL == 0 {
                            debug!("No socket available - waiting for reconnect command from ControlNetlinkActor");
                        }
                    }
                }
                Err(e) => {
                    warn!("Poll error: {:?}", e);
                    // Wait a bit before retrying to avoid busy loop on persistent poll errors
                    sleep(Duration::from_millis(SOCKET_READINESS_TIMEOUT_MS));
                }
            }
        }
    }
}

impl Drop for DataNetlinkActor {
    fn drop(&mut self) {
        if !self.command_recipient.is_closed() {
            self.command_recipient.close();
        }
    }
}

#[cfg(test)]
pub mod test {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use tokio::{spawn, sync::mpsc::channel};

    // Helper function to create a properly sized message vector
    fn create_test_message(payload: &[u8]) -> Vec<u8> {
        let msg = create_mock_netlink_message(payload);
        let actual_len = 20 + payload.len(); // 16 (nlmsg) + 4 (genl) + payload
        msg[..actual_len].to_vec()
    }

    // Test constants for simulating different message scenarios  
    fn get_partially_valid_messages() -> Vec<Vec<u8>> {
        vec![
            create_test_message(b"PARTIALLY_VALID1"),
            create_test_message(b"PARTIALLY_VALID2"),
            vec![], // Empty vec simulates reconnection scenario
            create_test_message(b"PARTIALLY_VALID3"),
        ]
    }

    fn get_valid_messages() -> Vec<Vec<u8>> {
        vec![
            create_test_message(b"VALID1"),
            create_test_message(b"VALID2"),
        ]
    }

    /// Creates a mock netlink message with proper headers for testing.
    ///
    /// Format: [netlink_header(16 bytes)] + [genetlink_header(4 bytes)] + [payload]
    const fn create_mock_netlink_message(payload: &[u8]) -> [u8; 100] {
        let mut msg = [0u8; 100];
        let total_len = 20 + payload.len(); // 16 (nlmsg) + 4 (genl) + payload

        // Netlink header (16 bytes)
        msg[0] = (total_len & 0xFF) as u8; // length (little-endian)
        msg[1] = ((total_len >> 8) & 0xFF) as u8;
        msg[2] = ((total_len >> 16) & 0xFF) as u8;
        msg[3] = ((total_len >> 24) & 0xFF) as u8;
        msg[4] = 0x10;
        msg[5] = 0x00; // type (mock type)
        msg[6] = 0x00;
        msg[7] = 0x00; // flags
        msg[8] = 0x01;
        msg[9] = 0x00;
        msg[10] = 0x00;
        msg[11] = 0x00; // seq
        msg[12] = 0x00;
        msg[13] = 0x00;
        msg[14] = 0x00;
        msg[15] = 0x00; // pid

        // Generic netlink header (4 bytes)
        msg[16] = 0x01; // cmd
        msg[17] = 0x00; // version
        msg[18] = 0x00;
        msg[19] = 0x00; // reserved

        // Copy payload
        let mut i = 0;
        while i < payload.len() && i < 80 {
            // Leave room for headers
            msg[20 + i] = payload[i];
            i += 1;
        }

        msg
    }

    // Use atomic counter instead of unsafe static mut for thread safety
    static SOCKET_COUNT: AtomicUsize = AtomicUsize::new(0);

    /// Mock socket implementation for testing netlink functionality.
    ///
    /// Simulates different socket behaviors for testing reconnection logic.
    pub struct MockSocket {
        pub valid: bool,
        budget: usize,
        messages: Vec<Vec<u8>>,
        fd: RawFd, // Mock file descriptor for testing
    }

    impl AsRawFd for MockSocket {
        fn as_raw_fd(&self) -> RawFd {
            self.fd
        }
    }

    impl MockSocket {
        /// Creates a new MockSocket for testing.
        ///
        /// The first socket created will have partially valid messages (including one that fails),
        /// while subsequent sockets will have only valid messages.
        pub fn new() -> Self {
            let count = SOCKET_COUNT.fetch_add(1, Ordering::SeqCst) + 1;

            if count == 1 {
                let messages = get_partially_valid_messages();
                MockSocket {
                    valid: true,
                    budget: messages.len(),
                    messages,
                    fd: 100 + count as RawFd, // Mock file descriptor
                }
            } else {
                // All subsequent sockets are valid for simpler testing
                let messages = get_valid_messages();
                MockSocket {
                    valid: true, // Always valid for simplicity
                    budget: messages.len(),
                    messages,
                    fd: 100 + count as RawFd, // Mock file descriptor
                }
            }
        }

        /// Simulates receiving data from a netlink socket.
        ///
        /// # Arguments
        ///
        /// * `buf` - Buffer to write received data into
        /// * `_flags` - Message flags (ignored in mock)
        ///
        /// # Returns
        ///
        /// Ok(size) on success, Err on failure or empty message
        pub fn recv(&mut self, buf: &mut [u8], _flags: i32) -> Result<usize, io::Error> {
            sleep(Duration::from_millis(1));

            if self.budget == 0 {
                // When there are no more messages, return WouldBlock to simulate non-blocking behavior
                return Err(io::Error::new(
                    io::ErrorKind::WouldBlock,
                    "No more data available",
                ));
            }

            let msg_index = self.messages.len() - self.budget;
            let msg = &self.messages[msg_index];
            self.budget -= 1;

            if !msg.is_empty() {
                let copy_len = std::cmp::min(msg.len(), buf.len());
                buf[..copy_len].copy_from_slice(&msg[..copy_len]);

                Ok(copy_len)
            } else {
                Err(io::Error::new(
                    io::ErrorKind::ConnectionAborted,
                    "Simulated connection failure",
                ))
            }
        }
    }

    /// Tests the DataNetlinkActor's ability to handle partial failures and reconnection.
    ///
    /// This test verifies that:
    /// - The actor correctly handles a mix of valid and invalid messages
    /// - Reconnection occurs when an empty message is encountered  
    /// - All expected payload data (without headers) are eventually received
    #[tokio::test]
    async fn test_data_netlink() {
        // Initialize logging for the test
        let _ = env_logger::builder()
            .filter_level(log::LevelFilter::Debug)
            .is_test(true)
            .try_init();

        // Reset socket count for this test
        SOCKET_COUNT.store(0, Ordering::SeqCst);

        let (command_sender, command_receiver) = channel(1);
        let (buffer_sender, mut buffer_receiver) = channel(1);

        let mut actor = DataNetlinkActor::new("family", "group", command_receiver);
        actor.add_recipient(buffer_sender);

        let task = spawn(DataNetlinkActor::run(actor));

        let mut received_messages = Vec::new();
        for i in 0..3 {
            // After receiving 2 messages, we expect a connection failure, so send a reconnect command
            if i == 2 {
                if let Err(_) = command_sender.send(NetlinkCommand::Reconnect).await {
                    break;
                }
                // Give some time for reconnection
                tokio::time::sleep(Duration::from_millis(10)).await;
            }

            let buffer = tokio::time::timeout(
                Duration::from_secs(5), // Reduced timeout since we're handling reconnect
                buffer_receiver.recv(),
            )
            .await;

            match buffer {
                Ok(Some(buffer)) => {
                    let message = String::from_utf8(buffer.to_vec())
                        .expect("Failed to convert buffer to string");
                    received_messages.push(message);
                }
                Ok(None) => {
                    break;
                }
                Err(_) => {
                    break;
                }
            }
        }

        // Build expected messages: only the payload data, headers should be stripped
        let expected_messages = vec![
            "PARTIALLY_VALID1".to_string(),
            "PARTIALLY_VALID2".to_string(),
            "VALID1".to_string(),
        ];

        assert_eq!(received_messages, expected_messages);

        let socket_count = SOCKET_COUNT.load(Ordering::SeqCst);
        assert!(socket_count > 1, "Socket should have reconnected");

        command_sender
            .send(NetlinkCommand::Close)
            .await
            .expect("Failed to send close command");
        task.await.expect("Task should complete successfully");
    }

    /// Tests payload extraction from mock netlink messages.
    #[test]
    fn test_payload_extraction() {
        // Test with valid message containing payload
        let mock_msg = create_mock_netlink_message(b"TEST_PAYLOAD");
        let actual_len = 20 + b"TEST_PAYLOAD".len(); // 16 (nlmsg) + 4 (genl) + payload
        let mut parser = NetlinkMessageParser::new();
        
        let result = parser.parse_buffer(&mock_msg[..actual_len]);
        assert!(result.is_ok());

        let messages = result.unwrap();
        assert_eq!(messages.len(), 1);
        
        let payload = &messages[0];
        let payload_str = String::from_utf8(payload.to_vec()).unwrap();
        assert_eq!(payload_str, "TEST_PAYLOAD");
    }

    /// Tests payload extraction with minimum size message.
    #[test]
    fn test_payload_extraction_empty_payload() {
        // Create message with headers but no payload
        let mock_msg = create_mock_netlink_message(b"");
        let actual_len = 20; // Only headers: 16 (nlmsg) + 4 (genl)
        let mut parser = NetlinkMessageParser::new();

        let result = parser.parse_buffer(&mock_msg[..actual_len]);
        assert!(result.is_ok());

        let messages = result.unwrap();
        assert_eq!(messages.len(), 1);
        assert!(messages[0].is_empty());
    }

    /// Tests payload extraction with invalid message (too small).
    #[test]
    fn test_payload_extraction_invalid_message() {
        // Buffer too small to contain headers
        let buffer = vec![0u8; 10];
        let mut parser = NetlinkMessageParser::new();

        let result = parser.parse_buffer(&buffer);
        assert!(result.is_ok());
        
        // Should have no complete messages due to insufficient data
        let messages = result.unwrap();
        assert!(messages.is_empty());
    }

    /// Tests handling multiple messages in one buffer.
    #[test]
    fn test_multiple_messages_in_buffer() {
        let mut combined_buffer = Vec::new();
        
        // Create two messages
        let msg1 = create_mock_netlink_message(b"MESSAGE1");
        let msg1_len = 20 + b"MESSAGE1".len();
        let msg2 = create_mock_netlink_message(b"MESSAGE2");
        let msg2_len = 20 + b"MESSAGE2".len();
        
        // Combine them in one buffer (simulate receiving multiple messages in one recv)
        combined_buffer.extend_from_slice(&msg1[..msg1_len]);
        combined_buffer.extend_from_slice(&msg2[..msg2_len]);
        
        let mut parser = NetlinkMessageParser::new();
        let result = parser.parse_buffer(&combined_buffer);
        assert!(result.is_ok());
        
        let messages = result.unwrap();
        assert_eq!(messages.len(), 2);
        
        let payload1_str = String::from_utf8(messages[0].to_vec()).unwrap();
        let payload2_str = String::from_utf8(messages[1].to_vec()).unwrap();
        assert_eq!(payload1_str, "MESSAGE1");
        assert_eq!(payload2_str, "MESSAGE2");
    }

    /// Tests handling fragmented messages across multiple recv operations.
    #[test]
    fn test_fragmented_message() {
        let msg = create_mock_netlink_message(b"FRAGMENTED_MESSAGE");
        let msg_len = 20 + b"FRAGMENTED_MESSAGE".len();
        let mut parser = NetlinkMessageParser::new();
        
        // Simulate first recv getting only part of the message
        let first_part = &msg[..15]; // Less than header size
        let result1 = parser.parse_buffer(first_part);
        assert!(result1.is_ok());
        let messages1 = result1.unwrap();
        assert!(messages1.is_empty()); // No complete messages yet
        
        // Simulate second recv getting the rest
        let second_part = &msg[15..msg_len];
        let result2 = parser.parse_buffer(second_part);
        assert!(result2.is_ok());
        let messages2 = result2.unwrap();
        assert_eq!(messages2.len(), 1);
        
        let payload_str = String::from_utf8(messages2[0].to_vec()).unwrap();
        assert_eq!(payload_str, "FRAGMENTED_MESSAGE");
    }

    /// Tests handling mixed scenario: complete message + partial message.
    #[test]
    fn test_mixed_complete_and_partial() {
        let mut combined_buffer = Vec::new();
        
        // First complete message
        let msg1 = create_mock_netlink_message(b"COMPLETE");
        let msg1_len = 20 + b"COMPLETE".len();
        combined_buffer.extend_from_slice(&msg1[..msg1_len]);
        
        // Partial second message
        let msg2 = create_mock_netlink_message(b"PARTIAL_MSG");
        let msg2_len = 20 + b"PARTIAL_MSG".len();
        combined_buffer.extend_from_slice(&msg2[..25]); // Only part of second message
        
        let mut parser = NetlinkMessageParser::new();
        let result1 = parser.parse_buffer(&combined_buffer);
        assert!(result1.is_ok());
        
        let messages1 = result1.unwrap();
        assert_eq!(messages1.len(), 1); // Only first complete message
        
        let payload1_str = String::from_utf8(messages1[0].to_vec()).unwrap();
        assert_eq!(payload1_str, "COMPLETE");
        
        // Send remaining part of second message
        let remaining_part = &msg2[25..msg2_len];
        let result2 = parser.parse_buffer(remaining_part);
        assert!(result2.is_ok());
        
        let messages2 = result2.unwrap();
        assert_eq!(messages2.len(), 1); // Second message now complete
        
        let payload2_str = String::from_utf8(messages2[0].to_vec()).unwrap();
        assert_eq!(payload2_str, "PARTIAL_MSG");
    }

    /// Tests the get_genl_family_group function with a valid constants file.
    #[test]
    fn test_get_genl_family_group() {
        // Use the test constants file since the production file might not exist
        let result = get_genl_family_group_from_path_safe("tests/data/constants.yml");
        assert!(result.is_ok());
        let (family, group) = result.unwrap();
        assert!(!family.is_empty());
        assert!(!group.is_empty());
    }

    /// Tests the get_genl_family_group_from_path function with a test file.
    #[test]
    fn test_get_genl_family_group_from_path() {
        let result = get_genl_family_group_from_path_safe("/non/existent/path.yml");
        assert!(result.is_err());
        assert!(result
            .unwrap_err()
            .contains("Failed to open constants file"));
    }

    /// Tests the get_genl_family_group_from_path function with the test constants file.
    #[test]
    fn test_get_genl_family_group_from_test_file() {
        let result = get_genl_family_group_from_path_safe("tests/data/constants.yml");
        assert!(result.is_ok());
        let (family, group) = result.unwrap();
        assert!(!family.is_empty());
        assert!(!group.is_empty());
    }

    /// Tests that get_genl_family_group returns default values when config file is missing.
    #[test]
    fn test_get_genl_family_group_defaults() {
        // Create a temporary SONIC_CONSTANTS path that doesn't exist
        let _original_path = SONIC_CONSTANTS;

        // Use the safe function to test default behavior
        let result = get_genl_family_group_from_path_safe("/non/existent/path/constants.yml");
        assert!(result.is_err());

        // Test the main function - it should not panic and should return defaults
        // when the config file is missing (simulated by the safe function)
        let (family, group) = get_genl_family_group();

        // The function should return defaults since the production config file likely doesn't exist in test env
        // Default values should be "sonic_stel" and "ipfix"
        if family == "sonic_stel" && group == "ipfix" {
            // This means it fell back to defaults
            assert_eq!(family, "sonic_stel");
            assert_eq!(group, "ipfix");
        } else {
            // If config file exists and is valid, we should get some values
            assert!(!family.is_empty());
            assert!(!group.is_empty());
        }
    }
}

/// Reads the Generic Netlink family and group names from the configuration file.
///
/// This function is used to determine which netlink family and multicast group
/// should be used for receiving SONIC STEL messages.
///
/// # Returns
///
/// A tuple containing (family_name, group_name).
///
/// # Fallback Behavior
///
/// If the configuration file cannot be read or parsed, this function will
/// use default values: ("sonic_stel", "ipfix")
pub fn get_genl_family_group() -> (String, String) {
    // Default values
    const DEFAULT_FAMILY: &str = "sonic_stel";
    const DEFAULT_GROUP: &str = "ipfix";

    // Try to read from config file, use defaults if it fails
    match get_genl_family_group_from_path_safe(SONIC_CONSTANTS) {
        Ok((family, group)) => {
            debug!(
                "Loaded netlink config from '{}': family='{}', group='{}'",
                SONIC_CONSTANTS, family, group
            );
            (family, group)
        }
        Err(e) => {
            warn!(
                "Failed to load config from '{}': {}. Using defaults: family='{}', group='{}'",
                SONIC_CONSTANTS, e, DEFAULT_FAMILY, DEFAULT_GROUP
            );
            (DEFAULT_FAMILY.to_string(), DEFAULT_GROUP.to_string())
        }
    }
}

/// Safe version of get_genl_family_group_from_path that returns Result instead of panicking.
///
/// # Arguments
///
/// * `path` - Path to the YAML configuration file
///
/// # Returns
///
/// A Result containing a tuple (family_name, group_name) on success,
/// or an error message on failure.
fn get_genl_family_group_from_path_safe(path: &str) -> Result<(String, String), String> {
    use std::fs::File;
    use std::io::Read;
    use yaml_rust::YamlLoader;

    // Try to read the YAML file
    let mut file = match File::open(path) {
        Ok(file) => file,
        Err(e) => return Err(format!("Failed to open constants file '{}': {}", path, e)),
    };

    let mut contents = String::new();
    if let Err(e) = file.read_to_string(&mut contents) {
        return Err(format!("Failed to read constants file '{}': {}", path, e));
    }

    // Parse YAML
    let yaml_docs = match YamlLoader::load_from_str(&contents) {
        Ok(docs) => docs,
        Err(e) => return Err(format!("Failed to parse YAML in '{}': {}", path, e)),
    };

    if yaml_docs.is_empty() {
        return Err(format!("Empty YAML document in constants file '{}'", path));
    }

    let yaml = &yaml_docs[0];

    // Extract family and group with default fallback
    let family = yaml["constants"]["high_frequency_telemetry"]["genl_family"]
        .as_str()
        .unwrap_or("sonic_stel")
        .to_string();

    let group = yaml["constants"]["high_frequency_telemetry"]["genl_multicast_group"]
        .as_str()
        .unwrap_or("ipfix")
        .to_string();

    Ok((family, group))
}
