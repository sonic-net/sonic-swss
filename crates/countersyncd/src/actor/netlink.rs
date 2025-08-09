use std::{
    collections::LinkedList,
    fs::File,
    io::{self, prelude::*},
    sync::Arc,
    thread::sleep,
    time::Duration,
};

use log::{error, warn};

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
        
        Ok(buffer)
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
                            warn!("Failed to receive message: {:?}", e);
                            actor.socket = None;
                            actor.reconnect();
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
    const PARTIALLY_VALID_MESSAGES: [&str; 4] = [
        "PARTIALLY_VALID1",
        "PARTIALLY_VALID2", 
        "", // Empty string simulates reconnection scenario
        "PARTIALLY_VALID3",
    ];

    const VALID_MESSAGES: [&str; 2] = ["VALID1", "VALID2"];

    // Use atomic counter instead of unsafe static mut for thread safety
    static SOCKET_COUNT: AtomicUsize = AtomicUsize::new(0);

    /// Mock socket implementation for testing netlink functionality.
    /// 
    /// Simulates different socket behaviors for testing reconnection logic.
    pub struct MockSocket {
        pub valid: bool,
        budget: usize,
        messages: Vec<String>,
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
                        .map(|&s| s.to_string()) // Pattern destructuring: &&str -> &str
                        .collect(),
                }
            } else {
                MockSocket {
                    valid: count <= 2, // Maximum of 2 valid sockets
                    budget: VALID_MESSAGES.len(),
                    messages: VALID_MESSAGES
                        .iter()
                        .map(|&s| s.to_string()) // Pattern destructuring: &&str -> &str
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
            let msg = self.messages[self.messages.len() - self.budget].clone();
            self.budget -= 1;
            if !msg.is_empty() {
                buf[..msg.len()].clone_from_slice(msg.as_bytes());
                Ok((msg.len(), Groups::empty()))
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
    /// - All expected messages are eventually received
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

        // Build expected messages: valid messages from first socket, then from second socket
        let mut expected_messages = Vec::new();
        for &msg in &PARTIALLY_VALID_MESSAGES {
            if msg.is_empty() {
                break; // Stop at empty message that triggers reconnect
            }
            expected_messages.push(msg.to_string());
        }
        for &msg in &VALID_MESSAGES {
            expected_messages.push(msg.to_string());
        }

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
}
