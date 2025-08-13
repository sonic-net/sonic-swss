use std::{
    collections::LinkedList,
    fs::File,
    io::{self, prelude::*},
    sync::Arc,
    thread::sleep,
    time::Duration,
};

use log::{debug, error, warn};

#[allow(unused_imports)]
use neli::{
    consts::socket::{Msg, NlFamily},
    router::synchronous::NlRouter,
    socket::NlSocket,
    utils::Groups,
};
use tokio::{
    select,
    sync::mpsc::{Receiver, Sender},
};

use yaml_rust::YamlLoader;

use super::super::message::{buffer::SocketBufferMessage, netlink::{NetlinkCommand, SocketConnect}};

#[cfg(not(test))]
type SocketType = NlSocket;
#[cfg(test)]
type SocketType = test::MockSocket;

#[cfg(not(test))]
// The unit of reconnect interval is milliseconds
const RECONNECT_INTERVAL_MS: u64 = 10_000_u64;
#[cfg(test)]
// The unit of reconnect interval is milliseconds
const RECONNECT_INTERVAL_MS: u64 = 1;
/// Maximum number of reconnection attempts before giving up
const RECONNECT_MAX_ATTEMPTS: u64 = 5;
/// Size of the buffer used for receiving netlink messages
const BUFFER_SIZE: usize = 0xFFFF;
/// Linux error code for "No buffer space available"
const ENOBUFS: i32 = 105;

/// Actor responsible for managing netlink socket connections and message distribution.
/// 
/// The NetlinkActor handles:
/// - Establishing and maintaining netlink socket connections
/// - Automatic reconnection on connection failures
/// - Distribution of received messages to multiple recipients
/// - Processing control commands for socket management
pub struct NetlinkActor {
    /// The generic netlink family name
    family: String,
    /// The multicast group name
    group: String,
    /// The active netlink socket connection (None if disconnected)
    socket: Option<SocketType>,
    /// List of channels to send received buffer messages to
    buffer_recipients: LinkedList<Sender<SocketBufferMessage>>,
    /// Channel for receiving control commands
    command_recipient: Receiver<NetlinkCommand>,
}

impl NetlinkActor {
    /// Creates a new NetlinkActor instance.
    /// 
    /// # Arguments
    /// 
    /// * `family` - The generic netlink family name
    /// * `group` - The multicast group name 
    /// * `command_recipient` - Channel for receiving control commands
    /// 
    /// # Returns
    /// 
    /// A new NetlinkActor instance with an initial connection attempt
    pub fn new(family: &str, group: &str, command_recipient: Receiver<NetlinkCommand>) -> Self {
        let socket = NetlinkActor::connect(family, group);
        NetlinkActor {
            family: family.to_string(),
            group: group.to_string(),
            socket,
            buffer_recipients: LinkedList::new(),
            command_recipient,
        }
    }

    /// Adds a new recipient channel for receiving buffer messages.
    /// 
    /// # Arguments
    /// 
    /// * `recipient` - Channel sender for distributing received messages
    pub fn add_recipient(&mut self, recipient: Sender<SocketBufferMessage>) {
        self.buffer_recipients.push_back(recipient);
    }

    /// Establishes a connection to the netlink socket for production use.
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
    fn connect(family: &str, group: &str) -> Option<SocketType> {
        let (sock, _) = match NlRouter::connect(
            NlFamily::Generic,
            // 0 is pid of kernel -> socket is connected to kernel
            Some(0),
            Groups::empty(),
        ) {
            Ok(result) => result,
            Err(e) => {
                warn!("Failed to connect to netlink router: {:?}", e);
                return None;
            }
        };

        let group_id = match sock.resolve_nl_mcast_group(family, group) {
            Ok(id) => id,
            Err(e) => {
                warn!("Failed to resolve group id for family '{}', group '{}': {:?}", family, group, e);
                return None;
            }
        };

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

        match socket.add_mcast_membership(Groups::new_groups(&[group_id])) {
            Ok(_) => Some(socket),
            Err(e) => {
                warn!("Failed to add mcast membership: {:?}", e);
                None
            }
        }
    }

    /// Mock connection method for testing purposes.
    /// 
    /// # Arguments
    /// 
    /// * `_` - Family name (ignored in test)
    /// * `_` - Group name (ignored in test)
    /// 
    /// # Returns
    /// 
    /// Some(MockSocket) if valid, None otherwise
    #[cfg(test)]
    fn connect(_: &str, _: &str) -> Option<SocketType> {
        let sock = SocketType::new();
        if sock.valid {
            Some(sock)
        } else {
            None
        }
    }

