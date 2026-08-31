use std::time::Duration;

use log::{debug, info, warn};

#[cfg(not(test))]
use netlink_sys::{protocols::NETLINK_GENERIC, Socket, SocketAddr};
use tokio::{
    sync::mpsc::Sender,
    time::{interval, MissedTickBehavior},
};

use std::io;

use super::super::message::netlink::NetlinkCommand;
#[cfg(not(test))]
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
/// Generic Netlink controller family ID (`GENL_ID_CTRL`).
const GENL_ID_CTRL: u16 = 0x10;
/// Generic netlink control command: CTRL_CMD_NEWFAMILY
const CTRL_CMD_NEWFAMILY: u8 = 1;
/// Generic netlink control command: CTRL_CMD_DELFAMILY  
const CTRL_CMD_DELFAMILY: u8 = 2;
/// Netlink attribute type: CTRL_ATTR_FAMILY_NAME
const CTRL_ATTR_FAMILY_NAME: u16 = 2;
/// Size of generic netlink header in bytes
const GENL_HEADER_SIZE: usize = 20;
/// Netlink control notify multicast group ID
#[cfg(not(test))]
const NLCTRL_NOTIFY_GROUP_ID: u32 = GENL_ID_CTRL as u32;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum FamilyEvent {
    Registered,
    Unregistered,
}

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
        Some(test::MockSocket)
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
        test::record_family_check();
        test::family_available()
    }

    #[cfg(not(test))]
    fn family_check_interval_ms() -> u64 {
        FAMILY_CHECK_INTERVAL_MS
    }

    #[cfg(test)]
    fn family_check_interval_ms() -> u64 {
        test::family_check_interval_ms()
    }

    /// Attempts to receive a control message from the control socket.
    ///
    /// Returns the target family's change if detected, None if no relevant message,
    /// or Err if there was an error receiving.
    async fn try_recv_control(
        socket: Option<&mut SocketType>,
        target_family: &str,
    ) -> Result<Option<FamilyEvent>, io::Error> {
        debug!("Attempting to receive control message");
        let socket = socket.ok_or_else(|| {
            io::Error::new(io::ErrorKind::NotConnected, "No control socket available")
        })?;

        let mut buffer = vec![0; BUFFER_SIZE];
        match socket.recv_from(&mut buffer, 0) {
            Ok((size, _addr)) => {
                if size == 0 {
                    return Ok(None);
                }

                buffer.resize(size, 0);
                debug!("Received control message of {} bytes", size);

                // Parse the netlink control message
                match Self::parse_control_message(&buffer, target_family) {
                    Ok(event) => {
                        if event.is_some() {
                            info!(
                                "Control message indicates family '{}' status change",
                                target_family
                            );
                        }
                        Ok(event)
                    }
                    Err(e) => {
                        debug!("Failed to parse control message: {:?}", e);
                        Ok(None) // Continue even if parsing fails
                    }
                }
            }
            Err(ref e) if e.kind() == io::ErrorKind::WouldBlock => {
                // No messages available - this is normal for non-blocking sockets
                Ok(None)
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
    /// The target family's change, or None when the message is unrelated.
    fn parse_control_message(
        buffer: &[u8],
        target_family: &str,
    ) -> Result<Option<FamilyEvent>, io::Error> {
        // Parse the netlink header
        if buffer.len() < NETLINK_HEADER_SIZE {
            return Ok(None);
        }

        let _nl_len = u32::from_le_bytes([buffer[0], buffer[1], buffer[2], buffer[3]]) as usize;
        let nl_type = u16::from_le_bytes([buffer[4], buffer[5]]);

        // Check if this is a generic netlink message
        if nl_type != GENL_ID_CTRL {
            return Ok(None);
        }

        // Parse the generic netlink header
        if buffer.len() < GENL_HEADER_SIZE {
            return Ok(None);
        }

        let genl_cmd = buffer[16];

        // Check if this is a family new/del command
        let event = match genl_cmd {
            CTRL_CMD_NEWFAMILY => FamilyEvent::Registered,
            CTRL_CMD_DELFAMILY => FamilyEvent::Unregistered,
            _ => return Ok(None),
        };

        match event {
            FamilyEvent::Registered | FamilyEvent::Unregistered => {
                debug!(
                    "Received control command: {}",
                    match event {
                        FamilyEvent::Registered => "NEWFAMILY",
                        FamilyEvent::Unregistered => "DELFAMILY",
                    }
                );

                // Parse attributes to find family name
                let attrs_start = GENL_HEADER_SIZE; // After netlink + genl headers
                if buffer.len() > attrs_start {
                    if Self::parse_family_name_from_attrs(&buffer[attrs_start..], target_family)? {
                        return Ok(Some(event));
                    }
                }
            }
        }

        Ok(None)
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
        #[cfg(test)]
        test::record_actor_family_state(family_was_available);
        let mut poll_interval = interval(Duration::from_millis(10));
        poll_interval.set_missed_tick_behavior(MissedTickBehavior::Delay);

        loop {
            poll_interval.tick().await;
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
                    Ok(Some(event)) => {
                        family_was_available = event == FamilyEvent::Registered;
                        #[cfg(test)]
                        test::record_actor_family_state(family_was_available);
                        // Family status changed, force reconnection to pick up new group ID
                        info!(
                            "Detected family '{}' {:?} event, sending reconnect command",
                            actor.family, event
                        );
                        if let Err(e) = actor.command_sender.send(NetlinkCommand::Reconnect).await {
                            warn!("Failed to send reconnect command: {:?}", e);
                            break; // Channel is closed, exit
                        }
                        #[cfg(test)]
                        test::record_reconnect_command();
                        #[cfg(test)]
                        test::record_control_message_handled();
                        continue;
                    }
                    Ok(None) => {
                        // No relevant control message, continue with periodic check
                        #[cfg(test)]
                        test::record_control_message_handled();
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
                > Self::family_check_interval_ms() as u128
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
                        #[cfg(test)]
                        test::record_reconnect_command();
                    } else {
                        warn!("Family '{}' is no longer available", actor.family);
                        // Don't send disconnect command, just let data actor handle it naturally
                    }
                    family_was_available = family_available;
                    #[cfg(test)]
                    test::record_actor_family_state(family_was_available);
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
                        if let Err(e) = actor
                            .command_sender
                            .send(NetlinkCommand::SoftReconnect)
                            .await
                        {
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
        }

        debug!("ControlNetlinkActor terminated");
    }
}

#[cfg(test)]
pub mod test {
    use super::*;
    use crate::actor::data_netlink::{self, DataNetlinkActor};
    use crate::message::buffer::SocketBufferMessage;
    use netlink_sys::SocketAddr;
    use std::{
        collections::VecDeque,
        sync::{
            atomic::{AtomicBool, AtomicUsize, Ordering},
            Mutex,
        },
        time::Duration,
    };
    use tokio::{
        spawn,
        sync::mpsc::{channel, Receiver},
        time::timeout,
    };

    struct MockControlMessage {
        bytes: Vec<u8>,
        command: u8,
        family: String,
    }

    static CONTROL_MESSAGES: Mutex<VecDeque<MockControlMessage>> = Mutex::new(VecDeque::new());
    static CONTROL_MESSAGES_RECEIVED: AtomicUsize = AtomicUsize::new(0);
    static CONTROL_MESSAGES_HANDLED: AtomicUsize = AtomicUsize::new(0);
    static RECONNECT_COMMANDS_SENT: AtomicUsize = AtomicUsize::new(0);
    static FAMILY_CHECKS: AtomicUsize = AtomicUsize::new(0);
    static FAMILY_CHECK_INTERVAL: AtomicUsize = AtomicUsize::new(FAMILY_CHECK_INTERVAL_MS as usize);
    static FAMILY_AVAILABLE: AtomicBool = AtomicBool::new(true);
    static ACTOR_FAMILY_AVAILABLE: AtomicBool = AtomicBool::new(true);
    static TARGET_FAMILY: Mutex<Option<String>> = Mutex::new(None);

    /// Mock socket for testing purposes.
    pub struct MockSocket;

    impl MockSocket {
        pub fn recv_from(
            &mut self,
            buf: &mut [u8],
            _flags: i32,
        ) -> Result<(usize, SocketAddr), io::Error> {
            let Some(message) = CONTROL_MESSAGES.lock().unwrap().pop_front() else {
                return Err(io::Error::new(
                    io::ErrorKind::WouldBlock,
                    "No control messages in test",
                ));
            };

            if TARGET_FAMILY.lock().unwrap().as_deref() == Some(message.family.as_str()) {
                match message.command {
                    CTRL_CMD_NEWFAMILY => set_kernel_family_available(true),
                    CTRL_CMD_DELFAMILY => set_kernel_family_available(false),
                    _ => {}
                }
            }

            let size = message.bytes.len().min(buf.len());
            buf[..size].copy_from_slice(&message.bytes[..size]);
            CONTROL_MESSAGES_RECEIVED.fetch_add(1, Ordering::SeqCst);
            Ok((size, SocketAddr::new(0, 0)))
        }
    }

    fn reset_control_mock(target_family: &str) {
        CONTROL_MESSAGES.lock().unwrap().clear();
        CONTROL_MESSAGES_RECEIVED.store(0, Ordering::SeqCst);
        CONTROL_MESSAGES_HANDLED.store(0, Ordering::SeqCst);
        RECONNECT_COMMANDS_SENT.store(0, Ordering::SeqCst);
        FAMILY_CHECKS.store(0, Ordering::SeqCst);
        FAMILY_CHECK_INTERVAL.store(FAMILY_CHECK_INTERVAL_MS as usize, Ordering::SeqCst);
        ACTOR_FAMILY_AVAILABLE.store(true, Ordering::SeqCst);
        *TARGET_FAMILY.lock().unwrap() = Some(target_family.to_string());
        set_kernel_family_available(true);
    }

    fn set_kernel_family_available(available: bool) {
        FAMILY_AVAILABLE.store(available, Ordering::SeqCst);
        data_netlink::test::set_connections_enabled(available);
    }

    pub(super) fn family_available() -> bool {
        FAMILY_AVAILABLE.load(Ordering::SeqCst)
    }

    pub(super) fn family_check_interval_ms() -> u64 {
        FAMILY_CHECK_INTERVAL.load(Ordering::SeqCst) as u64
    }

    pub(super) fn record_family_check() {
        FAMILY_CHECKS.fetch_add(1, Ordering::SeqCst);
    }

    pub(super) fn record_actor_family_state(available: bool) {
        ACTOR_FAMILY_AVAILABLE.store(available, Ordering::SeqCst);
    }

    pub(super) fn record_reconnect_command() {
        RECONNECT_COMMANDS_SENT.fetch_add(1, Ordering::SeqCst);
    }

    pub(super) fn record_control_message_handled() {
        CONTROL_MESSAGES_HANDLED.store(
            CONTROL_MESSAGES_RECEIVED.load(Ordering::SeqCst),
            Ordering::SeqCst,
        );
    }

    fn control_message(command: u8, family: &str) -> Vec<u8> {
        let family_name = format!("{}\0", family);
        let attr_len = 4 + family_name.len();
        let aligned_attr_len = (attr_len + 3) & !3;
        let message_len = GENL_HEADER_SIZE + aligned_attr_len;
        let mut message = vec![0u8; message_len];

        message[0..4].copy_from_slice(&(message_len as u32).to_le_bytes());
        message[4..6].copy_from_slice(&GENL_ID_CTRL.to_le_bytes());
        message[16] = command;
        message[20..22].copy_from_slice(&(attr_len as u16).to_le_bytes());
        message[22..24].copy_from_slice(&CTRL_ATTR_FAMILY_NAME.to_le_bytes());
        message[24..24 + family_name.len()].copy_from_slice(family_name.as_bytes());
        message
    }

    fn inject_control_message(command: u8, family: &str) {
        CONTROL_MESSAGES
            .lock()
            .unwrap()
            .push_back(MockControlMessage {
                bytes: control_message(command, family),
                command,
                family: family.to_string(),
            });
    }

    async fn wait_for_value(mut value: impl FnMut() -> usize, expected: usize, description: &str) {
        timeout(Duration::from_secs(1), async {
            while value() < expected {
                tokio::task::yield_now().await;
            }
        })
        .await
        .unwrap_or_else(|_| panic!("timed out waiting for {description}"));
    }

    async fn recv_payload(receiver: &mut Receiver<SocketBufferMessage>) -> String {
        let message = timeout(Duration::from_secs(1), receiver.recv())
            .await
            .expect("timed out waiting for data payload")
            .expect("data payload channel closed");
        String::from_utf8(message.to_vec()).expect("payload is valid UTF-8")
    }

    #[tokio::test]
    #[serial_test::serial]
    async fn test_control_netlink_actor() {
        reset_control_mock("test_family");

        let (command_sender, command_receiver) = channel(10);
        let actor = ControlNetlinkActor::new("test_family", command_sender);

        assert_eq!(actor.family, "test_family");
        assert!(actor.control_socket.is_some());

        drop(command_receiver);
        timeout(Duration::from_millis(100), ControlNetlinkActor::run(actor))
            .await
            .expect("control actor did not stop after its command channel closed");
    }

    /// Exercises the family lifecycle across both actors:
    /// connected -> family deleted -> disconnected -> family recreated -> connected.
    #[tokio::test]
    #[serial_test::serial]
    async fn test_control_and_data_netlink_family_lifecycle() {
        const FAMILY: &str = "test_family";
        const GROUP: &str = "test_group";
        const FAMILY_CHECKS_DISABLED_MS: usize = 60_000;

        data_netlink::test::reset_mock_state(false, 1);
        reset_control_mock(FAMILY);
        FAMILY_CHECK_INTERVAL.store(FAMILY_CHECKS_DISABLED_MS, Ordering::SeqCst);

        let (command_sender, command_receiver) = channel(10);
        let close_sender = command_sender.clone();
        let (buffer_sender, mut buffer_receiver) = channel(1);

        let mut data_actor = DataNetlinkActor::new(FAMILY, GROUP, command_receiver, 0, 5);
        data_actor.add_recipient(buffer_sender);
        let control_actor = ControlNetlinkActor::new(FAMILY, command_sender);

        let data_task = spawn(DataNetlinkActor::run(data_actor));
        let control_task = spawn(ControlNetlinkActor::run(control_actor));

        assert_eq!(
            recv_payload(&mut buffer_receiver).await,
            "test_family/test_group/socket-1"
        );
        assert_eq!(data_netlink::test::connection_attempts(), 1);
        assert_eq!(data_netlink::test::registered_socket_count(), 1);

        // A notification for another family must not affect the data socket.
        inject_control_message(CTRL_CMD_DELFAMILY, "other_family");
        wait_for_value(
            || CONTROL_MESSAGES_HANDLED.load(Ordering::SeqCst),
            1,
            "unrelated control message",
        )
        .await;
        assert_eq!(RECONNECT_COMMANDS_SENT.load(Ordering::SeqCst), 0);
        assert_eq!(data_netlink::test::connection_attempts(), 1);
        assert_eq!(data_netlink::test::live_socket_count(), 1);

        // DELFAMILY forces a reconnect. Resolution fails while the family is absent,
        // leaving the data actor disconnected with no replacement fd registered.
        inject_control_message(CTRL_CMD_DELFAMILY, FAMILY);
        wait_for_value(
            || CONTROL_MESSAGES_HANDLED.load(Ordering::SeqCst),
            2,
            "DELFAMILY control message",
        )
        .await;
        wait_for_value(
            data_netlink::test::connection_attempts,
            2,
            "failed reconnect after DELFAMILY",
        )
        .await;
        assert_eq!(RECONNECT_COMMANDS_SENT.load(Ordering::SeqCst), 1);
        assert_eq!(data_netlink::test::connection_attempts(), 2);
        assert_eq!(data_netlink::test::socket_count(), 1);
        assert_eq!(data_netlink::test::registered_socket_count(), 1);
        assert_eq!(data_netlink::test::live_socket_count(), 0);
        assert!(!ACTOR_FAMILY_AVAILABLE.load(Ordering::SeqCst));

        // The notification updated the control actor's state, so an immediate resolver
        // check must not emit a duplicate reconnect for the same unavailable state.
        let checks_after_delete = FAMILY_CHECKS.load(Ordering::SeqCst);
        FAMILY_CHECK_INTERVAL.store(0, Ordering::SeqCst);
        wait_for_value(
            || FAMILY_CHECKS.load(Ordering::SeqCst),
            checks_after_delete + 1,
            "family check after DELFAMILY",
        )
        .await;
        FAMILY_CHECK_INTERVAL.store(FAMILY_CHECKS_DISABLED_MS, Ordering::SeqCst);
        assert_eq!(RECONNECT_COMMANDS_SENT.load(Ordering::SeqCst), 1);

        // NEWFAMILY drives another reconnect. Once resolution succeeds, the replacement
        // data socket is registered and receives multicast data.
        inject_control_message(CTRL_CMD_NEWFAMILY, FAMILY);
        wait_for_value(
            || CONTROL_MESSAGES_HANDLED.load(Ordering::SeqCst),
            3,
            "NEWFAMILY control message",
        )
        .await;
        wait_for_value(
            data_netlink::test::connection_attempts,
            3,
            "successful reconnect after NEWFAMILY",
        )
        .await;
        assert_eq!(
            recv_payload(&mut buffer_receiver).await,
            "test_family/test_group/socket-2"
        );
        assert_eq!(RECONNECT_COMMANDS_SENT.load(Ordering::SeqCst), 2);
        assert_eq!(data_netlink::test::connection_attempts(), 3);
        assert_eq!(data_netlink::test::socket_count(), 2);
        assert_eq!(data_netlink::test::registered_socket_count(), 2);
        assert_eq!(data_netlink::test::live_socket_count(), 1);
        assert!(ACTOR_FAMILY_AVAILABLE.load(Ordering::SeqCst));

        // Likewise, NEWFAMILY moved the control actor back to available. The resolver
        // check observes the same state and must not reconnect the data actor again.
        let checks_after_create = FAMILY_CHECKS.load(Ordering::SeqCst);
        FAMILY_CHECK_INTERVAL.store(0, Ordering::SeqCst);
        wait_for_value(
            || FAMILY_CHECKS.load(Ordering::SeqCst),
            checks_after_create + 1,
            "family check after NEWFAMILY",
        )
        .await;
        FAMILY_CHECK_INTERVAL.store(FAMILY_CHECKS_DISABLED_MS, Ordering::SeqCst);
        assert_eq!(RECONNECT_COMMANDS_SENT.load(Ordering::SeqCst), 2);
        assert_eq!(data_netlink::test::connection_attempts(), 3);

        close_sender.send(NetlinkCommand::Close).await.unwrap();
        drop(close_sender);
        timeout(Duration::from_secs(1), data_task)
            .await
            .expect("data actor did not stop")
            .expect("data actor panicked");
        timeout(Duration::from_secs(1), control_task)
            .await
            .expect("control actor did not stop")
            .expect("control actor panicked");
    }

    /// Tests control message parsing functionality.
    #[test]
    fn test_control_message_parsing() {
        for command in [CTRL_CMD_NEWFAMILY, CTRL_CMD_DELFAMILY] {
            let message = control_message(command, "test_family");
            let expected = if command == CTRL_CMD_NEWFAMILY {
                FamilyEvent::Registered
            } else {
                FamilyEvent::Unregistered
            };
            assert_eq!(
                ControlNetlinkActor::parse_control_message(&message, "test_family").unwrap(),
                Some(expected)
            );
            assert_eq!(
                ControlNetlinkActor::parse_control_message(&message, "other_family").unwrap(),
                None
            );
        }
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
