use std::{sync::Arc, time::Duration};

use std::os::unix::io::AsRawFd;
#[cfg(test)]
use std::os::unix::io::RawFd;

use log::{debug, info, warn};

#[cfg(not(test))]
use netlink_sys::{protocols::NETLINK_GENERIC, Socket, SocketAddr};
use tokio::{
    io::{unix::AsyncFd, Interest, Ready as TokioReady},
    select,
    sync::mpsc::{Receiver, Sender},
    time::{interval, sleep_until, Instant, MissedTickBehavior},
};

use std::io;

use super::super::message::{
    buffer::SocketBufferMessage,
    netlink::{NetlinkCommand, NetlinkSubscription},
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
const DEFAULT_FAMILY: &str = "sonic_stel";
const DEFAULT_GROUP: &str = "ipfix";

/// Size of the buffer used for receiving netlink messages
#[cfg(test)]
const BUFFER_SIZE: usize = 0x1FFFF;
/// Linux error code for "No buffer space available" (ENOBUFS)
/// Note: std::io::ErrorKind doesn't have a specific variant for ENOBUFS,
/// so we use the raw OS error code for this specific netlink error condition.
const ENOBUFS: i32 = 105;

/// Maximum number of consecutive failures before waiting for ControlNetlinkActor
const MAX_LOCAL_RECONNECT_ATTEMPTS: u32 = 3;

/// Heartbeat logging interval.
const HEARTBEAT_INTERVAL_SECS: u64 = 5 * 60;

/// Retry interval after a socket cannot be registered with the Tokio reactor.
const SOCKET_REGISTRATION_RETRY_SECS: u64 = 1;
const MAX_SOCKET_REGISTRATION_RETRY_SECS: u64 = 60;
const WARNING_SUMMARY_INTERVAL_SECS: u64 = 60;

/// Maximum supported size for a single netlink datagram/message.
/// This bounds userspace allocation after peeking the datagram length.
const MAX_NETLINK_DATAGRAM_SIZE: usize = 16 * 1024 * 1024;

/// Netlink message parser for handling multiple messages in one datagram
#[derive(Debug)]
struct NetlinkMessageParser;

#[derive(Debug)]
struct ParseOutcome {
    messages: Vec<SocketBufferMessage>,
    dropped_messages: usize,
    first_error: Option<String>,
}

struct WarningLimiter {
    last_log: Option<tokio::time::Instant>,
    suppressed: usize,
}

impl WarningLimiter {
    fn new() -> Self {
        Self {
            last_log: None,
            suppressed: 0,
        }
    }

    fn record(&mut self, count: usize) -> Option<usize> {
        let now = tokio::time::Instant::now();
        if self.last_log.is_none_or(|last| {
            now.duration_since(last) >= Duration::from_secs(WARNING_SUMMARY_INTERVAL_SECS)
        }) {
            let suppressed = self.suppressed;
            self.last_log = Some(now);
            self.suppressed = 0;
            Some(suppressed)
        } else {
            self.suppressed += count;
            None
        }
    }
}

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
    ) -> Result<ParseOutcome, io::Error> {
        if complete_messages.is_empty() {
            return Err(error);
        }

        Ok(ParseOutcome {
            messages: complete_messages,
            dropped_messages: 1,
            first_error: Some(format!(
                "discarding trailing {remaining} bytes at offset {offset}: {error}"
            )),
        })
    }

    /// Parse a single netlink datagram that may contain one or more complete netlink messages.
    ///
    /// Netlink multicast sockets are datagram-oriented. If a userspace receive buffer is too small,
    /// the kernel discards the rest of that datagram, so bytes from a later recv must never be
    /// treated as a continuation of the previous one.
    /// Returns the generic-netlink payload from each complete netlink message. For IPFIX data,
    /// each payload can contain multiple IPFIX sets and records.
    fn parse_buffer(
        &mut self,
        new_data: &[u8],
        expected_family_id: u16,
    ) -> Result<ParseOutcome, io::Error> {
        let mut complete_messages = Vec::new();
        let mut dropped_messages = 0;
        let mut first_error = None;
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
            let nl_len = u32::from_ne_bytes([
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
            let message_data = &new_data[offset..offset + nl_len];
            debug!(
                "Found complete message: offset={}, length={}, aligned_length={}",
                offset, nl_len, aligned_nl_len
            );

            // Extract payload from this message
            match Self::extract_payload_from_slice(message_data, expected_family_id) {
                Ok(payload) => {
                    debug!(
                        "Successfully extracted payload with {} bytes",
                        payload.len()
                    );
                    complete_messages.push(payload);
                }
                Err(e) => {
                    dropped_messages += 1;
                    first_error.get_or_insert_with(|| {
                        format!("failed to extract message at offset {offset}: {e}")
                    });
                }
            }

            let remaining = new_data.len() - offset;
            offset += usize::min(aligned_nl_len, remaining);
        }

        Ok(ParseOutcome {
            messages: complete_messages,
            dropped_messages,
            first_error,
        })
    }

    #[cfg(test)]
    fn parse_test_buffer(
        &mut self,
        new_data: &[u8],
    ) -> Result<Vec<SocketBufferMessage>, io::Error> {
        self.parse_buffer(new_data, 0x10)
            .map(|outcome| outcome.messages)
    }

    /// Extract payload from a single complete netlink message
    fn extract_payload_from_slice(
        message_data: &[u8],
        expected_family_id: u16,
    ) -> Result<SocketBufferMessage, io::Error> {
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
        let nl_len = u32::from_ne_bytes(message_data[NLMSG_LEN].try_into().unwrap()) as usize;

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

        let nl_type = u16::from_ne_bytes(message_data[NLMSG_TYPE].try_into().unwrap());
        if nl_type != expected_family_id {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "Netlink message type {nl_type} does not match family {expected_family_id}"
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
            let nl_flags = u16::from_ne_bytes(message_data[NLMSG_FLAGS].try_into().unwrap());
            let nl_seq = u32::from_ne_bytes(message_data[NLMSG_SEQ].try_into().unwrap());
            let nl_pid = u32::from_ne_bytes(message_data[NLMSG_PID].try_into().unwrap());
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
                    u16::from_ne_bytes(message_data[GENL_RESERVED].try_into().unwrap());
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
    /// Family and multicast group associated with the active socket.
    subscription: Option<NetlinkSubscription>,
    /// List of channels to send received buffer messages to
    buffer_recipients: Vec<Sender<SocketBufferMessage>>,
    /// Channel for receiving control commands
    command_recipient: Receiver<NetlinkCommand>,
    /// Message parser for handling one or more netlink messages in each datagram
    message_parser: NetlinkMessageParser,
    /// Reused storage for complete netlink datagrams.
    receive_buffer: Vec<u8>,
    /// Netlink socket receive buffer size in bytes (0 = OS default). Reduces ENOBUFS when set.
    netlink_rcvbuf_bytes: usize,
}

impl DataNetlinkActor {
    fn membership_is_set(groups: &[u32], group_id: u32) -> bool {
        let Some(bit) = group_id.checked_sub(1) else {
            return false;
        };
        let word = bit as usize / u32::BITS as usize;
        word < groups.len() && groups[word] & (1 << (bit % u32::BITS)) != 0
    }

    fn register_active_socket(&mut self) -> Result<Option<AsyncFd<SocketType>>, io::Error> {
        let Some(socket) = self.socket.take() else {
            return Ok(None);
        };
        let fd = socket.as_raw_fd();
        #[cfg(test)]
        if test::fail_socket_registration() {
            self.socket = Some(socket);
            return Err(io::Error::other("simulated socket registration failure"));
        }
        match AsyncFd::try_with_interest(socket, Interest::READABLE | Interest::ERROR) {
            Ok(socket) => {
                #[cfg(test)]
                test::record_socket_registration();
                debug!("Registered data netlink socket fd {} with Tokio", fd);
                Ok(Some(socket))
            }
            Err(e) => {
                let (socket, cause) = e.into_parts();
                self.socket = Some(socket);
                Err(io::Error::new(
                    cause.kind(),
                    format!("failed to register data netlink socket fd {fd}: {cause}"),
                ))
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
    ) -> Result<(usize, i32, u32), io::Error> {
        let mut iov = libc::iovec {
            iov_base: buffer.as_mut_ptr() as *mut libc::c_void,
            iov_len: buffer.len(),
        };
        let mut msg: libc::msghdr = unsafe { std::mem::zeroed() };
        msg.msg_iov = &mut iov;
        msg.msg_iovlen = 1;
        let mut source: libc::sockaddr_nl = unsafe { std::mem::zeroed() };
        msg.msg_name = (&mut source as *mut libc::sockaddr_nl).cast();
        msg.msg_namelen = std::mem::size_of::<libc::sockaddr_nl>() as libc::socklen_t;

        // Safe on the Tokio worker because the fd is configured as non-blocking by connect().
        let size = unsafe { libc::recvmsg(fd, &mut msg, flags) };
        if size < 0 {
            Err(io::Error::last_os_error())
        } else {
            Ok((size as usize, msg.msg_flags, source.nl_pid))
        }
    }

    fn recv_datagram_fd_into(
        fd: std::os::unix::io::RawFd,
        max_size: usize,
        buffer: &mut Vec<u8>,
    ) -> Result<(), io::Error> {
        let mut peek_buffer = [0u8; 1];
        // Peek first to size the receive buffer exactly and to make truncation observable. A fixed
        // large buffer would either waste memory in the hot path or still silently truncate when a
        // producer emits a larger datagram than expected.
        let (needed, peek_flags, peek_source) =
            Self::recvmsg_into(fd, &mut peek_buffer, libc::MSG_PEEK | libc::MSG_TRUNC)?;
        if peek_source != 0 {
            let mut drain_buffer = [0u8; 1];
            let _ = Self::recvmsg_into(fd, &mut drain_buffer, libc::MSG_TRUNC);
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                format!("Data netlink datagram came from userspace port {peek_source}"),
            ));
        }

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

        buffer.resize(needed, 0);
        let (size, flags, source) = Self::recvmsg_into(fd, buffer, 0)?;
        if source != 0 {
            return Err(io::Error::new(
                io::ErrorKind::PermissionDenied,
                format!("Data netlink datagram came from userspace port {source}"),
            ));
        }
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
        Ok(())
    }

    #[cfg(test)]
    fn recv_datagram_fd(
        fd: std::os::unix::io::RawFd,
        max_size: usize,
    ) -> Result<Vec<u8>, io::Error> {
        let mut buffer = Vec::new();
        Self::recv_datagram_fd_into(fd, max_size, &mut buffer)?;
        Ok(buffer)
    }

    fn recv_netlink_datagram(
        socket: &mut SocketType,
        buffer: &mut Vec<u8>,
    ) -> Result<(), io::Error> {
        #[cfg(test)]
        socket.prepare_recv()?;

        Self::recv_datagram_fd_into(socket.as_raw_fd(), MAX_NETLINK_DATAGRAM_SIZE, buffer)
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
        DataNetlinkActor {
            family: family.to_string(),
            group: group.to_string(),
            socket: None,
            subscription: None,
            buffer_recipients: Vec::new(),
            command_recipient,
            message_parser: NetlinkMessageParser::new(),
            receive_buffer: Vec::new(),
            netlink_rcvbuf_bytes,
        }
    }

    /// Adds a new recipient channel for receiving buffer messages.
    ///
    /// # Arguments
    ///
    /// * `recipient` - Channel sender for distributing received messages
    pub fn add_recipient(&mut self, recipient: Sender<SocketBufferMessage>) {
        self.buffer_recipients.push(recipient);
    }

    #[cfg(not(test))]
    fn open_socket(&self, subscription: NetlinkSubscription) -> Option<SocketType> {
        debug!(
            "Opening data socket for family '{}' ({}), group '{}' ({})",
            self.family, subscription.family_id, self.group, subscription.group_id
        );
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

        if let Err(e) = socket.add_membership(subscription.group_id) {
            warn!(
                "Failed to add mcast membership for group_id {}: {:?}",
                subscription.group_id, e
            );
            return None;
        }

        // Set non-blocking mode
        if let Err(e) = socket.set_non_blocking(true) {
            warn!("Failed to set non-blocking mode: {:?}", e);
            return None;
        }

        info!(
            "Connected data socket to family '{}' ({}), group '{}' ({})",
            self.family, subscription.family_id, self.group, subscription.group_id
        );
        Some(socket)
    }

    #[cfg(test)]
    fn open_socket(&self, subscription: NetlinkSubscription) -> Option<SocketType> {
        test::record_connection_attempt();
        if test::fail_socket_open() {
            return None;
        }
        let Some((family_id, group_id)) = test::resolve_mock_family(&self.family, &self.group) else {
            debug!(
                "Test: family '{}', group '{}' is unavailable, connection failed",
                self.family, self.group
            );
            return None;
        };
        if (family_id, group_id) != (subscription.family_id, subscription.group_id) {
            return None;
        }

        let sock = SocketType::new(&self.family, &self.group, family_id, group_id);
        if sock.valid {
            debug!("Test: Created new valid MockSocket");
            Some(sock)
        } else {
            debug!("Test: MockSocket reports invalid, connection failed");
            None
        }
    }

    #[cfg(not(test))]
    fn has_membership(socket: &SocketType, group_id: u32) -> bool {
        let mut groups = [0u32; 32];
        let mut length = std::mem::size_of_val(&groups) as libc::socklen_t;
        let result = unsafe {
            libc::getsockopt(
                socket.as_raw_fd(),
                libc::SOL_NETLINK,
                libc::NETLINK_LIST_MEMBERSHIPS,
                groups.as_mut_ptr().cast(),
                &mut length,
            )
        };
        if result < 0 {
            warn!("Failed to inspect data netlink memberships: {}", io::Error::last_os_error());
            return false;
        }
        let count = length as usize / std::mem::size_of::<u32>();
        Self::membership_is_set(&groups[..count.min(groups.len())], group_id)
    }

    #[cfg(test)]
    fn has_membership(socket: &SocketType, group_id: u32) -> bool {
        test::socket_has_membership(socket.socket_id, group_id)
    }

    fn connect(&mut self, subscription: NetlinkSubscription) {
        if self.socket.as_ref().is_some_and(|socket| {
            self.subscription == Some(subscription)
                && Self::has_membership(socket, subscription.group_id)
        }) {
            debug!("Data socket already has the requested subscription");
            return;
        }

        self.socket = None;
        self.subscription = Some(subscription);
        self.socket = self.open_socket(subscription);
        if self.socket.is_none() {
            warn!(
                "Failed to open data socket for family '{}' ({}), group '{}' ({})",
                self.family, subscription.family_id, self.group, subscription.group_id
            );
        }
    }

    fn disconnect(&mut self) {
        self.socket = None;
        self.subscription = None;
    }

    fn reconnect(&mut self, subscription: NetlinkSubscription) {
        self.socket = None;
        self.subscription = Some(subscription);
        self.socket = self.open_socket(subscription);
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
        expected_family_id: u16,
        receive_buffer: &mut Vec<u8>,
    ) -> Result<ParseOutcome, io::Error> {
        // Try to receive with non-blocking mode (socket should already be set to non-blocking)
        debug!("Attempting to receive netlink message...");
        let recv_result = Self::recv_netlink_datagram(socket, receive_buffer);

        match recv_result {
            Ok(()) => {
                let size = receive_buffer.len();
                debug!("Received netlink data, size: {} bytes", size);

                if size == 0 {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "Received empty netlink datagram",
                    ));
                }

                if log::log_enabled!(log::Level::Debug) {
                    let hex_dump = format_hex_lines(receive_buffer);
                    debug!("Raw netlink recv buffer ({} bytes):\n{}", size, hex_dump);
                }

                // Parse buffer which may contain multiple messages and/or incomplete messages
                let outcome = message_parser.parse_buffer(receive_buffer, expected_family_id)?;
                debug!(
                    "Parsed {} complete messages and dropped {} from {} bytes of data",
                    outcome.messages.len(),
                    outcome.dropped_messages,
                    size
                );

                Ok(outcome)
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
            NetlinkCommand::Connect(subscription) => self.connect(subscription),
            NetlinkCommand::Reconnect(subscription) => self.reconnect(subscription),
            NetlinkCommand::Disconnect => self.disconnect(),
            NetlinkCommand::Close => return false,
        }

        true
    }

    async fn send_to_recipients(&mut self, message: &SocketBufferMessage) -> bool {
        let mut index = 0;
        while index < self.buffer_recipients.len() {
            match self.buffer_recipients[index].send(message.clone()).await {
                Ok(()) => index += 1,
                Err(error) => {
                    warn!("Removing closed data recipient {}: {:?}", index + 1, error);
                    self.buffer_recipients.remove(index);
                }
            }
        }
        !self.buffer_recipients.is_empty()
    }

    fn register_with_backoff(
        &mut self,
        retry_secs: &mut u64,
        retry_at: &mut Instant,
        last_logged_delay: &mut Option<u64>,
    ) -> Option<AsyncFd<SocketType>> {
        match self.register_active_socket() {
            Ok(socket) => {
                *retry_secs = SOCKET_REGISTRATION_RETRY_SECS;
                *last_logged_delay = None;
                *retry_at = Instant::now() + Duration::from_secs(*retry_secs);
                socket
            }
            Err(error) => {
                let next_delay = if last_logged_delay.is_some() {
                    (*retry_secs * 2).min(MAX_SOCKET_REGISTRATION_RETRY_SECS)
                } else {
                    *retry_secs
                };
                if *last_logged_delay != Some(next_delay) {
                    warn!(
                        "{}; retrying registration in {} second(s)",
                        error, next_delay
                    );
                    *last_logged_delay = Some(next_delay);
                }
                *retry_secs = next_delay;
                *retry_at = Instant::now() + Duration::from_secs(next_delay);
                None
            }
        }
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
            SocketRead(Result<Option<ParseOutcome>, io::Error>),
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
        let mut registration_retry_secs = SOCKET_REGISTRATION_RETRY_SECS;
        let mut registration_retry_at =
            Instant::now() + Duration::from_secs(registration_retry_secs);
        let mut last_registration_warning_delay = None;
        let mut enobufs_warnings = WarningLimiter::new();
        let mut invalid_data_warnings = WarningLimiter::new();
        let mut socket = actor.register_with_backoff(
            &mut registration_retry_secs,
            &mut registration_retry_at,
            &mut last_registration_warning_delay,
        );

        loop {
            if socket.is_none() {
                select! {
                    command = actor.command_recipient.recv() => {
                        let Some(command) = command else {
                            break;
                        };
                        if matches!(command, NetlinkCommand::Connect(subscription)
                            if actor.subscription == Some(subscription))
                        {
                            continue;
                        }
                        record_comm_stats(
                            ChannelLabel::ControlNetlinkToDataNetlink,
                            actor.command_recipient.len(),
                        );
                        if !actor.handle_command(command) {
                            break;
                        }
                        socket = actor.register_with_backoff(
                            &mut registration_retry_secs,
                            &mut registration_retry_at,
                            &mut last_registration_warning_delay,
                        );
                        consecutive_failures = 0;
                    }
                    _ = heartbeat_interval.tick() => {
                        info!("DataNetlinkActor is running without a data socket - waiting for reconnect");
                    }
                    _ = sleep_until(registration_retry_at), if actor.subscription.is_some() => {
                        if actor.socket.is_none() {
                            actor.socket = actor.open_socket(actor.subscription.unwrap());
                            if actor.socket.is_none() {
                                registration_retry_secs =
                                    (registration_retry_secs * 2).min(MAX_SOCKET_REGISTRATION_RETRY_SECS);
                                registration_retry_at = Instant::now()
                                    + Duration::from_secs(registration_retry_secs);
                                continue;
                            }
                        }
                        socket = actor.register_with_backoff(
                            &mut registration_retry_secs,
                            &mut registration_retry_at,
                            &mut last_registration_warning_delay,
                        );
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
                            let family_id = actor
                                .subscription
                                .expect("registered socket has a subscription")
                                .family_id;
                            match Self::try_recv(
                                guard.get_inner_mut(),
                                &mut actor.message_parser,
                                family_id,
                                &mut actor.receive_buffer,
                            ) {
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
                    let Some(command) = command else {
                        break;
                    };
                    if matches!(command, NetlinkCommand::Connect(subscription)
                        if actor.subscription == Some(subscription)
                            && Self::has_membership(
                                socket.as_ref().unwrap().get_ref(),
                                subscription.group_id,
                            ))
                    {
                        continue;
                    }
                    actor.socket = Self::unregister_socket(&mut socket);
                    record_comm_stats(
                        ChannelLabel::ControlNetlinkToDataNetlink,
                        actor.command_recipient.len(),
                    );
                    if !actor.handle_command(command) {
                        break;
                    }
                    socket = actor.register_with_backoff(
                        &mut registration_retry_secs,
                        &mut registration_retry_at,
                        &mut last_registration_warning_delay,
                    );
                    consecutive_failures = 0;
                }
                ActorEvent::SocketRead(result) => {
                    match result {
                        Ok(Some(outcome)) => {
                            consecutive_failures = 0;
                            if outcome.dropped_messages > 0 {
                                if let Some(suppressed) =
                                    invalid_data_warnings.record(outcome.dropped_messages)
                                {
                                    warn!(
                                        "Dropped {} invalid netlink message(s): {}{}",
                                        outcome.dropped_messages,
                                        outcome.first_error.as_deref().unwrap_or("invalid data"),
                                        if suppressed > 0 {
                                            format!("; {suppressed} additional message(s) suppressed")
                                        } else {
                                            String::new()
                                        }
                                    );
                                }
                            }

                            if outcome.messages.is_empty() {
                                debug!("Received netlink datagram but no complete payload was extracted");
                            } else {
                                debug!(
                                    "Successfully parsed {} complete netlink messages",
                                    outcome.messages.len()
                                );

                                for (i, message) in outcome.messages.iter().enumerate() {
                                    if log::log_enabled!(log::Level::Debug) {
                                        let hex_dump = format_hex_lines(message.as_ref());
                                        debug!(
                                            "Outgoing netlink payload {}/{} ({} bytes):\n{}",
                                            i + 1,
                                            outcome.messages.len(),
                                            message.len(),
                                            hex_dump
                                        );
                                    }
                                    debug!(
                                        "Processing netlink message {}/{}: {} bytes",
                                        i + 1,
                                        outcome.messages.len(),
                                        message.len()
                                    );

                                    if !actor.send_to_recipients(message).await {
                                        warn!("DataNetlinkActor has no live recipients; terminating");
                                        return;
                                    }
                                }

                                debug!("Completed processing {} netlink messages, each sent individually", outcome.messages.len());
                            }
                        }
                        Ok(None) => {}
                        Err(e) if e.raw_os_error() == Some(ENOBUFS) => {
                            if let Some(suppressed) = enobufs_warnings.record(1) {
                                warn!(
                                    "Netlink receive buffer full (ENOBUFS); {} prior notification(s) suppressed. Consider increasing --netlink-rcvbuf or reducing HFT load: {:?}",
                                    suppressed, e
                                );
                            }
                        }
                        Err(e) if e.kind() == io::ErrorKind::InvalidData => {
                            if let Some(suppressed) = invalid_data_warnings.record(1) {
                                warn!(
                                    "Dropping invalid netlink datagram; {} prior event(s) suppressed: {:?}",
                                    suppressed, e
                                );
                            }
                        }
                        Err(e) if e.kind() == io::ErrorKind::PermissionDenied => {
                            if let Some(suppressed) = invalid_data_warnings.record(1) {
                                warn!(
                                    "Dropping non-kernel netlink datagram; {} prior event(s) suppressed: {:?}",
                                    suppressed, e
                                );
                            }
                        }
                        Err(e) => {
                            warn!("Failed to receive message: {:?}", e);
                            if socket.is_some() {
                                actor.socket = Self::unregister_socket(&mut socket);
                            }
                            actor.socket = None;
                            consecutive_failures += 1;
                            if consecutive_failures <= MAX_LOCAL_RECONNECT_ATTEMPTS {
                                debug!("Attempting quick reconnect #{}", consecutive_failures);
                                if let Some(subscription) = actor.subscription {
                                    actor.reconnect(subscription);
                                    socket = actor.register_with_backoff(
                                        &mut registration_retry_secs,
                                        &mut registration_retry_at,
                                        &mut last_registration_warning_delay,
                                    );
                                }
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
        msg[4..6].copy_from_slice(&family_id.to_ne_bytes());
        let actual_len = 20 + payload.len(); // 16 (nlmsg) + 4 (genl) + payload
        msg[..actual_len].to_vec()
    }

    fn create_large_mock_netlink_message(payload: &[u8]) -> Vec<u8> {
        let total_len = 20 + payload.len();
        let mut msg = vec![0u8; total_len];
        msg[0..4].copy_from_slice(&(total_len as u32).to_ne_bytes());
        msg[4..6].copy_from_slice(&0x10u16.to_ne_bytes());
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
    static NEXT_RECV_ERRNO: AtomicUsize = AtomicUsize::new(0);
    static SOCKET_REGISTRATION_FAILURES: AtomicUsize = AtomicUsize::new(0);
    static SOCKET_OPEN_FAILURES: AtomicUsize = AtomicUsize::new(0);
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

    pub(crate) fn resolve_mock_family(family: &str, group: &str) -> Option<(u16, u32)> {
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
        NEXT_RECV_ERRNO.store(0, Ordering::SeqCst);
        SOCKET_REGISTRATION_FAILURES.store(0, Ordering::SeqCst);
        SOCKET_OPEN_FAILURES.store(0, Ordering::SeqCst);
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
        LIVE_SUBSCRIPTIONS
            .lock()
            .unwrap()
            .retain(|subscription| subscription.family_id != family_id);
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

    fn fail_next_recv_with_errno(errno: i32) {
        NEXT_RECV_ERRNO.store(errno as usize, Ordering::SeqCst);
    }

    pub(super) fn fail_socket_registration() -> bool {
        SOCKET_REGISTRATION_FAILURES
            .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |remaining| {
                remaining.checked_sub(1)
            })
            .is_ok()
    }

    pub(super) fn fail_socket_open() -> bool {
        SOCKET_OPEN_FAILURES
            .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |remaining| {
                remaining.checked_sub(1)
            })
            .is_ok()
    }

    pub(super) fn socket_has_membership(socket_id: usize, group_id: u32) -> bool {
        LIVE_SUBSCRIPTIONS
            .lock()
            .unwrap()
            .iter()
            .any(|subscription| {
                subscription.socket_id == socket_id && subscription.group_id == group_id
            })
    }

    fn send_raw_to_current_socket(datagram: &[u8]) {
        LIVE_SUBSCRIPTIONS
            .lock()
            .unwrap()
            .last()
            .expect("mock socket sender is available")
            .sender
            .send(datagram)
            .expect("send raw mock datagram");
    }

    /// Mock socket backed by a real datagram fd so tests exercise Tokio readiness registration.
    pub struct MockSocket {
        pub valid: bool,
        socket: UnixDatagram,
        _sender: UnixDatagram,
        fail_on_recv: bool,
        pub(super) socket_id: usize,
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
            let errno = NEXT_RECV_ERRNO.swap(0, Ordering::SeqCst) as i32;
            if errno != 0 {
                return Err(io::Error::from_raw_os_error(errno));
            }
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

        let first = NetlinkSubscription {
            family_id: 0x20,
            group_id: 0x100,
        };
        command_sender
            .send(NetlinkCommand::Connect(first))
            .await
            .unwrap();

        assert_eq!(
            recv_payload(&mut buffer_receiver).await,
            "family-1/group-1/socket-1"
        );

        command_sender
            .send(NetlinkCommand::Connect(first))
            .await
            .unwrap();
        tokio::task::yield_now().await;
        assert_eq!(REGISTERED_SOCKET_COUNT.load(Ordering::SeqCst), 1);
        assert_eq!(SOCKET_COUNT.load(Ordering::SeqCst), 1);
        send_to_current_socket(b"after-soft-reconnect");
        assert_eq!(
            recv_payload(&mut buffer_receiver).await,
            "after-soft-reconnect"
        );

        command_sender
            .send(NetlinkCommand::Reconnect(first))
            .await
            .unwrap();
        assert_eq!(
            recv_payload(&mut buffer_receiver).await,
            "family-1/group-1/socket-2"
        );

        FAIL_NEXT_SOCKET.store(true, Ordering::SeqCst);
        command_sender
            .send(NetlinkCommand::Reconnect(first))
            .await
            .unwrap();
        assert_eq!(
            recv_payload(&mut buffer_receiver).await,
            "family-1/group-1/socket-4"
        );

        command_sender
            .send(NetlinkCommand::Reconnect(first))
            .await
            .unwrap();
        assert_eq!(
            recv_payload(&mut buffer_receiver).await,
            "family-1/group-1/socket-5"
        );

        let socket_count = SOCKET_COUNT.load(Ordering::SeqCst);
        assert_eq!(socket_count, 5);

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
        command_sender
            .send(NetlinkCommand::Connect(NetlinkSubscription {
                family_id: 0x20,
                group_id: 0x100,
            }))
            .await
            .unwrap();

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

    #[tokio::test(start_paused = true)]
    #[serial_test::serial]
    async fn test_data_netlink_wakes_after_idle_without_polling() {
        reset_mock_state(true, 1);

        let (command_sender, command_receiver) = channel(1);
        let (buffer_sender, mut buffer_receiver) = channel(1);
        let mut actor = DataNetlinkActor::new("family", "group", command_receiver, 0);
        actor.add_recipient(buffer_sender);
        let task = spawn(DataNetlinkActor::run(actor));

        command_sender
            .send(NetlinkCommand::Connect(NetlinkSubscription {
                family_id: 0x20,
                group_id: 0x100,
            }))
            .await
            .unwrap();

        wait_for_socket_registrations(1).await;
        for _ in 0..60 {
            tokio::time::advance(Duration::from_secs(1)).await;
            tokio::task::yield_now().await;
        }
        assert!(RECV_ATTEMPTS.load(Ordering::SeqCst) <= 1);

        send_to_current_socket(b"after-idle");
        assert_eq!(recv_payload(&mut buffer_receiver).await, "after-idle");

        command_sender.send(NetlinkCommand::Close).await.unwrap();
        task.await.unwrap();
    }

    #[tokio::test]
    #[serial_test::serial]
    async fn test_enobufs_keeps_socket_and_drains_queued_data() {
        reset_mock_state(true, 1);
        let (command_sender, command_receiver) = channel(2);
        let (buffer_sender, mut buffer_receiver) = channel(1);
        let mut actor = DataNetlinkActor::new("family", "group", command_receiver, 0);
        actor.add_recipient(buffer_sender);
        let task = spawn(DataNetlinkActor::run(actor));
        let subscription = NetlinkSubscription {
            family_id: 0x20,
            group_id: 0x100,
        };
        command_sender
            .send(NetlinkCommand::Connect(subscription))
            .await
            .unwrap();
        wait_for_socket_registrations(1).await;

        fail_next_recv_with_errno(ENOBUFS);
        send_to_current_socket(b"after-enobufs");
        assert_eq!(recv_payload(&mut buffer_receiver).await, "after-enobufs");
        assert_eq!(CONNECTION_ATTEMPTS.load(Ordering::SeqCst), 1);
        assert_eq!(live_socket_count(), 1);

        command_sender.send(NetlinkCommand::Close).await.unwrap();
        task.await.unwrap();
    }

    #[tokio::test(start_paused = true)]
    #[serial_test::serial]
    async fn test_invalid_datagram_keeps_socket_and_quiesces() {
        reset_mock_state(true, 1);
        let (command_sender, command_receiver) = channel(2);
        let (buffer_sender, mut buffer_receiver) = channel(1);
        let mut actor = DataNetlinkActor::new("family", "group", command_receiver, 0);
        actor.add_recipient(buffer_sender);
        let task = spawn(DataNetlinkActor::run(actor));
        command_sender
            .send(NetlinkCommand::Connect(NetlinkSubscription {
                family_id: 0x20,
                group_id: 0x100,
            }))
            .await
            .unwrap();
        wait_for_socket_registrations(1).await;

        send_raw_to_current_socket(&[1, 2, 3, 4]);
        for _ in 0..100 {
            if RECV_ATTEMPTS.load(Ordering::SeqCst) >= 2 {
                break;
            }
            tokio::task::yield_now().await;
        }
        let attempts = RECV_ATTEMPTS.load(Ordering::SeqCst);
        tokio::time::advance(Duration::from_secs(60)).await;
        tokio::task::yield_now().await;
        assert_eq!(RECV_ATTEMPTS.load(Ordering::SeqCst), attempts);
        assert_eq!(CONNECTION_ATTEMPTS.load(Ordering::SeqCst), 1);

        send_to_current_socket(b"after-invalid");
        assert_eq!(recv_payload(&mut buffer_receiver).await, "after-invalid");
        command_sender.send(NetlinkCommand::Close).await.unwrap();
        task.await.unwrap();
    }

    #[test]
    fn test_wrong_family_message_is_dropped() {
        let mut parser = NetlinkMessageParser::new();
        let message = create_test_message_for_family(b"wrong-family", 0x21);
        let outcome = parser.parse_buffer(&message, 0x20).unwrap();

        assert!(outcome.messages.is_empty());
        assert_eq!(outcome.dropped_messages, 1);
    }

    #[test]
    fn test_membership_bitmap_uses_one_based_group_ids() {
        assert!(DataNetlinkActor::membership_is_set(&[1], 1));
        assert!(DataNetlinkActor::membership_is_set(&[1 << 29], 30));
        assert!(DataNetlinkActor::membership_is_set(&[0, 1], 33));
        assert!(!DataNetlinkActor::membership_is_set(&[30], 30));
        assert!(!DataNetlinkActor::membership_is_set(&[u32::MAX], 0));
    }

    #[tokio::test]
    #[serial_test::serial]
    async fn test_closed_recipient_is_removed_while_live_recipient_continues() {
        reset_mock_state(true, 1);
        let (command_sender, command_receiver) = channel(2);
        let (closed_sender, closed_receiver) = channel(1);
        let (live_sender, mut live_receiver) = channel(1);
        drop(closed_receiver);
        let mut actor = DataNetlinkActor::new("family", "group", command_receiver, 0);
        actor.add_recipient(closed_sender);
        actor.add_recipient(live_sender);
        let task = spawn(DataNetlinkActor::run(actor));
        command_sender
            .send(NetlinkCommand::Connect(NetlinkSubscription {
                family_id: 0x20,
                group_id: 0x100,
            }))
            .await
            .unwrap();
        wait_for_socket_registrations(1).await;

        send_to_current_socket(b"live-recipient");
        assert_eq!(recv_payload(&mut live_receiver).await, "live-recipient");

        command_sender.send(NetlinkCommand::Close).await.unwrap();
        task.await.unwrap();
    }

    #[tokio::test(start_paused = true)]
    #[serial_test::serial]
    async fn test_socket_registration_retries_with_backoff() {
        reset_mock_state(true, 1);
        SOCKET_REGISTRATION_FAILURES.store(3, Ordering::SeqCst);
        let (command_sender, command_receiver) = channel(2);
        let (buffer_sender, mut buffer_receiver) = channel(1);
        let mut actor = DataNetlinkActor::new("family", "group", command_receiver, 0);
        actor.add_recipient(buffer_sender);
        let task = spawn(DataNetlinkActor::run(actor));
        command_sender
            .send(NetlinkCommand::Connect(NetlinkSubscription {
                family_id: 0x20,
                group_id: 0x100,
            }))
            .await
            .unwrap();

        for _ in 0..16 {
            tokio::task::yield_now().await;
        }
        for _ in 0..8 {
            tokio::time::advance(Duration::from_secs(1)).await;
            for _ in 0..16 {
                tokio::task::yield_now().await;
            }
        }
        wait_for_socket_registrations(1).await;
        send_to_current_socket(b"after-registration-retry");
        assert_eq!(
            recv_payload(&mut buffer_receiver).await,
            "after-registration-retry"
        );

        command_sender.send(NetlinkCommand::Close).await.unwrap();
        task.await.unwrap();
    }

    #[tokio::test(start_paused = true)]
    #[serial_test::serial]
    async fn test_socket_open_failure_is_retried() {
        reset_mock_state(true, 1);
        SOCKET_OPEN_FAILURES.store(2, Ordering::SeqCst);
        let (command_sender, command_receiver) = channel(2);
        let (buffer_sender, mut buffer_receiver) = channel(1);
        let mut actor = DataNetlinkActor::new("family", "group", command_receiver, 0);
        actor.add_recipient(buffer_sender);
        let task = spawn(DataNetlinkActor::run(actor));
        command_sender
            .send(NetlinkCommand::Connect(NetlinkSubscription {
                family_id: 0x20,
                group_id: 0x100,
            }))
            .await
            .unwrap();
        for _ in 0..16 {
            tokio::task::yield_now().await;
        }
        assert_eq!(CONNECTION_ATTEMPTS.load(Ordering::SeqCst), 1);

        tokio::time::advance(Duration::from_secs(1)).await;
        for _ in 0..16 {
            tokio::task::yield_now().await;
        }
        assert_eq!(CONNECTION_ATTEMPTS.load(Ordering::SeqCst), 2);
        tokio::time::advance(Duration::from_secs(2)).await;
        wait_for_socket_registrations(1).await;
        assert_eq!(CONNECTION_ATTEMPTS.load(Ordering::SeqCst), 3);

        send_to_current_socket(b"after-open-retry");
        assert_eq!(recv_payload(&mut buffer_receiver).await, "after-open-retry");
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

        let result = parser.parse_test_buffer(&mock_msg[..actual_len]);
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

        let result = parser.parse_test_buffer(&mock_msg[..actual_len]);
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

        let result = parser.parse_test_buffer(&buffer);
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
        let result = parser.parse_test_buffer(&combined_buffer);
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
        let result = parser.parse_test_buffer(&combined_buffer);
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
        let messages = parser.parse_test_buffer(&buffer).unwrap();

        assert_eq!(messages.len(), 1);
        assert_eq!(messages[0].as_ref(), b"COMPLETE");
    }

    #[test]
    fn test_zero_bytes_larger_than_alignment_padding_are_rejected() {
        let mut parser = NetlinkMessageParser::new();
        let result = parser.parse_test_buffer(&[0; 4]);

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

        let result1 = parser.parse_test_buffer(&first_datagram[..BUFFER_SIZE]);
        assert!(result1.is_err());
        assert_eq!(result1.unwrap_err().kind(), io::ErrorKind::InvalidData);

        let result2 = parser.parse_test_buffer(&second_datagram);
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
        let result = parser.parse_test_buffer(&combined_buffer);
        assert!(result.is_ok());
        let messages = result.unwrap();
        assert_eq!(messages.len(), 1);
        assert_eq!(messages[0].as_ref(), b"COMPLETE");

        // The next datagram must be parsed independently, not as a continuation.
        let result = parser.parse_test_buffer(&msg2[..msg2_len]);
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
        let (size, flags, _) =
            DataNetlinkActor::recvmsg_into(rx.as_raw_fd(), &mut buffer, 0).unwrap();

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

    /// Tests exact values are read rather than silently replaced with defaults.
    #[test]
    fn test_get_genl_family_group() {
        assert_eq!(
            get_genl_family_group_from_path_safe("tests/data/constants.yml").unwrap(),
            ("test_family".to_string(), "test_group".to_string())
        );
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

    /// Tests invalid configuration is reported and the public wrapper falls back.
    #[test]
    fn test_get_genl_family_group_defaults() {
        let file = tempfile::NamedTempFile::new().unwrap();
        std::fs::write(file.path(), "constants:\n  high_frequency_telemetry:\n    genl_family: 7\n").unwrap();
        assert!(get_genl_family_group_from_path_safe(file.path().to_str().unwrap()).is_err());
        assert_eq!(
            get_genl_family_group_from_path(file.path().to_str().unwrap()),
            (DEFAULT_FAMILY.to_string(), DEFAULT_GROUP.to_string())
        );
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
    get_genl_family_group_from_path(SONIC_CONSTANTS)
}

fn get_genl_family_group_from_path(path: &str) -> (String, String) {
    match get_genl_family_group_from_path_safe(path) {
        Ok((family, group)) => {
            debug!(
                "Loaded netlink config from '{}': family='{}', group='{}'",
                path, family, group
            );
            (family, group)
        }
        Err(e) => {
            warn!(
                "Failed to load config from '{}': {}. Using defaults: family='{}', group='{}'",
                path, e, DEFAULT_FAMILY, DEFAULT_GROUP
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

    let hft = &yaml["constants"]["high_frequency_telemetry"];
    let family = hft["genl_family"]
        .as_str()
        .filter(|value| !value.is_empty())
        .ok_or_else(|| format!("Missing or invalid genl_family in '{path}'"))?;
    let group = hft["genl_multicast_group"]
        .as_str()
        .filter(|value| !value.is_empty())
        .ok_or_else(|| format!("Missing or invalid genl_multicast_group in '{path}'"))?;

    Ok((family.to_string(), group.to_string()))
}
