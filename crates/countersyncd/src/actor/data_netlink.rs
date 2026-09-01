use std::{collections::LinkedList, sync::Arc, time::Duration};

use std::os::unix::io::AsRawFd;
#[cfg(test)]
use std::os::unix::io::RawFd;

use log::{debug, info, warn};

use netlink_sys::Socket;
#[cfg(not(test))]
use netlink_sys::{protocols::NETLINK_GENERIC, SocketAddr};
use tokio::{
    io::{unix::AsyncFd, Interest, Ready as TokioReady},
    select,
    sync::mpsc::{Receiver, Sender},
    time::{interval, Instant, MissedTickBehavior},
};

use std::io;

use super::super::message::{
    buffer::SocketBufferMessage,
    netlink::{NetlinkCommand, SocketConnect},
};
#[cfg(not(test))]
use super::netlink_utils;
use crate::utilities::{format_hex_lines, record_comm_stats, ChannelLabel};

#[cfg(not(test))]
type SocketType = Socket;
#[cfg(test)]
type SocketType = test::MockSocket;

/// Path to the sonic constants configuration file
const SONIC_CONSTANTS: &str = "/etc/sonic/constants.yml";

/// Size of the buffer used for receiving netlink messages
#[cfg(test)]
const BUFFER_SIZE: usize = 0x1FFFF;
/// Linux error code for "No buffer space available" (ENOBUFS)
/// Note: std::io::ErrorKind doesn't have a specific variant for ENOBUFS,
/// so we use the raw OS error code for this specific netlink error condition.
const ENOBUFS: i32 = 105;

/// Maximum number of consecutive failures before waiting for ControlNetlinkActor
const MAX_LOCAL_RECONNECT_ATTEMPTS: u32 = 3;

/// Socket health check timeout - if no data received for this duration, socket is considered unhealthy
const SOCKET_HEALTH_TIMEOUT_SECS: u64 = 60;

/// Heartbeat logging interval.
const HEARTBEAT_INTERVAL_SECS: u64 = 5 * 60;

/// Retry interval after a socket cannot be registered with the Tokio reactor.
const SOCKET_REGISTRATION_RETRY_SECS: u64 = 1;

/// Maximum supported size for a single netlink datagram/message.
/// This bounds userspace allocation after peeking the datagram length.
const MAX_NETLINK_DATAGRAM_SIZE: usize = 16 * 1024 * 1024;

/// Netlink message parser for handling multiple messages in one datagram
#[derive(Debug)]
struct NetlinkMessageParser;

impl NetlinkMessageParser {
    fn new() -> Self {
        Self
    }

    /// Mirrors `NLMSG_ALIGN` from `linux/netlink.h` by rounding lengths up to the next 4-byte boundary.
    fn nlmsg_align(len: usize) -> usize {
        (len + 3) & !3
    }

    fn is_valid_alignment_padding(data: &[u8]) -> bool {
        data.len() <= 3 && data.iter().all(|byte| *byte == 0)
    }

    fn return_parsed_or_error(
        complete_messages: Vec<SocketBufferMessage>,
        error: io::Error,
        offset: usize,
        remaining: usize,
    ) -> Result<Vec<SocketBufferMessage>, io::Error> {
        if complete_messages.is_empty() {
            return Err(error);
        }

        warn!(
            "Discarding trailing {} bytes at offset {} after parsing {} message(s): {}",
            remaining,
            offset,
            complete_messages.len(),
            error
        );
        Ok(complete_messages)
    }

    /// Parse a single netlink datagram that may contain one or more complete netlink messages.
    ///
    /// Netlink multicast sockets are datagram-oriented. If a userspace receive buffer is too small,
    /// the kernel discards the rest of that datagram, so bytes from a later recv must never be
    /// treated as a continuation of the previous one.
    /// Returns the generic-netlink payload from each complete netlink message. For IPFIX data,
    /// each payload can contain multiple IPFIX sets and records.
    fn parse_buffer(&mut self, new_data: &[u8]) -> Result<Vec<SocketBufferMessage>, io::Error> {
        let mut complete_messages = Vec::new();
        let mut offset = 0;

        // Parse all complete messages in the buffer
        while offset < new_data.len() {
            // Check if we have enough data for a netlink header
            if offset + 16 > new_data.len() {
                if Self::is_valid_alignment_padding(&new_data[offset..]) {
                    break;
                }

                let remaining = new_data.len() - offset;
                let error = io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!(
                        "Incomplete netlink header at offset {}: have {} bytes",
                        offset, remaining
                    ),
                );
                return Self::return_parsed_or_error(complete_messages, error, offset, remaining);
            }

            // Extract message length from netlink header
            let nl_len = u32::from_le_bytes([
                new_data[offset],
                new_data[offset + 1],
                new_data[offset + 2],
                new_data[offset + 3],
            ]) as usize;

            // Validate message length
            if nl_len < 16 {
                let error = io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("Invalid netlink message length: {} (too small)", nl_len),
                );
                return Self::return_parsed_or_error(
                    complete_messages,
                    error,
                    offset,
                    new_data.len() - offset,
                );
            }

