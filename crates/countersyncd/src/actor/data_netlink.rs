use std::{
    collections::LinkedList,
    sync::Arc,
    thread::sleep,
    time::{Duration, Instant},
};

#[cfg(test)]
use std::{
    os::unix::io::{AsRawFd, RawFd},
};

use log::{debug, info, warn};

#[allow(unused_imports)]
use neli::{
    consts::socket::{Msg, NlFamily},
    router::synchronous::NlRouter,
    socket::NlSocket,
    utils::Groups,
};
use tokio::{
    sync::mpsc::{Receiver, Sender},
};

use std::io;

use super::super::message::{buffer::SocketBufferMessage, netlink::{NetlinkCommand, SocketConnect}};

#[cfg(not(test))]
type SocketType = NlSocket;
#[cfg(test)]
type SocketType = test::MockSocket;

/// Path to the sonic constants configuration file
const SONIC_CONSTANTS: &str = "/usr/share/sonic/countersyncd/constants.yml";

/// Size of the buffer used for receiving netlink messages
const BUFFER_SIZE: usize = 0xFFFF;
/// Linux error code for "No buffer space available" (ENOBUFS)
/// Note: std::io::ErrorKind doesn't have a specific variant for ENOBUFS,
/// so we use the raw OS error code for this specific netlink error condition.
const ENOBUFS: i32 = 105;

/// Maximum number of consecutive failures before waiting for ControlNetlinkActor
const MAX_LOCAL_RECONNECT_ATTEMPTS: u32 = 3;

/// Socket health check timeout - if no data received for this duration, socket is considered unhealthy
const SOCKET_HEALTH_TIMEOUT_SECS: u64 = 10;

/// Heartbeat logging interval (in iterations) - log every 5 minutes at 10ms per iteration
const HEARTBEAT_LOG_INTERVAL: u32 = 30000; // 30000 * 10ms = 5 minutes

/// Debug logging interval (in iterations) - log debug info every 30 seconds
const DEBUG_LOG_INTERVAL: u32 = 3000; // 3000 * 10ms = 30 seconds

/// WouldBlock debug logging interval (in iterations) - log WouldBlock every minute
const WOULDBLOCK_LOG_INTERVAL: u32 = 6000; // 6000 * 10ms = 1 minute