    /// Attempts to reconnect to the netlink socket with exponential backoff.
    /// 
    /// Will retry up to RECONNECT_MAX_ATTEMPTS times before giving up and closing
    /// the command channel.
    fn reconnect(&mut self) {
        for i in 0..RECONNECT_MAX_ATTEMPTS {
            sleep(Duration::from_millis(RECONNECT_INTERVAL_MS));
            match NetlinkActor::connect(&self.family, &self.group) {
                Some(socket) => {
                    self.socket = Some(socket);
                    return;
                }
                None => {
                    warn!(
                        "Failed to reconnect to netlink socket family: '{}' group: '{}' ... {}/{}",
                        self.family, self.group, i, RECONNECT_MAX_ATTEMPTS
                    );
                    continue;
                }
            }
        }
        error!(
            "Failed to reconnect to netlink socket family: '{}' group: '{}'",
            self.family, self.group
        );
        if !self.command_recipient.is_closed() {
            self.command_recipient.close();
        }
    }

    /// Resets the actor's configuration and attempts to reconnect.
    /// 
    /// # Arguments
    /// 
    /// * `family` - New family name to use
    /// * `group` - New group name to use  
    fn reset(&mut self, family: &str, group: &str) {
        self.family = family.to_string();
        self.group = group.to_string();
        self.reconnect();
    }