            if nl_len > MAX_NETLINK_DATAGRAM_SIZE {
                let error = io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("Invalid netlink message length: {} (too large)", nl_len),
                );
                return Self::return_parsed_or_error(
                    complete_messages,
                    error,
                    offset,
                    new_data.len() - offset,
                );
            }

            // Check if we have the complete message
            if offset + nl_len > new_data.len() {
                let remaining = new_data.len() - offset;
                let error = io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!(
                        "Incomplete netlink message at offset {}: need {} bytes, have {} bytes",
                        offset, nl_len, remaining
                    ),
                );
                return Self::return_parsed_or_error(complete_messages, error, offset, remaining);
            }

            let aligned_nl_len = Self::nlmsg_align(nl_len);

            // Extract complete message without trailing alignment padding
            let message_data = new_data[offset..offset + nl_len].to_vec();
            debug!(
                "Found complete message: offset={}, length={}, aligned_length={}",
                offset, nl_len, aligned_nl_len
            );

            // Extract payload from this message
            match Self::extract_payload_from_slice(&message_data) {
                Ok(payload) => {
                    debug!(
                        "Successfully extracted payload with {} bytes",
                        payload.len()
                    );
                    complete_messages.push(payload);
                }
                Err(e) => {
                    warn!(
                        "Failed to extract payload from message at offset {}: {}",
                        offset, e
                    );
                    // Continue with next message instead of failing completely
                }
            }

            let remaining = new_data.len() - offset;
            offset += usize::min(aligned_nl_len, remaining);
        }

        Ok(complete_messages)
    }

    /// Extract payload from a single complete netlink message
    fn extract_payload_from_slice(message_data: &[u8]) -> Result<SocketBufferMessage, io::Error> {
        const NLMSG_HDRLEN: usize = 16; // sizeof(struct nlmsghdr)
        const GENL_HDRLEN: usize = 4; // sizeof(struct genlmsghdr)
        const TOTAL_HEADER_SIZE: usize = NLMSG_HDRLEN + GENL_HDRLEN;
        const NLMSG_LEN: std::ops::Range<usize> = 0..4;
        const NLMSG_TYPE: std::ops::Range<usize> = 4..6;
        const NLMSG_FLAGS: std::ops::Range<usize> = 6..8;
        const NLMSG_SEQ: std::ops::Range<usize> = 8..12;
        const NLMSG_PID: std::ops::Range<usize> = 12..16;
        const GENL_CMD: usize = 16;
        const GENL_VERSION: usize = 17;
        const GENL_RESERVED: std::ops::Range<usize> = 18..20;

        if message_data.len() < TOTAL_HEADER_SIZE {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "Message too small: {} bytes, expected at least {}",
                    message_data.len(),
                    TOTAL_HEADER_SIZE
                ),
            ));
        }

        // Extract netlink message length from header
        let nl_len = u32::from_le_bytes(message_data[NLMSG_LEN].try_into().unwrap()) as usize;

        if nl_len != message_data.len() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "Message length mismatch: header says {}, actual {}",
                    nl_len,
                    message_data.len()
                ),
            ));
        }

        // Debug: Print headers only when debug logging is enabled
        if log::log_enabled!(log::Level::Debug) {
            debug!(
                "Netlink Header ({} bytes): {:02x?}",
                NLMSG_HDRLEN,
                &message_data[..NLMSG_HDRLEN]
            );
            let nl_type = u16::from_le_bytes(message_data[NLMSG_TYPE].try_into().unwrap());
            let nl_flags = u16::from_le_bytes(message_data[NLMSG_FLAGS].try_into().unwrap());
            let nl_seq = u32::from_le_bytes(message_data[NLMSG_SEQ].try_into().unwrap());
            let nl_pid = u32::from_le_bytes(message_data[NLMSG_PID].try_into().unwrap());
            debug!(
                "  nl_len={}, nl_type={}, nl_flags=0x{:04x}, nl_seq={}, nl_pid={}",
                nl_len, nl_type, nl_flags, nl_seq, nl_pid
            );

            if message_data.len() >= TOTAL_HEADER_SIZE {
                debug!(
                    "Generic Netlink Header ({} bytes): {:02x?}",
                    GENL_HDRLEN,
                    &message_data[NLMSG_HDRLEN..TOTAL_HEADER_SIZE]
                );
                let genl_cmd = message_data[GENL_CMD];
                let genl_version = message_data[GENL_VERSION];
                let genl_reserved =
                    u16::from_le_bytes(message_data[GENL_RESERVED].try_into().unwrap());
                debug!(
                    "  genl_cmd={}, genl_version={}, genl_reserved=0x{:04x}",
                    genl_cmd, genl_version, genl_reserved
                );
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
    /// Message parser for handling one or more netlink messages in each datagram
    message_parser: NetlinkMessageParser,
    /// Netlink socket receive buffer size in bytes (0 = OS default). Reduces ENOBUFS when set.
    netlink_rcvbuf_bytes: usize,
}

impl DataNetlinkActor {
    fn register_active_socket(&mut self) -> Option<AsyncFd<SocketType>> {
        let socket = self.socket.take()?;
        let fd = socket.as_raw_fd();
        match AsyncFd::try_with_interest(socket, Interest::READABLE | Interest::ERROR) {
            Ok(socket) => {
                #[cfg(test)]
                test::record_socket_registration();
                debug!("Registered data netlink socket fd {} with Tokio", fd);
                Some(socket)
            }
            Err(e) => {
                let (socket, cause) = e.into_parts();
                warn!(
                    "Failed to register data netlink socket fd {} with Tokio: {:?}",
                    fd, cause
                );
                self.socket = Some(socket);
                None
            }
        }
    }

    fn unregister_socket(socket: &mut Option<AsyncFd<SocketType>>) -> Option<SocketType> {
        socket.take().map(|socket| socket.into_inner())
    }

    fn recvmsg_into(
        fd: std::os::unix::io::RawFd,
        buffer: &mut [u8],
        flags: i32,
    ) -> Result<(usize, i32), io::Error> {
        let mut iov = libc::iovec {
            iov_base: buffer.as_mut_ptr() as *mut libc::c_void,
            iov_len: buffer.len(),
        };
        let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
        msg.msg_iov = &mut iov;
        msg.msg_iovlen = 1;

        // Safe on the Tokio worker because the fd is configured as non-blocking by connect().
        let size = unsafe { libc::recvmsg(fd, &mut msg, flags) };
        if size < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok((size as usize, msg.msg_flags))
        }
    }

    fn recv_datagram_fd(
        fd: std::os::unix::io::RawFd,
        max_size: usize,
    ) -> Result<Vec<u8>, io::Error> {
        let mut peek_buffer = [0u8; 1];
        // Peek first to size the receive buffer exactly and to make truncation observable. A fixed
        // large buffer would either waste memory in the hot path or still silently truncate when a
        // producer emits a larger datagram than expected.
        let (needed, peek_flags) =
            Self::recvmsg_into(fd, &mut peek_buffer, libc::MSG_PEEK | libc::MSG_TRUNC)?;

        if needed == 0 {
            let _ = Self::recvmsg_into(fd, &mut peek_buffer, 0);
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "Received empty netlink datagram",
            ));
        }

        if needed > max_size {
            let mut drain_buffer = [0u8; 1];
            if let Err(err) = Self::recvmsg_into(fd, &mut drain_buffer, libc::MSG_TRUNC) {
                return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!(
                        "Failed to drain oversized netlink datagram of {} bytes: {}",
                        needed, err
                    ),
                ));
            }
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "Netlink datagram length {} exceeds maximum supported {} bytes",
                    needed, max_size
                ),
            ));
        }

        let mut buffer = vec![0u8; needed];
        let (size, flags) = Self::recvmsg_into(fd, &mut buffer, 0)?;
        if flags & libc::MSG_TRUNC != 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "Truncated netlink datagram: buffer={} bytes, received={} bytes, peek_flags=0x{:x}",
                    buffer.len(),
                    size,
                    peek_flags
                ),
            ));
        }

        buffer.truncate(size);
        Ok(buffer)
    }

    fn recv_netlink_datagram(socket: &mut SocketType) -> Result<Vec<u8>, io::Error> {
        #[cfg(test)]
        socket.prepare_recv()?;

        Self::recv_datagram_fd(socket.as_raw_fd(), MAX_NETLINK_DATAGRAM_SIZE)
    }

    /// Creates a new DataNetlinkActor instance.
    ///
    /// # Arguments
    ///
    /// * `family` - The generic netlink family name
    /// * `group` - The multicast group name
    /// * `command_recipient` - Channel for receiving control commands
    /// * `netlink_rcvbuf_bytes` - Socket SO_RCVBUF size in bytes (0 = OS default). Larger values reduce ENOBUFS under high HFT load.
    ///
    /// # Returns
    ///
    /// A new DataNetlinkActor instance with an initial connection attempt
    pub fn new(
        family: &str,
        group: &str,
        command_recipient: Receiver<NetlinkCommand>,
        netlink_rcvbuf_bytes: usize,
    ) -> Self {
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
            netlink_rcvbuf_bytes,
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
                        return Self::connect_fallback(family, group, self.netlink_rcvbuf_bytes);
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
                return Self::connect_fallback(family, group, self.netlink_rcvbuf_bytes);
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

        netlink_utils::set_socket_rcvbuf(&socket, self.netlink_rcvbuf_bytes);

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
        debug!(
            "Socket created successfully, ready to receive multicast messages on group_id: {}",
            group_id
        );
        Some(socket)
    }

    /// Mock connection method using shared router for testing.
    #[cfg(test)]
    fn connect_with_nl_resolver(&mut self, family: &str, group: &str) -> Option<SocketType> {
        test::record_connection_attempt();
        let Some((family_id, group_id)) = test::resolve_mock_family(family, group) else {
            debug!(
                "Test: family '{}', group '{}' is unavailable, connection failed",
                family, group
            );
            return None;
        };

        let sock = SocketType::new(family, group, family_id, group_id);
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
    fn connect_fallback(
        family: &str,
        group: &str,
        netlink_rcvbuf_bytes: usize,
    ) -> Option<SocketType> {
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
                if let Err(e) = netlink_utils::set_socket_recv_timeout(&s, Duration::from_secs(2)) {
                    warn!("Failed to set temporary resolver receive timeout: {:?}", e);
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
        let group_id = match netlink_utils::resolve_multicast_group(&mut temp_socket, family, group)
        {
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

        netlink_utils::set_socket_rcvbuf(&socket, netlink_rcvbuf_bytes);

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
        debug!(
            "Socket created successfully, ready to receive multicast messages on group_id: {}",
            group_id
        );
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
                if time_since_last_data >= Duration::from_secs(SOCKET_HEALTH_TIMEOUT_SECS) {
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
    /// 3. Truncated or malformed datagrams, which are rejected without splicing future recv data
    fn try_recv(
        socket: &mut SocketType,
        message_parser: &mut NetlinkMessageParser,
    ) -> Result<Vec<SocketBufferMessage>, io::Error> {
        // Try to receive with non-blocking mode (socket should already be set to non-blocking)
        debug!("Attempting to receive netlink message...");
        let recv_result = Self::recv_netlink_datagram(socket);

        match recv_result {
            Ok(buffer) => {
                let size = buffer.len();
                debug!("Received netlink data, size: {} bytes", size);

                if size == 0 {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "Received empty netlink datagram",
                    ));
                }

                if log::log_enabled!(log::Level::Debug) {
                    let hex_dump = format_hex_lines(&buffer);
                    debug!("Raw netlink recv buffer ({} bytes):\n{}", size, hex_dump);
                }

                // Parse buffer which may contain multiple messages and/or incomplete messages
                let messages = message_parser.parse_buffer(&buffer)?;
                debug!(
                    "Parsed {} complete messages from {} bytes of data",
                    messages.len(),
                    size
                );

                Ok(messages)
            }
            Err(err) => {
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

    fn handle_command(&mut self, command: NetlinkCommand) -> bool {
        match command {
            NetlinkCommand::SocketConnect(SocketConnect { family, group }) => {
                self.reset(&family, &group);
            }
            NetlinkCommand::Reconnect => {
                self.connect(true);
            }
            NetlinkCommand::SoftReconnect => {
                self.connect(false);
            }
            NetlinkCommand::Close => return false,
        }

        true
    }

    /// Continuously processes incoming netlink messages and control commands.
    /// The loop will exit when the command channel is closed or a Close command is received.
    ///
    /// # Arguments
    ///
    /// * `actor` - The DataNetlinkActor instance to run
    pub async fn run(mut actor: DataNetlinkActor) {
        enum ActorEvent {
            Command(Option<NetlinkCommand>),
            SocketRead(Result<Option<Vec<SocketBufferMessage>>, io::Error>),
            Heartbeat,
        }

        debug!(
            "Starting DataNetlinkActor with {} buffer recipients configured",
            actor.buffer_recipients.len()
        );
        let mut consecutive_failures = 0u32;
        let mut heartbeat_interval = interval(Duration::from_secs(HEARTBEAT_INTERVAL_SECS));
        heartbeat_interval.set_missed_tick_behavior(MissedTickBehavior::Delay);
        heartbeat_interval.tick().await;
        let mut socket = actor.register_active_socket();

        loop {
            if socket.is_none() {
                select! {
                    command = actor.command_recipient.recv() => {
                        let Some(command) = command else {
                            break;
                        };
                        record_comm_stats(
                            ChannelLabel::ControlNetlinkToDataNetlink,
                            actor.command_recipient.len(),
                        );
                        if !actor.handle_command(command) {
                            break;
                        }
                        socket = actor.register_active_socket();
                        consecutive_failures = 0;
                    }
                    _ = heartbeat_interval.tick() => {
                        info!("DataNetlinkActor is running without a data socket - waiting for reconnect");
                    }
                    _ = tokio::time::sleep(Duration::from_secs(SOCKET_REGISTRATION_RETRY_SECS)), if actor.socket.is_some() => {
                        socket = actor.register_active_socket();
                    }
                }
                continue;
            }

            let event = select! {
                biased;
                command = actor.command_recipient.recv() => ActorEvent::Command(command),
                readiness = socket
                    .as_mut()
                    .unwrap()
                    .ready_mut(Interest::READABLE | Interest::ERROR) => {
                    match readiness {
                        Ok(mut guard) => {
                            match Self::try_recv(guard.get_inner_mut(), &mut actor.message_parser) {
                                Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                                    guard.clear_ready_matching(TokioReady::READABLE);
                                    guard.clear_ready_matching(TokioReady::ERROR);
                                    ActorEvent::SocketRead(Ok(None))
                                }
                                Err(e) if e.raw_os_error() == Some(ENOBUFS) => {
                                    guard.clear_ready_matching(TokioReady::ERROR);
                                    ActorEvent::SocketRead(Err(e))
                                }
                                result => ActorEvent::SocketRead(result.map(Some)),
                            }
                        }
                        Err(e) => {
                            actor.socket = Self::unregister_socket(&mut socket);
                            ActorEvent::SocketRead(Err(e))
                        }
                    }
                }
                _ = heartbeat_interval.tick() => ActorEvent::Heartbeat,
            };

            match event {
                ActorEvent::Command(command) => {
                    actor.socket = Self::unregister_socket(&mut socket);
                    let Some(command) = command else {
                        break;
                    };
                    record_comm_stats(
                        ChannelLabel::ControlNetlinkToDataNetlink,
                        actor.command_recipient.len(),
                    );
                    if !actor.handle_command(command) {
                        break;
                    }
                    socket = actor.register_active_socket();
                    consecutive_failures = 0;
                }
                ActorEvent::SocketRead(result) => {
                    match result {
                        Ok(Some(messages)) => {
                            consecutive_failures = 0;
                            actor.last_data_time = Instant::now();

                            if messages.is_empty() {
                                debug!("Received netlink datagram but no complete payload was extracted");
                            } else {
                                debug!(
                                    "Successfully parsed {} complete netlink messages",
                                    messages.len()
                                );

                                for (i, message) in messages.iter().enumerate() {
                                    if log::log_enabled!(log::Level::Debug) {
                                        let hex_dump = format_hex_lines(message.as_ref());
                                        debug!(
                                            "Outgoing netlink payload {}/{} ({} bytes):\n{}",
                                            i + 1,
                                            messages.len(),
                                            message.len(),
                                            hex_dump
                                        );
                                    }
                                    debug!(
                                        "Processing netlink message {}/{}: {} bytes",
                                        i + 1,
                                        messages.len(),
                                        message.len()
                                    );

                                    for (j, recipient) in actor.buffer_recipients.iter().enumerate()
                                    {
                                        debug!(
                                            "Sending netlink message {}/{} to recipient {}",
                                            i + 1,
                                            messages.len(),
                                            j + 1
                                        );
                                        if let Err(e) = recipient.send(message.clone()).await {
                                            warn!("Failed to send netlink message {}/{} to recipient {}: {:?}",
                                              i + 1, messages.len(), j + 1, e);
                                        } else {
                                            debug!("Successfully sent netlink message {}/{} ({} bytes) to recipient {}",
                                               i + 1, messages.len(), message.len(), j + 1);
                                        }
                                    }
                                }

                                debug!("Completed processing {} netlink messages, each sent individually", messages.len());
                            }
                        }
                        Ok(None) => {}
                        Err(e) if e.raw_os_error() == Some(ENOBUFS) => {
                            warn!(
                            "Netlink receive buffer full (ENOBUFS). Consider increasing --netlink-rcvbuf or reducing HFT load. Error: {:?}",
                            e
                        );
                        }
                        Err(e) if e.kind() == io::ErrorKind::InvalidData => {
                            warn!("Dropping invalid netlink datagram: {:?}", e);
                        }
                        Err(e) => {
                            warn!("Failed to receive message: {:?}", e);
                            if actor.socket.is_none() {
                                actor.socket = Self::unregister_socket(&mut socket);
                            }
                            actor.disconnect();
                            consecutive_failures += 1;
                            if consecutive_failures <= MAX_LOCAL_RECONNECT_ATTEMPTS {
                                debug!("Attempting quick reconnect #{}", consecutive_failures);
                                actor.connect(true);
                                socket = actor.register_active_socket();
                            } else {
                                debug!("Too many consecutive failures, waiting for reconnect command from ControlNetlinkActor");
                            }
                        }
                    }
                }
                ActorEvent::Heartbeat => {
                    info!("DataNetlinkActor is running normally - waiting for data messages");
                    debug!(
                        "DataNetlinkActor heartbeat: socket=true, recipients={}, failures={}",
                        actor.buffer_recipients.len(),
                        consecutive_failures
                    );
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
    use std::os::unix::net::UnixDatagram;
    use std::sync::{
        atomic::{AtomicBool, AtomicUsize, Ordering},
        Mutex,
    };
    use tokio::{spawn, sync::mpsc::channel};

    // Helper function to create a properly sized message vector
    fn create_test_message_for_family(payload: &[u8], family_id: u16) -> Vec<u8> {
        let mut msg = create_mock_netlink_message(payload);
        msg[4..6].copy_from_slice(&family_id.to_le_bytes());
        let actual_len = 20 + payload.len(); // 16 (nlmsg) + 4 (genl) + payload
        msg[..actual_len].to_vec()
    }

    fn create_large_mock_netlink_message(payload: &[u8]) -> Vec<u8> {
        let total_len = 20 + payload.len();
        let mut msg = vec![0u8; total_len];
        msg[0..4].copy_from_slice(&(total_len as u32).to_le_bytes());
        msg[4..6].copy_from_slice(&0x10u16.to_le_bytes());
        msg[16] = 0x01;
        msg[20..].copy_from_slice(payload);
        msg
    }

    // Test constants for simulating different message scenarios
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

    fn append_aligned_mock_netlink_message(buffer: &mut Vec<u8>, payload: &[u8]) {
        let msg = create_mock_netlink_message(payload);
        let msg_len = 20 + payload.len();
        let aligned_len = NetlinkMessageParser::nlmsg_align(msg_len);

        buffer.extend_from_slice(&msg[..msg_len]);
        buffer.resize(buffer.len() + (aligned_len - msg_len), 0);
    }

    // Use atomic counter instead of unsafe static mut for thread safety
    static SOCKET_COUNT: AtomicUsize = AtomicUsize::new(0);
    static FAIL_NEXT_SOCKET: AtomicBool = AtomicBool::new(false);
    static EMPTY_NEXT_SOCKET: AtomicBool = AtomicBool::new(false);
    static MESSAGES_NEXT_SOCKET: AtomicUsize = AtomicUsize::new(1);
    static RECV_ATTEMPTS: AtomicUsize = AtomicUsize::new(0);
    static REGISTERED_SOCKET_COUNT: AtomicUsize = AtomicUsize::new(0);
    static CONNECTION_ATTEMPTS: AtomicUsize = AtomicUsize::new(0);
    static NEXT_SOCKET_ID: AtomicUsize = AtomicUsize::new(1);
    static AUTO_SEED_SOCKETS: AtomicBool = AtomicBool::new(true);
    static FAIL_SOCKET_ID: AtomicUsize = AtomicUsize::new(0);
    static MOCK_KERNEL_FAMILY: Mutex<Option<MockFamily>> = Mutex::new(None);
    static ALLOW_ANY_FAMILY: AtomicBool = AtomicBool::new(true);
    static LIVE_SUBSCRIPTIONS: Mutex<Vec<MockSubscription>> = Mutex::new(Vec::new());
    static SUCCESSFUL_CONNECTIONS: Mutex<Vec<(u16, u32)>> = Mutex::new(Vec::new());

    #[derive(Clone, Debug, Eq, PartialEq)]
    struct MockFamily {
        name: String,
        group: String,
        family_id: u16,
        group_id: u32,
    }

    struct MockSubscription {
        socket_id: usize,
        family_id: u16,
        group_id: u32,
        sender: UnixDatagram,
    }

    pub(super) fn record_socket_registration() {
        REGISTERED_SOCKET_COUNT.fetch_add(1, Ordering::SeqCst);
    }

    pub(super) fn record_connection_attempt() {
        CONNECTION_ATTEMPTS.fetch_add(1, Ordering::SeqCst);
    }

    pub(super) fn resolve_mock_family(family: &str, group: &str) -> Option<(u16, u32)> {
        if ALLOW_ANY_FAMILY.load(Ordering::SeqCst) {
            return Some((0x20, 0x100));
        }

        MOCK_KERNEL_FAMILY
            .lock()
            .unwrap()
            .as_ref()
            .filter(|entry| entry.name == family && entry.group == group)
            .map(|entry| (entry.family_id, entry.group_id))
    }

    pub(crate) fn reset_mock_state(empty_next_socket: bool, messages_next_socket: usize) {
        assert!(
            LIVE_SUBSCRIPTIONS.lock().unwrap().is_empty(),
            "cannot reset mock state while data sockets are active"
        );
        SOCKET_COUNT.store(0, Ordering::SeqCst);
        FAIL_NEXT_SOCKET.store(false, Ordering::SeqCst);
        EMPTY_NEXT_SOCKET.store(empty_next_socket, Ordering::SeqCst);
        MESSAGES_NEXT_SOCKET.store(messages_next_socket, Ordering::SeqCst);
        RECV_ATTEMPTS.store(0, Ordering::SeqCst);
        REGISTERED_SOCKET_COUNT.store(0, Ordering::SeqCst);
        CONNECTION_ATTEMPTS.store(0, Ordering::SeqCst);
        AUTO_SEED_SOCKETS.store(true, Ordering::SeqCst);
        FAIL_SOCKET_ID.store(0, Ordering::SeqCst);
        ALLOW_ANY_FAMILY.store(true, Ordering::SeqCst);
        *MOCK_KERNEL_FAMILY.lock().unwrap() = None;
        SUCCESSFUL_CONNECTIONS.lock().unwrap().clear();
    }

    pub(crate) fn use_mock_kernel_registry() {
        ALLOW_ANY_FAMILY.store(false, Ordering::SeqCst);
        AUTO_SEED_SOCKETS.store(false, Ordering::SeqCst);
        *MOCK_KERNEL_FAMILY.lock().unwrap() = None;
    }

    pub(crate) fn register_mock_family(family: &str, group: &str, family_id: u16, group_id: u32) {
        ALLOW_ANY_FAMILY.store(false, Ordering::SeqCst);
        *MOCK_KERNEL_FAMILY.lock().unwrap() = Some(MockFamily {
            name: family.to_string(),
            group: group.to_string(),
            family_id,
            group_id,
        });
    }

    pub(crate) fn unregister_mock_family(family: &str, family_id: u16) {
        let mut registry = MOCK_KERNEL_FAMILY.lock().unwrap();
        if registry
            .as_ref()
            .is_some_and(|entry| entry.name == family && entry.family_id == family_id)
        {
            *registry = None;
        }
    }

    pub(crate) fn mock_family_exists(family: &str) -> bool {
        if ALLOW_ANY_FAMILY.load(Ordering::SeqCst) {
            return true;
        }

        MOCK_KERNEL_FAMILY
            .lock()
            .unwrap()
            .as_ref()
            .is_some_and(|entry| entry.name == family)
    }

    pub(crate) fn connection_attempts() -> usize {
        CONNECTION_ATTEMPTS.load(Ordering::SeqCst)
    }

    pub(crate) fn registered_socket_count() -> usize {
        REGISTERED_SOCKET_COUNT.load(Ordering::SeqCst)
    }

    pub(crate) fn live_socket_count() -> usize {
        LIVE_SUBSCRIPTIONS.lock().unwrap().len()
    }

    pub(crate) fn current_subscription() -> Option<(u16, u32)> {
        LIVE_SUBSCRIPTIONS
            .lock()
            .unwrap()
            .last()
            .map(|subscription| (subscription.family_id, subscription.group_id))
    }

    pub(crate) fn successful_connections() -> Vec<(u16, u32)> {
        SUCCESSFUL_CONNECTIONS.lock().unwrap().clone()
    }

    pub(crate) fn send_kernel_data(family_id: u16, group_id: u32, payload: &[u8]) -> bool {
        LIVE_SUBSCRIPTIONS
            .lock()
            .unwrap()
            .iter()
            .filter(|subscription| {
                subscription.family_id == family_id && subscription.group_id == group_id
            })
            .fold(false, |sent, subscription| {
                subscription.send(payload) || sent
            })
    }

    pub(crate) fn fail_current_socket_on_next_recv() {
        let socket_id = LIVE_SUBSCRIPTIONS
            .lock()
            .unwrap()
            .last()
            .expect("mock data socket is connected")
            .socket_id;
        FAIL_SOCKET_ID.store(socket_id, Ordering::SeqCst);
    }

    /// Mock socket backed by a real datagram fd so tests exercise Tokio readiness registration.
    pub struct MockSocket {
        pub valid: bool,
        socket: UnixDatagram,
        _sender: UnixDatagram,
        fail_on_recv: bool,
        socket_id: usize,
    }

    impl AsRawFd for MockSocket {
        fn as_raw_fd(&self) -> RawFd {
            self.socket.as_raw_fd()
        }
    }

    impl MockSocket {
        pub fn new(family: &str, group: &str, family_id: u16, group_id: u32) -> Self {
            let count = SOCKET_COUNT.fetch_add(1, Ordering::SeqCst) + 1;
            let socket_id = NEXT_SOCKET_ID.fetch_add(1, Ordering::SeqCst);
            let (sender, socket) = UnixDatagram::pair().expect("create mock socket pair");
            socket
                .set_nonblocking(true)
                .expect("set mock socket nonblocking");
            LIVE_SUBSCRIPTIONS.lock().unwrap().push(MockSubscription {
                socket_id,
                family_id,
                group_id,
                sender: sender.try_clone().expect("clone mock sender"),
            });
            SUCCESSFUL_CONNECTIONS
                .lock()
                .unwrap()
                .push((family_id, group_id));
            let fail_on_recv = FAIL_NEXT_SOCKET.swap(false, Ordering::SeqCst);
            let message_count = MESSAGES_NEXT_SOCKET.swap(1, Ordering::SeqCst);
            if AUTO_SEED_SOCKETS.load(Ordering::SeqCst)
                && !EMPTY_NEXT_SOCKET.swap(false, Ordering::SeqCst)
            {
                for message_index in 1..=message_count {
                    let payload = if message_count == 1 {
                        format!("{family}/{group}/socket-{count}")
                    } else {
                        format!("{family}/{group}/message-{message_index}")
                    };
                    sender
                        .send(&create_test_message_for_family(
                            payload.as_bytes(),
                            family_id,
                        ))
                        .expect("seed mock socket");
                }
            }

            Self {
                valid: true,
                socket,
                _sender: sender,
                fail_on_recv,
                socket_id,
            }
        }

        pub(super) fn prepare_recv(&mut self) -> Result<(), io::Error> {
            RECV_ATTEMPTS.fetch_add(1, Ordering::SeqCst);
            let injected_failure = FAIL_SOCKET_ID
                .compare_exchange(self.socket_id, 0, Ordering::SeqCst, Ordering::SeqCst)
                .is_ok();
            if self.fail_on_recv || injected_failure {
                self.fail_on_recv = false;
                return Err(io::Error::new(
                    io::ErrorKind::ConnectionAborted,
                    "simulated data socket failure",
                ));
            }
            Ok(())
        }
    }

    impl Drop for MockSocket {
        fn drop(&mut self) {
            LIVE_SUBSCRIPTIONS
                .lock()
                .unwrap()
                .retain(|entry| entry.socket_id != self.socket_id);
        }
    }

    impl MockSubscription {
        fn send(&self, payload: &[u8]) -> bool {
            self.sender
                .send(&create_test_message_for_family(payload, self.family_id))
                .is_ok()
        }
    }

    async fn wait_for_socket_registrations(expected: usize) {
        tokio::time::timeout(Duration::from_secs(1), async {
            while REGISTERED_SOCKET_COUNT.load(Ordering::SeqCst) < expected {
                tokio::task::yield_now().await;
            }
        })
        .await
        .expect("socket was not registered with Tokio");
    }

    fn send_to_current_socket(payload: &[u8]) {
        assert!(LIVE_SUBSCRIPTIONS
            .lock()
            .unwrap()
            .last()
            .expect("mock socket sender is available")
            .send(payload));
    }

    async fn recv_payload(receiver: &mut Receiver<SocketBufferMessage>) -> String {
        let buffer = tokio::time::timeout(Duration::from_secs(1), receiver.recv())
            .await
            .expect("data socket readiness timed out")
            .expect("data channel closed");
        String::from_utf8(buffer.to_vec()).expect("payload is valid UTF-8")
    }

    /// Verifies that every replacement socket is registered with Tokio after repeated family
    /// changes, an explicit reconnect, and an automatic reconnect after a receive failure.
    #[tokio::test]
    #[serial_test::serial]
    async fn test_data_netlink_reregisters_replacement_sockets() {
        let _ = env_logger::builder()
            .filter_level(log::LevelFilter::Debug)
            .is_test(true)
            .try_init();

        reset_mock_state(false, 1);

        let (command_sender, command_receiver) = channel(4);
        let (buffer_sender, mut buffer_receiver) = channel(1);

        let mut actor = DataNetlinkActor::new("family-1", "group-1", command_receiver, 0);
        actor.add_recipient(buffer_sender);

        let task = spawn(DataNetlinkActor::run(actor));

        assert_eq!(
            recv_payload(&mut buffer_receiver).await,
            "family-1/group-1/socket-1"
        );

        command_sender
            .send(NetlinkCommand::SoftReconnect)
            .await
            .unwrap();
        wait_for_socket_registrations(2).await;
        assert_eq!(SOCKET_COUNT.load(Ordering::SeqCst), 1);
        send_to_current_socket(b"after-soft-reconnect");
        assert_eq!(
            recv_payload(&mut buffer_receiver).await,
            "after-soft-reconnect"
        );

        command_sender
            .send(NetlinkCommand::SocketConnect(SocketConnect {
                family: "family-2".to_string(),
                group: "group-2".to_string(),
            }))
            .await
            .unwrap();
        assert_eq!(
            recv_payload(&mut buffer_receiver).await,
            "family-2/group-2/socket-2"
        );

        command_sender
            .send(NetlinkCommand::SocketConnect(SocketConnect {
                family: "family-3".to_string(),
                group: "group-3".to_string(),
            }))
            .await
            .unwrap();
        assert_eq!(
            recv_payload(&mut buffer_receiver).await,
            "family-3/group-3/socket-3"
        );

        FAIL_NEXT_SOCKET.store(true, Ordering::SeqCst);
        command_sender
            .send(NetlinkCommand::Reconnect)
            .await
            .unwrap();
        assert_eq!(
            recv_payload(&mut buffer_receiver).await,
            "family-3/group-3/socket-5"
        );

        command_sender
            .send(NetlinkCommand::Reconnect)
            .await
            .unwrap();
        assert_eq!(
            recv_payload(&mut buffer_receiver).await,
            "family-3/group-3/socket-6"
        );

        let socket_count = SOCKET_COUNT.load(Ordering::SeqCst);
        assert_eq!(socket_count, 6);

        command_sender
            .send(NetlinkCommand::Close)
            .await
            .expect("Failed to send close command");
        task.await.expect("Task should complete successfully");
    }

    #[tokio::test]
    #[serial_test::serial]
    async fn test_data_netlink_processes_all_ready_datagrams() {
        reset_mock_state(false, 3);

        let (command_sender, command_receiver) = channel(1);
        let (buffer_sender, mut buffer_receiver) = channel(3);
        let mut actor = DataNetlinkActor::new("family", "group", command_receiver, 0);
        actor.add_recipient(buffer_sender);
        let task = spawn(DataNetlinkActor::run(actor));

        for message_index in 1..=3 {
            assert_eq!(
                recv_payload(&mut buffer_receiver).await,
                format!("family/group/message-{message_index}")
            );
        }
        tokio::time::timeout(Duration::from_secs(1), async {
            while RECV_ATTEMPTS.load(Ordering::SeqCst) < 4 {
                tokio::task::yield_now().await;
            }
        })
        .await
        .expect("socket was not drained to WouldBlock");
        assert!(RECV_ATTEMPTS.load(Ordering::SeqCst) >= 4);

        command_sender.send(NetlinkCommand::Close).await.unwrap();
        task.await.unwrap();
    }

    #[tokio::test]
    #[serial_test::serial]
    async fn test_data_netlink_wakes_after_idle_without_polling() {
        reset_mock_state(true, 1);

        let (command_sender, command_receiver) = channel(1);
        let (buffer_sender, mut buffer_receiver) = channel(1);
        let mut actor = DataNetlinkActor::new("family", "group", command_receiver, 0);
        actor.add_recipient(buffer_sender);
        let task = spawn(DataNetlinkActor::run(actor));

        wait_for_socket_registrations(1).await;
        tokio::time::sleep(Duration::from_millis(25)).await;
        assert!(RECV_ATTEMPTS.load(Ordering::SeqCst) <= 1);

        send_to_current_socket(b"after-idle");
        assert_eq!(recv_payload(&mut buffer_receiver).await, "after-idle");

        command_sender.send(NetlinkCommand::Close).await.unwrap();
        task.await.unwrap();
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
        assert!(result.is_err());
        assert_eq!(result.unwrap_err().kind(), io::ErrorKind::InvalidData);
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

    /// Tests handling multiple aligned messages where the first message length
    /// is not a multiple of 4 and therefore requires netlink padding.
    #[test]
    fn test_multiple_aligned_messages_in_buffer() {
        let mut combined_buffer = Vec::new();

        append_aligned_mock_netlink_message(&mut combined_buffer, b"A");
        append_aligned_mock_netlink_message(&mut combined_buffer, b"SECOND");

        let mut parser = NetlinkMessageParser::new();
        let result = parser.parse_buffer(&combined_buffer);
        assert!(result.is_ok());

        let messages = result.unwrap();
        assert_eq!(messages.len(), 2);
        assert_eq!(String::from_utf8(messages[0].to_vec()).unwrap(), "A");
        assert_eq!(String::from_utf8(messages[1].to_vec()).unwrap(), "SECOND");
    }

    #[test]
    fn test_trailing_zero_bytes_larger_than_alignment_padding_are_discarded() {
        let mut buffer = create_mock_netlink_message(b"COMPLETE").to_vec();
        buffer.truncate(20 + b"COMPLETE".len());
        buffer.extend_from_slice(&[0; 4]);

        let mut parser = NetlinkMessageParser::new();
        let messages = parser.parse_buffer(&buffer).unwrap();

        assert_eq!(messages.len(), 1);
        assert_eq!(messages[0].as_ref(), b"COMPLETE");
    }

    #[test]
    fn test_zero_bytes_larger_than_alignment_padding_are_rejected() {
        let mut parser = NetlinkMessageParser::new();
        let result = parser.parse_buffer(&[0; 4]);

        assert!(result.is_err());
        assert_eq!(result.unwrap_err().kind(), io::ErrorKind::InvalidData);
    }

    /// Tests that a truncated datagram is never spliced with the next datagram.
    ///
    /// Without the fix, parse_buffer() keeps the first truncated datagram in an
    /// incomplete buffer and combines it with the next recv, producing one
    /// corrupted payload instead of dropping the truncated datagram.
    #[test]
    fn test_truncated_datagram_is_not_spliced_with_next_datagram() {
        let first_payload = vec![b'A'; BUFFER_SIZE + 3200];
        let first_datagram = create_large_mock_netlink_message(&first_payload);
        let second_datagram = create_large_mock_netlink_message(b"SECOND_DATAGRAM");
        let mut parser = NetlinkMessageParser::new();

        let result1 = parser.parse_buffer(&first_datagram[..BUFFER_SIZE]);
        assert!(result1.is_err());
        assert_eq!(result1.unwrap_err().kind(), io::ErrorKind::InvalidData);

        let result2 = parser.parse_buffer(&second_datagram);
        assert!(result2.is_ok());
        let messages2 = result2.unwrap();
        assert_eq!(messages2.len(), 1);
        assert_eq!(messages2[0].as_ref(), b"SECOND_DATAGRAM");
    }

    /// Tests handling a datagram with a complete message followed by a truncated message.
    #[test]
    fn test_complete_message_followed_by_truncated_message_is_rejected() {
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
        let result = parser.parse_buffer(&combined_buffer);
        assert!(result.is_ok());
        let messages = result.unwrap();
        assert_eq!(messages.len(), 1);
        assert_eq!(messages[0].as_ref(), b"COMPLETE");

        // The next datagram must be parsed independently, not as a continuation.
        let result = parser.parse_buffer(&msg2[..msg2_len]);
        assert!(result.is_ok());
        let messages = result.unwrap();
        assert_eq!(messages.len(), 1);
        assert_eq!(messages[0].as_ref(), b"PARTIAL_MSG");
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn test_recv_datagram_larger_than_default_buffer() {
        use std::os::unix::net::UnixDatagram;

        let (tx, rx) = UnixDatagram::pair().unwrap();
        let payload = vec![0xab; BUFFER_SIZE + 4096];
        tx.send(&payload).unwrap();

        let received =
            DataNetlinkActor::recv_datagram_fd(rx.as_raw_fd(), MAX_NETLINK_DATAGRAM_SIZE).unwrap();

        assert_eq!(received, payload);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn test_recv_datagram_larger_than_max_size_is_drained() {
        use std::os::unix::net::UnixDatagram;

        let (tx, rx) = UnixDatagram::pair().unwrap();
        let payload = vec![0xef; BUFFER_SIZE + 4096];
        let next_payload = b"NEXT";
        tx.send(&payload).unwrap();
        tx.send(next_payload).unwrap();

        let result = DataNetlinkActor::recv_datagram_fd(rx.as_raw_fd(), BUFFER_SIZE);
        assert!(result.is_err());
        assert_eq!(result.unwrap_err().kind(), io::ErrorKind::InvalidData);

        let received = DataNetlinkActor::recv_datagram_fd(rx.as_raw_fd(), BUFFER_SIZE).unwrap();
        assert_eq!(received, next_payload);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn test_recv_datagram_empty_is_invalid_and_drained() {
        use std::os::unix::net::UnixDatagram;

        let (tx, rx) = UnixDatagram::pair().unwrap();
        let next_payload = b"NEXT";
        tx.send(&[]).unwrap();
        tx.send(next_payload).unwrap();

        let result = DataNetlinkActor::recv_datagram_fd(rx.as_raw_fd(), BUFFER_SIZE);
        assert!(result.is_err());
        assert_eq!(result.unwrap_err().kind(), io::ErrorKind::InvalidData);

        let received = DataNetlinkActor::recv_datagram_fd(rx.as_raw_fd(), BUFFER_SIZE).unwrap();
        assert_eq!(received, next_payload);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn test_plain_recvmsg_reports_truncation() {
        use std::os::unix::net::UnixDatagram;

        let (tx, rx) = UnixDatagram::pair().unwrap();
        let payload = vec![0xcd; BUFFER_SIZE + 4096];
        tx.send(&payload).unwrap();

        let mut buffer = vec![0u8; BUFFER_SIZE];
        let (size, flags) = DataNetlinkActor::recvmsg_into(rx.as_raw_fd(), &mut buffer, 0).unwrap();

        assert_eq!(size, BUFFER_SIZE);
        assert_ne!(flags & libc::MSG_TRUNC, 0);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn test_recv_datagram_fd_propagates_wouldblock() {
        use std::os::unix::net::UnixDatagram;

        let (_tx, rx) = UnixDatagram::pair().unwrap();
        rx.set_nonblocking(true).unwrap();

        let result = DataNetlinkActor::recv_datagram_fd(rx.as_raw_fd(), BUFFER_SIZE);

        assert!(result.is_err());
        assert_eq!(result.unwrap_err().kind(), io::ErrorKind::WouldBlock);
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

    #[test]
    #[serial_test::serial]
    fn test_netlink_rcvbuf_stored_on_construction() {
        reset_mock_state(false, 1);
        let (_, command_receiver) = channel(1);
        let actor = DataNetlinkActor::new("family", "group", command_receiver, 4194304);
        assert_eq!(actor.netlink_rcvbuf_bytes, 4194304);
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