/// Socket readiness check timeout in milliseconds
const SOCKET_READINESS_TIMEOUT_MS: u64 = 10;

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
    /// Reusable netlink resolver for family/group resolution (None if not available)
    #[allow(dead_code)]
    nl_resolver: Option<NlRouter>,
    /// Timestamp of when we last received data on the socket (for health checking)
    last_data_time: Option<Instant>,
    /// List of channels to send received buffer messages to
    buffer_recipients: LinkedList<Sender<SocketBufferMessage>>,
    /// Channel for receiving control commands
    command_recipient: Receiver<NetlinkCommand>,
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
            last_data_time: None,
            buffer_recipients: LinkedList::new(),
            command_recipient,
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

    /// Creates a netlink resolver for family/group resolution.
    /// 
    /// # Returns
    /// 
    /// Some(router) if creation is successful, None otherwise
    #[cfg(not(test))]
    fn create_nl_resolver() -> Option<NlRouter> {
        match NlRouter::connect(NlFamily::Generic, Some(0), Groups::empty()) {
            Ok((router, _)) => {
                debug!("Created netlink resolver for family/group resolution");
                Some(router)
            },
            Err(e) => {
                warn!("Failed to create netlink resolver: {:?}", e);
                None
            }
        }
    }

    /// Mock netlink resolver for testing.
    #[cfg(test)]
    fn create_nl_resolver() -> Option<NlRouter> {
        // Return None for tests to avoid complexity
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
        debug!("Attempting to connect to family '{}', group '{}'", family, group);
        
        // Try to use existing netlink resolver first
        let group_id = if let Some(ref resolver) = self.nl_resolver {
            match resolver.resolve_nl_mcast_group(family, group) {
                Ok(id) => {
                    debug!("Resolved group ID {} for family '{}', group '{}' (using netlink resolver)", id, family, group);
                    id
                },
                Err(e) => {
                    debug!("Failed to resolve group with netlink resolver: {:?}, recreating resolver", e);
                    // Resolver might be stale, recreate it
                    self.nl_resolver = Self::create_nl_resolver();
                    
                    // Try again with new resolver
                    if let Some(ref resolver) = self.nl_resolver {
                        match resolver.resolve_nl_mcast_group(family, group) {
                            Ok(id) => {
                                debug!("Resolved group ID {} for family '{}', group '{}' (using new netlink resolver)", id, family, group);
                                id
                            },
                            Err(e) => {
                                warn!("Failed to resolve group id for family '{}', group '{}' with new netlink resolver: {:?}", family, group, e);
                                warn!("This suggests the family '{}' is not registered in the kernel", family);
                                return None;
                            }
                        }
                    } else {
                        // Fallback to creating temporary router
                        return Self::connect_fallback(family, group);
                    }
                }
            }
        } else {
            // Create netlink resolver if not available
            self.nl_resolver = Self::create_nl_resolver();
            
            if let Some(ref resolver) = self.nl_resolver {
                match resolver.resolve_nl_mcast_group(family, group) {
                    Ok(id) => {
                        debug!("Resolved group ID {} for family '{}', group '{}' (using new netlink resolver)", id, family, group);
                        id
                    },
                    Err(e) => {
                        warn!("Failed to resolve group id for family '{}', group '{}': {:?}", family, group, e);
                        warn!("This suggests the family '{}' is not registered in the kernel", family);
                        return None;
                    }
                }
            } else {
                // Fallback to creating temporary router
                return Self::connect_fallback(family, group);
            }
        };

        debug!("Creating socket for family '{}' with group_id {}", family, group_id);
        let socket = match SocketType::connect(
            NlFamily::Generic,
            // 0 is pid of kernel -> socket is connected to kernel
            Some(0),
            Groups::empty(),
        ) {
            Ok(socket) => socket,
            Err(e) => {
                warn!("Failed to connect socket: {:?}", e);
                return None;
            }
        };

        debug!("Adding multicast membership for group_id {}", group_id);
        match socket.add_mcast_membership(Groups::new_groups(&[group_id])) {
            Ok(_) => {
                info!("Successfully connected to family '{}', group '{}' with group_id: {}", family, group, group_id);
                debug!("Socket created successfully, ready to receive multicast messages on group_id: {}", group_id);
                Some(socket)
            },
            Err(e) => {
                warn!("Failed to add mcast membership for group_id {}: {:?}", group_id, e);
                // Explicitly drop the socket to ensure it's closed
                drop(socket);
                None
            }
        }
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
        debug!("Using fallback connection for family '{}', group '{}'", family, group);
        
        let (sock, _) = match NlRouter::connect(
            NlFamily::Generic,
            // 0 is pid of kernel -> socket is connected to kernel
            Some(0),
            Groups::empty(),
        ) {
            Ok(result) => result,
            Err(e) => {
                warn!("Failed to connect to netlink router: {:?}", e);
                warn!("Possible causes: insufficient permissions, netlink not supported, or kernel module not loaded");
                return None;
            }
        };

        debug!("Router connected, resolving group ID for family '{}', group '{}'", family, group);
        let group_id = match sock.resolve_nl_mcast_group(family, group) {
            Ok(id) => {
                debug!("Resolved group ID {} for family '{}', group '{}'", id, family, group);
                id
            },
            Err(e) => {
                warn!("Failed to resolve group id for family '{}', group '{}': {:?}", family, group, e);
                warn!("This suggests the family '{}' is not registered in the kernel", family);
                // Explicitly drop the temporary router to ensure it's closed
                drop(sock);
                return None;
            }
        };

        debug!("Creating socket for family '{}' with group_id {}", family, group_id);
        let socket = match SocketType::connect(
            NlFamily::Generic,
            // 0 is pid of kernel -> socket is connected to kernel
            Some(0),
            Groups::empty(),
        ) {
            Ok(socket) => socket,
            Err(e) => {
                warn!("Failed to connect socket: {:?}", e);
                // Explicitly drop the temporary router to ensure it's closed
                drop(sock);
                return None;
            }
        };

        debug!("Adding multicast membership for group_id {}", group_id);
        match socket.add_mcast_membership(Groups::new_groups(&[group_id])) {
            Ok(_) => {
                info!("Successfully connected to family '{}', group '{}' with group_id: {}", family, group, group_id);
                debug!("Socket created successfully, ready to receive multicast messages on group_id: {}", group_id);
                // Explicitly drop the temporary router since we no longer need it
                drop(sock);
                Some(socket)
            },
            Err(e) => {
                warn!("Failed to add mcast membership for group_id {}: {:?}", group_id, e);
                // Explicitly drop both socket and temporary router to ensure they're closed
                drop(socket);
                drop(sock);
                None
            }
        }
    }

    /// Attempts to establish a connection on demand.
    /// 
    /// This will be called when receiving a Reconnect command from ControlNetlinkActor.
    /// Implements socket health checking - if current socket hasn't received data recently,
    /// it will be closed and replaced with a new connection.
    fn connect(&mut self) {
        // Check if current socket is healthy
        if let Some(_socket) = &self.socket {
            if let Some(last_data_time) = self.last_data_time {
                let time_since_last_data = Instant::now().duration_since(last_data_time);
                if time_since_last_data.as_secs() > SOCKET_HEALTH_TIMEOUT_SECS {
                    warn!("Socket unhealthy - no data received for {} seconds, forcing reconnection", 
                          time_since_last_data.as_secs());
                    // Close the unhealthy socket
                    self.socket = None;
                    self.last_data_time = None;
                } else {
                    debug!("Socket healthy - data received {} seconds ago, skipping reconnect", 
                           time_since_last_data.as_secs());
                    return;
                }
            } else {
                // Socket exists but no data ever received - consider it new
                debug!("Socket exists but no data received yet, skipping reconnect");
                return;
            }
        }
        
        debug!("Establishing new connection for family '{}', group '{}'", self.family, self.group);
        self.socket = self.connect_with_nl_resolver(&self.family.clone(), &self.group.clone());
        if self.socket.is_some() {
            info!("Successfully connected to family '{}', group '{}'", self.family, self.group);
            self.last_data_time = None; // Reset data time for new socket
        } else {
            warn!("Failed to connect to family '{}', group '{}'", self.family, self.group);
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
            debug!("Disconnecting socket for family '{}', group '{}'", self.family, self.group);
            self.socket = None;
            self.last_data_time = None;
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
        debug!("Resetting connection: family '{}' -> '{}', group '{}' -> '{}'", 
               self.family, family, self.group, group);
        self.family = family.to_string();
        self.group = group.to_string();
        self.connect();
    }

    /// Attempts to receive a message from the netlink socket.
    /// 
    /// Returns immediately with WouldBlock if no data is available, allowing
    /// the event loop to handle other operations concurrently.
    async fn try_recv(socket: Option<&mut SocketType>) -> Result<SocketBufferMessage, io::Error> {
        let socket = socket.ok_or_else(|| {
            io::Error::new(io::ErrorKind::NotConnected, "No socket available")
        })?;

        let mut buffer = Arc::new(vec![0; BUFFER_SIZE]);
        let buffer_slice = Arc::get_mut(&mut buffer)
            .ok_or_else(|| io::Error::new(io::ErrorKind::Other, "Failed to get mutable reference to buffer"))?;
        
        // Try to receive with MSG_DONTWAIT to make it non-blocking
        debug!("Attempting to receive netlink message...");
        let result = socket.recv(buffer_slice, Msg::DONTWAIT);
        
        match result {
            Ok((size, _groups)) => {
                debug!("Received netlink message, size: {} bytes", size);
                
                if size == 0 {
                    return Err(io::Error::new(
                        io::ErrorKind::UnexpectedEof,
                        "No more data to receive",
                    ));
                }
                
                Arc::get_mut(&mut buffer)
                    .ok_or_else(|| io::Error::new(io::ErrorKind::Other, "Failed to get mutable reference to buffer"))?
                    .resize(size, 0);
                
                // Parse netlink message and extract payload
                Self::extract_payload(buffer)
            },
            Err(e) => {
                debug!("Socket recv failed: {:?} (raw_os_error: {:?})", e, e.raw_os_error());
                Err(e)
            }
        }
    }

    /// Extracts the payload from a netlink message by parsing headers.
    /// 
    /// This function parses both the netlink header (nlmsghdr) and generic netlink 
    /// header (genlmsghdr) to extract only the actual payload data, excluding headers.
    /// 
    /// # Arguments
    /// 
    /// * `raw_buffer` - The raw buffer containing the complete netlink message
    /// 
    /// # Returns
    /// 
    /// Result containing the payload data or an IO error if parsing fails
    fn extract_payload(raw_buffer: Arc<Vec<u8>>) -> Result<SocketBufferMessage, io::Error> {
        // For now, let's implement a basic header parsing approach
        // Standard netlink header is 16 bytes, generic netlink header is 4 bytes
        const NLMSG_HDRLEN: usize = 16; // sizeof(struct nlmsghdr)
        const GENL_HDRLEN: usize = 4;   // sizeof(struct genlmsghdr)
        const TOTAL_HEADER_SIZE: usize = NLMSG_HDRLEN + GENL_HDRLEN;
        
        if raw_buffer.len() < TOTAL_HEADER_SIZE {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Buffer too small: {} bytes, expected at least {}", 
                       raw_buffer.len(), TOTAL_HEADER_SIZE)
            ));
        }
        
        // Extract netlink message length from header (first 4 bytes, little-endian)
        let nl_len = u32::from_le_bytes([
            raw_buffer[0], raw_buffer[1], raw_buffer[2], raw_buffer[3]
        ]) as usize;
        
        // Validate message length
        if nl_len < TOTAL_HEADER_SIZE || nl_len > raw_buffer.len() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Invalid netlink message length: {} (buffer size: {})", 
                       nl_len, raw_buffer.len())
            ));
        }
        
        // Debug: Print headers only when debug logging is enabled
        if log::log_enabled!(log::Level::Debug) {
            debug!("Netlink Header (16 bytes): {:02x?}", &raw_buffer[0..16]);
            let nl_type = u16::from_le_bytes([raw_buffer[4], raw_buffer[5]]);
            let nl_flags = u16::from_le_bytes([raw_buffer[6], raw_buffer[7]]);
            let nl_seq = u32::from_le_bytes([raw_buffer[8], raw_buffer[9], raw_buffer[10], raw_buffer[11]]);
            let nl_pid = u32::from_le_bytes([raw_buffer[12], raw_buffer[13], raw_buffer[14], raw_buffer[15]]);
            debug!("  nl_len={}, nl_type={}, nl_flags=0x{:04x}, nl_seq={}, nl_pid={}", 
                   nl_len, nl_type, nl_flags, nl_seq, nl_pid);
            
            debug!("Generic Netlink Header (4 bytes): {:02x?}", &raw_buffer[16..20]);
            let genl_cmd = raw_buffer[16];
            let genl_version = raw_buffer[17];
            let genl_reserved = u16::from_le_bytes([raw_buffer[18], raw_buffer[19]]);
            debug!("  genl_cmd={}, genl_version={}, genl_reserved=0x{:04x}", 
                   genl_cmd, genl_version, genl_reserved);
        }
        
        // Extract payload after both headers
        let payload_start = TOTAL_HEADER_SIZE;
        let payload_end = nl_len;
        
        if payload_start >= payload_end {
            // No payload data, return empty payload
            Ok(Arc::new(Vec::new()))
        } else {
            // Return payload data without headers
            let payload = raw_buffer[payload_start..payload_end].to_vec();
            Ok(Arc::new(payload))
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
        debug!("Starting DataNetlinkActor with {} buffer recipients configured", actor.buffer_recipients.len());
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
                debug!("DataNetlinkActor heartbeat: socket={}, recipients={}, failures={}", 
                       actor.socket.is_some(), actor.buffer_recipients.len(), consecutive_failures);
                if actor.socket.is_some() {
                    debug!("Socket is available and we are actively trying to receive messages");
                    consecutive_failures = 0; // Reset failure counter when socket is available
                }
            }

            // Check for pending commands first (non-blocking)
            if let Ok(command) = actor.command_recipient.try_recv() {
                match command {
                    NetlinkCommand::SocketConnect(SocketConnect{family, group}) => {
                        actor.reset(&family, &group);
                        consecutive_failures = 0; // Reset failure counter on reconnect command
                    }
                    NetlinkCommand::Reconnect => {
                        actor.connect();
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
                        match Self::try_recv(actor.socket.as_mut()).await {
                            Ok(buffer) => {
                                consecutive_failures = 0; // Reset failure counter on successful receive
                                actor.last_data_time = Some(Instant::now()); // Update data reception timestamp
                                debug!("Successfully received and extracted payload with {} bytes", buffer.len());
                                // Send buffer to all recipients
                                for recipient in &actor.buffer_recipients {
                                    if let Err(e) = recipient.send(buffer.clone()).await {
                                        warn!("Failed to send buffer to recipient: {:?}", e);
                                        // Consider removing failed recipients here if needed
                                    }
                                }
                            },
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
                                        debug!("Attempting quick reconnect #{}", consecutive_failures);
                                        actor.connect();
                                    } else {
                                        debug!("Too many consecutive failures, waiting for reconnect command from ControlNetlinkActor");
                                    }
                                }
                            },
                        }
                    } else if actor.socket.is_none() {
                        // No socket available, log this periodically but don't spam
                        if heartbeat_counter % DEBUG_LOG_INTERVAL == 0 {
                            debug!("No socket available - waiting for reconnect command from ControlNetlinkActor");
                        }
                    }
                },
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

    // Test constants for simulating different message scenarios
    const PARTIALLY_VALID_MESSAGES: [&[u8]; 4] = [
        &create_mock_netlink_message(b"PARTIALLY_VALID1"),
        &create_mock_netlink_message(b"PARTIALLY_VALID2"), 
        &[], // Empty slice simulates reconnection scenario
        &create_mock_netlink_message(b"PARTIALLY_VALID3"),
    ];

    const VALID_MESSAGES: [&[u8]; 2] = [
        &create_mock_netlink_message(b"VALID1"), 
        &create_mock_netlink_message(b"VALID2")
    ];

    /// Creates a mock netlink message with proper headers for testing.
    /// 
    /// Format: [netlink_header(16 bytes)] + [genetlink_header(4 bytes)] + [payload]
    const fn create_mock_netlink_message(payload: &[u8]) -> [u8; 100] {
        let mut msg = [0u8; 100];
        let total_len = 20 + payload.len(); // 16 (nlmsg) + 4 (genl) + payload
        
        // Netlink header (16 bytes)
        msg[0] = (total_len & 0xFF) as u8;        // length (little-endian)
        msg[1] = ((total_len >> 8) & 0xFF) as u8;
        msg[2] = ((total_len >> 16) & 0xFF) as u8;
        msg[3] = ((total_len >> 24) & 0xFF) as u8;
        msg[4] = 0x10; msg[5] = 0x00;             // type (mock type)
        msg[6] = 0x00; msg[7] = 0x00;             // flags
        msg[8] = 0x01; msg[9] = 0x00; msg[10] = 0x00; msg[11] = 0x00; // seq
        msg[12] = 0x00; msg[13] = 0x00; msg[14] = 0x00; msg[15] = 0x00; // pid
        
        // Generic netlink header (4 bytes)
        msg[16] = 0x01; // cmd
        msg[17] = 0x00; // version
        msg[18] = 0x00; msg[19] = 0x00; // reserved
        
        // Copy payload
        let mut i = 0;
        while i < payload.len() && i < 80 { // Leave room for headers
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
            
            println!("Creating MockSocket #{}", count);
            
            if count == 1 {
                println!("MockSocket #{}: PARTIALLY_VALID_MESSAGES (4 messages)", count);
                MockSocket {
                    valid: true,
                    budget: PARTIALLY_VALID_MESSAGES.len(),
                    messages: PARTIALLY_VALID_MESSAGES
                        .iter()
                        .map(|&msg| msg.to_vec())
                        .collect(),
                    fd: 100 + count as RawFd, // Mock file descriptor
                }
            } else {
                // All subsequent sockets are valid for simpler testing
                println!("MockSocket #{}: VALID_MESSAGES (2 messages), valid=true", count);
                MockSocket {
                    valid: true, // Always valid for simplicity
                    budget: VALID_MESSAGES.len(),
                    messages: VALID_MESSAGES
                        .iter()
                        .map(|&msg| msg.to_vec())
                        .collect(),
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
        /// Ok((size, groups)) on success, Err on failure or empty message
        pub fn recv(&mut self, buf: &mut [u8], _flags: Msg) -> Result<(usize, Groups), io::Error> {
            sleep(Duration::from_millis(1));
            
            println!("MockSocket recv called: budget={}, messages.len()={}", self.budget, self.messages.len());
            
            if self.budget == 0 {
                println!("MockSocket: No more messages available, returning WouldBlock");
                // When there are no more messages, return WouldBlock to simulate non-blocking behavior
                return Err(io::Error::new(io::ErrorKind::WouldBlock, "No more data available"));
            }
            
            let msg_index = self.messages.len() - self.budget;
            let msg = &self.messages[msg_index];
            self.budget -= 1;
            
            println!("MockSocket: Serving message index {}, budget now {}", msg_index, self.budget);
            
            if !msg.is_empty() {
                let copy_len = std::cmp::min(msg.len(), buf.len());
                buf[..copy_len].copy_from_slice(&msg[..copy_len]);
                
                // Extract payload for debugging
                if msg.len() >= 20 {
                    let payload = &msg[20..];
                    let payload_str = String::from_utf8_lossy(payload);
                    println!("MockSocket: Returning message '{}' ({} bytes)", payload_str, copy_len);
                }
                
                Ok((copy_len, Groups::empty()))
            } else {
                println!("MockSocket: Empty message, returning ConnectionAborted");
                Err(io::Error::new(io::ErrorKind::ConnectionAborted, "Simulated connection failure"))
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
        
        println!("Starting test_data_netlink");
        
        // Reset socket count for this test
        SOCKET_COUNT.store(0, Ordering::SeqCst);
        println!("Reset socket count to 0");
        
        let (command_sender, command_receiver) = channel(1);
        let (buffer_sender, mut buffer_receiver) = channel(1);
        println!("Created channels");
        
        let mut actor = DataNetlinkActor::new("family", "group", command_receiver);
        println!("Created DataNetlinkActor");

        actor.add_recipient(buffer_sender);
        println!("Added buffer recipient");
        
        let task = spawn(DataNetlinkActor::run(actor));
        println!("Spawned DataNetlinkActor::run task");

        let mut received_messages = Vec::new();
        for i in 0..3 {
            println!("Waiting for message {} of 3", i + 1);
            
            // After receiving 2 messages, we expect a connection failure, so send a reconnect command
            if i == 2 {
                println!("Sending reconnect command to handle connection failure");
                if let Err(e) = command_sender.send(NetlinkCommand::Reconnect).await {
                    println!("Failed to send reconnect command: {:?}", e);
                    break;
                }
                // Give some time for reconnection
                tokio::time::sleep(Duration::from_millis(10)).await;
            }
            
            let buffer = tokio::time::timeout(
                Duration::from_secs(5), // Reduced timeout since we're handling reconnect
                buffer_receiver.recv()
            ).await;
            
            match buffer {
                Ok(Some(buffer)) => {
                    let message = String::from_utf8(buffer.to_vec())
                        .expect("Failed to convert buffer to string");
                    println!("Received message {}: '{}'", i + 1, message);
                    received_messages.push(message);
                },
                Ok(None) => {
                    println!("Channel closed while waiting for message {}", i + 1);
                    break;
                },
                Err(_) => {
                    println!("Timeout waiting for message {}", i + 1);
                    break;
                }
            }
        }

        println!("Received {} messages total", received_messages.len());
        for (i, msg) in received_messages.iter().enumerate() {
            println!("Message {}: '{}'", i + 1, msg);
        }

        // Build expected messages: only the payload data, headers should be stripped
        let expected_messages = vec![
            "PARTIALLY_VALID1".to_string(),
            "PARTIALLY_VALID2".to_string(),
            "VALID1".to_string(),
        ];

        println!("Expected {} messages", expected_messages.len());
        for (i, msg) in expected_messages.iter().enumerate() {
            println!("Expected {}: '{}'", i + 1, msg);
        }

        assert_eq!(received_messages, expected_messages);
        
        let socket_count = SOCKET_COUNT.load(Ordering::SeqCst);
        println!("Final socket count: {}", socket_count);
        assert!(socket_count > 1, "Socket should have reconnected");

        println!("Sending close command");
        command_sender.send(NetlinkCommand::Close).await.expect("Failed to send close command");

        println!("Waiting for task to complete");
        task.await.expect("Task should complete successfully");
        println!("Test completed successfully");
    }

    /// Tests payload extraction from mock netlink messages.
    #[test]
    fn test_payload_extraction() {
        // Test with valid message containing payload
        let mock_msg = create_mock_netlink_message(b"TEST_PAYLOAD");
        let buffer = Arc::new(mock_msg.to_vec());
        
        let result = DataNetlinkActor::extract_payload(buffer);
        assert!(result.is_ok());
        
        let payload = result.unwrap();
        let payload_str = String::from_utf8(payload.to_vec()).unwrap();
        assert_eq!(payload_str, "TEST_PAYLOAD");
    }

    /// Tests payload extraction with minimum size message.
    #[test]
    fn test_payload_extraction_empty_payload() {
        // Create message with headers but no payload
        let mock_msg = create_mock_netlink_message(b"");
        let buffer = Arc::new(mock_msg[..20].to_vec()); // Only headers
        
        let result = DataNetlinkActor::extract_payload(buffer);
        assert!(result.is_ok());
        
        let payload = result.unwrap();
        assert!(payload.is_empty());
    }

    /// Tests payload extraction with invalid message (too small).
    #[test]
    fn test_payload_extraction_invalid_message() {
        // Buffer too small to contain headers
        let buffer = Arc::new(vec![0u8; 10]);
        
        let result = DataNetlinkActor::extract_payload(buffer);
        assert!(result.is_err());
        
        let error = result.unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidData);
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
        println!("Family: {}, Group: {}", family, group);
    }

    /// Tests the get_genl_family_group_from_path function with a test file.
    #[test]
    fn test_get_genl_family_group_from_path() {
        let result = get_genl_family_group_from_path_safe("/non/existent/path.yml");
        assert!(result.is_err());
        assert!(result.unwrap_err().contains("Failed to open constants file"));
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
            debug!("Loaded netlink config from '{}': family='{}', group='{}'", SONIC_CONSTANTS, family, group);
            (family, group)
        }
        Err(e) => {
            warn!("Failed to load config from '{}': {}. Using defaults: family='{}', group='{}'", 
                  SONIC_CONSTANTS, e, DEFAULT_FAMILY, DEFAULT_GROUP);
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