    /// Attempts to receive a message from the netlink socket.
    /// 
    /// Returns an error if no socket is available or if the receive operation fails.
    async fn try_recv(socket: Option<&mut SocketType>) -> Result<SocketBufferMessage, io::Error> {
        let socket = socket.ok_or_else(|| {
            sleep(Duration::from_millis(RECONNECT_INTERVAL_MS));
            io::Error::new(io::ErrorKind::NotConnected, "No socket available")
        })?;

        let mut buffer = Arc::new(vec![0; BUFFER_SIZE]);
        let buffer_slice = Arc::get_mut(&mut buffer)
            .ok_or_else(|| io::Error::new(io::ErrorKind::Other, "Failed to get mutable reference to buffer"))?;
        
        let (size, _) = socket.recv(buffer_slice, Msg::empty())?;
        
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
        NetlinkActor::extract_payload(buffer)
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
            debug!("No payload data");
            Ok(Arc::new(Vec::new()))
        } else {
            // Return payload data without headers
            let payload = raw_buffer[payload_start..payload_end].to_vec();
            // Debug: Print payload in binary format only when debug logging is enabled
            // Only show first 16 bytes to avoid overwhelming logs
            const PREVIEW_SIZE: usize = 16;
            if payload.len() <= PREVIEW_SIZE {
                debug!("Payload ({} bytes): {:02x?}", payload.len(), payload);
            } else {
                debug!("Payload ({} bytes): {:02x?}...", payload.len(), &payload[..PREVIEW_SIZE]);
            }
            Ok(Arc::new(payload))
        }
    }

    /// Main event loop for the NetlinkActor.
    /// 
    /// Continuously processes incoming netlink messages and control commands.
    /// The loop will exit when the command channel is closed or a Close command is received.
    /// 
    /// # Arguments
    /// 
    /// * `actor` - The NetlinkActor instance to run
    pub async fn run(mut actor: NetlinkActor) {
        loop {
            select! {
                ret = NetlinkActor::try_recv(actor.socket.as_mut()) => {
                    match ret {
                        Ok(buffer) => {
                            // Send buffer to all recipients
                            for recipient in &actor.buffer_recipients {
                                if let Err(e) = recipient.send(buffer.clone()).await {
                                    warn!("Failed to send buffer to recipient: {:?}", e);
                                    // Consider removing failed recipients here if needed
                                }
                            }
                        },
                        Err(e) => {
                            // Handle ENOBUFS (No buffer space available) specifically
                            if let Some(os_error) = e.raw_os_error() {
                                match os_error {
                                    ENOBUFS => {
                                        warn!("Netlink receive buffer full (ENOBUFS). Consider increasing buffer size or processing messages faster. Error: {:?}", e);
                                        // Don't disconnect on ENOBUFS, just continue
                                        continue;
                                    },
                                    _ => {
                                        warn!("Failed to receive message: {:?}", e);
                                        actor.socket = None;
                                        actor.reconnect();
                                    }
                                }
                            } else {
                                warn!("Failed to receive message: {:?}", e);
                                actor.socket = None;
                                actor.reconnect();
                            }
                        },
                    }
                },
                cmd = actor.command_recipient.recv() => {
                    match cmd {
                        None => {
                            break;
                        },
                        Some(cmd) => {
                            match cmd {
                                NetlinkCommand::SocketConnect(SocketConnect{family, group}) => {
                                    actor.reset(&family, &group);
                                }
                                NetlinkCommand::Reconnect => {
                                    actor.reconnect();
                                }
                                NetlinkCommand::Close => {
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

impl Drop for NetlinkActor {
    fn drop(&mut self) {
        if !self.command_recipient.is_closed() {
            self.command_recipient.close();
        }
    }
}

// Configuration constants for SONiC integration
#[cfg(test)]
const SONIC_CONSTANTS: &str = "tests/data/constants.yml";
#[cfg(not(test))]
const SONIC_CONSTANTS: &str = "/etc/sonic/constants.yml";

/// Reads the generic netlink family and group from the SONiC constants file.
/// 
/// If the constants file doesn't exist or cannot be read, returns default values:
/// - genl_family: "sonic_stel"  
/// - genl_multicast_group: "ipfix"
/// 
/// # Panics
/// 
/// Panics if the constants file exists but cannot be parsed as valid YAML.
pub fn get_genl_family_group() -> (String, String) {
    get_genl_family_group_from_path(SONIC_CONSTANTS)
}

/// Internal function to read family and group from a specific file path.
/// This allows for better testability by accepting different file paths.
fn get_genl_family_group_from_path(path: &str) -> (String, String) {
    // Default values if constants file is not available
    const DEFAULT_FAMILY: &str = "sonic_stel";
    const DEFAULT_GROUP: &str = "ipfix";

    let mut fd = match File::open(path) {
        Ok(file) => file,
        Err(_) => {
            warn!("SONiC constants file not found at {}, using default values: family='{}', group='{}'", 
                  path, DEFAULT_FAMILY, DEFAULT_GROUP);
            return (DEFAULT_FAMILY.to_string(), DEFAULT_GROUP.to_string());
        }
    };

    let mut yaml_str = String::new();
    if let Err(_) = fd.read_to_string(&mut yaml_str) {
        warn!("Failed to read SONiC constants file, using default values: family='{}', group='{}'", 
              DEFAULT_FAMILY, DEFAULT_GROUP);
        return (DEFAULT_FAMILY.to_string(), DEFAULT_GROUP.to_string());
    }
    
    let docs = match YamlLoader::load_from_str(&yaml_str) {
        Ok(docs) => docs,
        Err(e) => {
            panic!("Failed to parse YAML constants file: {:?}", e);
        }
    };
    
    if docs.is_empty() {
        warn!("Empty YAML constants file, using default values: family='{}', group='{}'", 
              DEFAULT_FAMILY, DEFAULT_GROUP);
        return (DEFAULT_FAMILY.to_string(), DEFAULT_GROUP.to_string());
    }

    let constants = &docs[0];
    let high_frequency_telemetry = &constants["constants"]["high_frequency_telemetry"];
    
    let family = high_frequency_telemetry["genl_family"]
        .as_str()
        .unwrap_or_else(|| {
            warn!("Missing or invalid genl_family in constants, using default: '{}'", DEFAULT_FAMILY);
            DEFAULT_FAMILY
        })
        .to_string();
    
    let group = high_frequency_telemetry["genl_multicast_group"]
        .as_str()
        .unwrap_or_else(|| {
            warn!("Missing or invalid genl_multicast_group in constants, using default: '{}'", DEFAULT_GROUP);
            DEFAULT_GROUP
        })
        .to_string();
    
    (family, group)
}

#[cfg(test)]
mod test {
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
    }

    impl MockSocket {
        /// Creates a new MockSocket for testing.
        /// 
        /// The first socket created will have partially valid messages (including one that fails),
        /// while subsequent sockets will have only valid messages.
        pub fn new() -> Self {
            let count = SOCKET_COUNT.fetch_add(1, Ordering::SeqCst) + 1;
            
            if count == 1 {
                MockSocket {
                    valid: true,
                    budget: PARTIALLY_VALID_MESSAGES.len(),
                    messages: PARTIALLY_VALID_MESSAGES
                        .iter()
                        .map(|&msg| msg.to_vec())
                        .collect(),
                }
            } else {
                MockSocket {
                    valid: count <= 2, // Maximum of 2 valid sockets
                    budget: VALID_MESSAGES.len(),
                    messages: VALID_MESSAGES
                        .iter()
                        .map(|&msg| msg.to_vec())
                        .collect(),
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
            if self.budget == 0 {
                return Ok((0, Groups::empty()));
            }
            let msg = &self.messages[self.messages.len() - self.budget];
            self.budget -= 1;
            if !msg.is_empty() {
                let copy_len = std::cmp::min(msg.len(), buf.len());
                buf[..copy_len].copy_from_slice(&msg[..copy_len]);
                Ok((copy_len, Groups::empty()))
            } else {
                Err(io::Error::new(io::ErrorKind::ConnectionAborted, "Simulated connection failure"))
            }
        }
    }

    /// Tests the NetlinkActor's ability to handle partial failures and reconnection.
    /// 
    /// This test verifies that:
    /// - The actor correctly handles a mix of valid and invalid messages
    /// - Reconnection occurs when an empty message is encountered  
    /// - All expected payload data (without headers) are eventually received
    #[tokio::test]
    async fn test_netlink() {
        // Reset socket count for this test
        SOCKET_COUNT.store(0, Ordering::SeqCst);
        
        let (_command_sender, command_receiver) = channel(1);
        let (buffer_sender, mut buffer_receiver) = channel(1);
        let mut actor = NetlinkActor::new("family", "group", command_receiver);

        actor.add_recipient(buffer_sender);
        let task = spawn(NetlinkActor::run(actor));

        let mut received_messages = Vec::new();
        for _ in 0..4 {
            let buffer = buffer_receiver.recv().await
                .expect("Failed to receive buffer from actor");
            let message = String::from_utf8(buffer.to_vec())
                .expect("Failed to convert buffer to string");
            received_messages.push(message);
        }

        // Build expected messages: only the payload data, headers should be stripped
        let expected_messages = vec![
            "PARTIALLY_VALID1".to_string(),
            "PARTIALLY_VALID2".to_string(),
            "VALID1".to_string(),
            "VALID2".to_string(),
        ];

        assert_eq!(received_messages, expected_messages);
        assert!(SOCKET_COUNT.load(Ordering::SeqCst) > 1, "Socket should have reconnected");

        task.await.expect("Task should complete successfully");
    }

    /// Tests parsing of family and group configuration from YAML constants file.
    #[test]
    fn test_family_group_parse() {
        let (family, group) = get_genl_family_group();
        assert_eq!(family, "sonic_stel");
        assert_eq!(group, "ipfix");
    }

    /// Tests that default values are used when constants file doesn't exist.
    #[test]
    fn test_family_group_defaults_when_file_missing() {
        // Test with non-existent file - this tests the actual implementation
        let (family, group) = get_genl_family_group_from_path("/non/existent/path.yml");
        assert_eq!(family, "sonic_stel");
        assert_eq!(group, "ipfix");
    }
    
    /// Tests that the function can read from a valid constants file.
    #[test]
    fn test_family_group_from_valid_file() {
        // Test with existing file should still work
        let (family, group) = get_genl_family_group_from_path("tests/data/constants.yml");
        assert_eq!(family, "sonic_stel");
        assert_eq!(group, "ipfix");
    }

    /// Tests payload extraction from mock netlink messages.
    #[test]
    fn test_payload_extraction() {
        // Test with valid message containing payload
        let mock_msg = create_mock_netlink_message(b"TEST_PAYLOAD");
        let buffer = Arc::new(mock_msg.to_vec());
        
        let result = NetlinkActor::extract_payload(buffer);
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
        
        let result = NetlinkActor::extract_payload(buffer);
        assert!(result.is_ok());
        
        let payload = result.unwrap();
        assert!(payload.is_empty());
    }

    /// Tests payload extraction with invalid message (too small).
    #[test]
    fn test_payload_extraction_invalid_message() {
        // Buffer too small to contain headers
        let buffer = Arc::new(vec![0u8; 10]);
        
        let result = NetlinkActor::extract_payload(buffer);
        assert!(result.is_err());
        
        let error = result.unwrap_err();
        assert_eq!(error.kind(), io::ErrorKind::InvalidData);
    }
}
