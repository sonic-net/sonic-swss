use std::time::Duration;

use log::{debug, info, warn};

#[cfg(not(test))]
use netlink_sys::{protocols::NETLINK_GENERIC, Socket, SocketAddr};
use tokio::{
    sync::mpsc::Sender,
    time::{interval, Instant, MissedTickBehavior},
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
/// Interval for heartbeat logging.
const HEARTBEAT_LOG_INTERVAL_SECS: u64 = 60;
/// Interval for periodic reconnect commands.
const PERIODIC_RECONNECT_INTERVAL_SECS: u64 = 60;
/// Interval for control socket recreation attempts.
const CONTROL_SOCKET_RECREATE_INTERVAL_SECS: u64 = 3 * 60;
/// Minimum netlink message header size in bytes
const NETLINK_HEADER_SIZE: usize = 16;
/// Generic Netlink controller family ID (`GENL_ID_CTRL`).
const GENL_ID_CTRL: u16 = 0x10;
/// Generic netlink control command: CTRL_CMD_NEWFAMILY
const CTRL_CMD_NEWFAMILY: u8 = 1;
/// Generic netlink control command: CTRL_CMD_DELFAMILY  
const CTRL_CMD_DELFAMILY: u8 = 2;
/// Netlink attribute type: CTRL_ATTR_FAMILY_ID
#[cfg(test)]
const CTRL_ATTR_FAMILY_ID: u16 = 1;
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
    last_family_check: Instant,
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
            last_family_check: Instant::now(),
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

        // The nlctrl notify group has the same reserved ID as GENL_ID_CTRL.
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
        test::connect_control_socket()
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
        test::family_available()
    }

    /// Attempts to receive a control message from the control socket.
    ///
    /// Returns all target-family changes found in one receive,
    /// or Err if there was an error receiving.
    async fn try_recv_control(
        socket: Option<&mut SocketType>,
        target_family: &str,
    ) -> Result<Vec<FamilyEvent>, io::Error> {
        debug!("Attempting to receive control message");
        let socket = socket.ok_or_else(|| {
            io::Error::new(io::ErrorKind::NotConnected, "No control socket available")
        })?;
        let mut events = Vec::new();

        loop {
            let mut buffer = Vec::with_capacity(BUFFER_SIZE);
            match socket.recv_from(&mut buffer, 0) {
                Ok((size, _addr)) => {
                    if size == 0 {
                        continue;
                    }

                    buffer.truncate(size);
                    debug!("Received control message of {} bytes", size);

                    match Self::parse_control_datagram(&buffer, target_family) {
                        Ok(received_events) => events.extend(received_events),
                        Err(error) if events.is_empty() => return Err(error),
                        Err(error) => {
                            warn!(
                                "Ignoring malformed control datagram after {} valid event(s): {}",
                                events.len(),
                                error
                            );
                        }
                    }
                }
                Err(ref error) if error.kind() == io::ErrorKind::WouldBlock => {
                    if !events.is_empty() {
                        info!(
                            "Control messages indicate {} status change(s) for family '{}'",
                            events.len(),
                            target_family
                        );
                    }
                    return Ok(events);
                }
                Err(error) if !events.is_empty() => {
                    warn!(
                        "Control socket failed after {} valid event(s): {:?}",
                        events.len(),
                        error
                    );
                    return Ok(events);
                }
                Err(error) => {
                    debug!("Control socket error: {:?}", error);
                    return Err(error);
                }
            }
        }
    }

    fn parse_control_datagram(
        buffer: &[u8],
        target_family: &str,
    ) -> Result<Vec<FamilyEvent>, io::Error> {
        let mut events = Vec::new();
        let mut offset = 0usize;

        while offset + NETLINK_HEADER_SIZE <= buffer.len() {
            let nl_len =
                u32::from_le_bytes(buffer[offset..offset + 4].try_into().unwrap()) as usize;
            let message_end = offset
                .checked_add(nl_len)
                .filter(|end| *end <= buffer.len());
            let Some(message_end) = message_end.filter(|_| nl_len >= NETLINK_HEADER_SIZE) else {
                let error = io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("Invalid control netlink message length {nl_len} at offset {offset}"),
                );
                if events.is_empty() {
                    return Err(error);
                }
                warn!(
                    "Discarding malformed trailing control message after {} valid event(s): {}",
                    events.len(),
                    error
                );
                break;
            };
            match Self::parse_control_message(&buffer[offset..message_end], target_family) {
                Ok(Some(event)) => events.push(event),
                Ok(None) => {}
                Err(error) if !events.is_empty() => {
                    warn!(
                        "Discarding malformed trailing control message after {} valid event(s): {}",
                        events.len(),
                        error
                    );
                    break;
                }
                Err(error) => return Err(error),
            }
            let aligned_len = nl_len.checked_add(3).ok_or_else(|| {
                io::Error::new(
                    io::ErrorKind::InvalidData,
                    "Control message alignment overflow",
                )
            })? & !3;
            offset = offset.checked_add(aligned_len).ok_or_else(|| {
                io::Error::new(
                    io::ErrorKind::InvalidData,
                    "Control message offset overflow",
                )
            })?;
        }

        Ok(events)
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

        let nl_len = u32::from_le_bytes([buffer[0], buffer[1], buffer[2], buffer[3]]) as usize;
        if nl_len < GENL_HEADER_SIZE || nl_len > buffer.len() {
            return Ok(None);
        }
        let nl_type = u16::from_le_bytes([buffer[4], buffer[5]]);
        debug!(
            "Control netlink header: nl_type={}, genl_cmd={}",
            nl_type,
            buffer.get(16).copied().unwrap_or_default()
        );

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
                if nl_len > attrs_start {
                    if Self::parse_family_name_from_attrs(
                        &buffer[attrs_start..nl_len],
                        target_family,
                    )? {
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
        let mut family_was_available = true; // Assume family starts available
        let mut last_heartbeat = Instant::now();
        let mut last_periodic_reconnect = Instant::now();
        let mut last_control_socket_recreate = Instant::now();
        let mut poll_interval = interval(Duration::from_millis(10));
        poll_interval.set_missed_tick_behavior(MissedTickBehavior::Delay);

        loop {
            poll_interval.tick().await;
            let now = Instant::now();

            // Log heartbeat every minute to show the actor is running
            if now.duration_since(last_heartbeat)
                >= Duration::from_secs(HEARTBEAT_LOG_INTERVAL_SECS)
            {
                info!(
                    "ControlNetlinkActor is running normally - monitoring family '{}', family_was_available={}",
                    actor.family,
                    family_was_available,
                );
                last_heartbeat = now;
            }

            // Check for control socket activity
            if let Some(ref mut control_socket) = actor.control_socket {
                match Self::try_recv_control(Some(control_socket), &actor.family).await {
                    Ok(events) if !events.is_empty() => {
                        let final_event = *events.last().unwrap();
                        family_was_available = final_event == FamilyEvent::Registered;
                        info!(
                            "Detected {} family '{}' event(s), final state {:?}; reconnecting once",
                            events.len(),
                            actor.family,
                            final_event
                        );
                        if let Err(e) = actor.command_sender.send(NetlinkCommand::Reconnect).await {
                            warn!("Failed to send reconnect command: {:?}", e);
                            break;
                        }
                        continue;
                    }
                    Ok(_) => {
                        // No relevant control message, continue with periodic check
                    }
                    Err(e) => {
                        debug!("Failed to receive control message: {:?}", e);
                        // Don't reconnect control socket immediately, it's not critical
                        // But we should try to recreate it periodically
                        if now.duration_since(last_control_socket_recreate)
                            >= Duration::from_secs(CONTROL_SOCKET_RECREATE_INTERVAL_SECS)
                        {
                            debug!("Attempting to recreate control socket");
                            actor.control_socket = Self::connect_control_socket();
                            last_control_socket_recreate = now;
                        }
                    }
                }
            } else if now.duration_since(last_control_socket_recreate)
                >= Duration::from_secs(CONTROL_SOCKET_RECREATE_INTERVAL_SECS)
            {
                debug!("Attempting to create missing control socket");
                actor.control_socket = Self::connect_control_socket();
                last_control_socket_recreate = now;
            }

            // Perform periodic family existence check
            if now.duration_since(actor.last_family_check).as_millis()
                >= FAMILY_CHECK_INTERVAL_MS as u128
            {
                actor.last_family_check = now;
                let family_available = actor.check_family_exists();
                debug!(
                    "heartbeat: family_available={}, family_was_available={}",
                    family_available, family_was_available
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
                    if now.duration_since(last_periodic_reconnect)
                        >= Duration::from_secs(PERIODIC_RECONNECT_INTERVAL_SECS)
                    {
                        debug!(
                            "Sending periodic soft reconnect command to check data socket health"
                        );
                        if let Err(e) = actor
                            .command_sender
                            .send(NetlinkCommand::SoftReconnect)
                            .await
                        {
                            warn!("Failed to send periodic soft reconnect command: {:?}", e);
                            break; // Channel is closed, exit
                        }
                        last_periodic_reconnect = now;
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
        time::{advance, sleep, timeout},
    };

    struct MockControlMessage {
        bytes: Vec<u8>,
    }

    static CONTROL_MESSAGES: Mutex<VecDeque<MockControlMessage>> = Mutex::new(VecDeque::new());
    static TARGET_FAMILY: Mutex<Option<String>> = Mutex::new(None);
    static CONTROL_SOCKET_ERROR: AtomicBool = AtomicBool::new(false);
    static CONTROL_SOCKET_CONNECTIONS: AtomicUsize = AtomicUsize::new(0);
    static CONTROL_SOCKET_FAILURES_REMAINING: AtomicUsize = AtomicUsize::new(0);

    pub(super) fn connect_control_socket() -> Option<MockSocket> {
        CONTROL_SOCKET_CONNECTIONS.fetch_add(1, Ordering::SeqCst);
        let remaining = CONTROL_SOCKET_FAILURES_REMAINING.load(Ordering::SeqCst);
        if remaining > 0 {
            CONTROL_SOCKET_FAILURES_REMAINING.fetch_sub(1, Ordering::SeqCst);
            None
        } else {
            Some(MockSocket)
        }
    }

    /// Mock socket for testing purposes.
    pub struct MockSocket;

    impl MockSocket {
        pub fn recv_from(
            &mut self,
            buf: &mut Vec<u8>,
            _flags: i32,
        ) -> Result<(usize, SocketAddr), io::Error> {
            if CONTROL_SOCKET_ERROR.load(Ordering::SeqCst) {
                return Err(io::Error::new(
                    io::ErrorKind::ConnectionAborted,
                    "simulated control socket failure",
                ));
            }
            let Some(message) = CONTROL_MESSAGES.lock().unwrap().pop_front() else {
                return Err(io::Error::new(
                    io::ErrorKind::WouldBlock,
                    "No control messages in test",
                ));
            };

            buf.extend_from_slice(&message.bytes);
            let size = message.bytes.len();
            Ok((size, SocketAddr::new(0, 0)))
        }
    }

    fn reset_control_mock(target_family: &str) {
        CONTROL_MESSAGES.lock().unwrap().clear();
        *TARGET_FAMILY.lock().unwrap() = Some(target_family.to_string());
        CONTROL_SOCKET_ERROR.store(false, Ordering::SeqCst);
        CONTROL_SOCKET_CONNECTIONS.store(0, Ordering::SeqCst);
        CONTROL_SOCKET_FAILURES_REMAINING.store(0, Ordering::SeqCst);
    }

    pub(super) fn family_available() -> bool {
        TARGET_FAMILY
            .lock()
            .unwrap()
            .as_ref()
            .is_some_and(|family| data_netlink::test::mock_family_exists(family))
    }

    fn control_message(command: u8, family: &str, family_id: u16) -> Vec<u8> {
        let family_name = format!("{}\0", family);
        let id_attr_len = 6usize;
        let aligned_id_attr_len = (id_attr_len + 3) & !3;
        let name_attr_len = 4 + family_name.len();
        let aligned_name_attr_len = (name_attr_len + 3) & !3;
        let message_len = GENL_HEADER_SIZE + aligned_id_attr_len + aligned_name_attr_len;
        let mut message = vec![0u8; message_len];

        message[0..4].copy_from_slice(&(message_len as u32).to_le_bytes());
        message[4..6].copy_from_slice(&GENL_ID_CTRL.to_le_bytes());
        message[16] = command;
        message[20..22].copy_from_slice(&(id_attr_len as u16).to_le_bytes());
        message[22..24].copy_from_slice(&CTRL_ATTR_FAMILY_ID.to_le_bytes());
        message[24..26].copy_from_slice(&family_id.to_le_bytes());
        let name_offset = GENL_HEADER_SIZE + aligned_id_attr_len;
        message[name_offset..name_offset + 2]
            .copy_from_slice(&(name_attr_len as u16).to_le_bytes());
        message[name_offset + 2..name_offset + 4]
            .copy_from_slice(&CTRL_ATTR_FAMILY_NAME.to_le_bytes());
        message[name_offset + 4..name_offset + 4 + family_name.len()]
            .copy_from_slice(family_name.as_bytes());
        message
    }

    fn inject_family_deleted(family: &str, family_id: u16) {
        data_netlink::test::unregister_mock_family(family, family_id);
        CONTROL_MESSAGES
            .lock()
            .unwrap()
            .push_back(MockControlMessage {
                bytes: control_message(CTRL_CMD_DELFAMILY, family, family_id),
            });
    }

    fn inject_family_registered(family: &str, group: &str, family_id: u16, group_id: u32) {
        data_netlink::test::register_mock_family(family, group, family_id, group_id);
        CONTROL_MESSAGES
            .lock()
            .unwrap()
            .push_back(MockControlMessage {
                bytes: control_message(CTRL_CMD_NEWFAMILY, family, family_id),
            });
    }

    async fn wait_for_value(mut value: impl FnMut() -> usize, expected: usize, description: &str) {
        timeout(Duration::from_secs(1), async {
            while value() < expected {
                sleep(Duration::from_millis(1)).await;
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

    struct RunningActors {
        close_sender: Sender<NetlinkCommand>,
        buffer_receiver: Receiver<SocketBufferMessage>,
        data_task: tokio::task::JoinHandle<()>,
        control_task: tokio::task::JoinHandle<()>,
    }

    async fn start_actors(
        family: &str,
        group: &str,
        family_id: u16,
        group_id: u32,
    ) -> RunningActors {
        data_netlink::test::reset_mock_state(false, 1);
        data_netlink::test::use_mock_kernel_registry();
        data_netlink::test::register_mock_family(family, group, family_id, group_id);
        reset_control_mock(family);

        let (command_sender, command_receiver) = channel(10);
        let close_sender = command_sender.clone();
        let (buffer_sender, buffer_receiver) = channel(4);
        let mut data_actor = DataNetlinkActor::new(family, group, command_receiver, 0, 5);
        data_actor.add_recipient(buffer_sender);
        let control_actor = ControlNetlinkActor::new(family, command_sender);

        RunningActors {
            close_sender,
            buffer_receiver,
            data_task: spawn(DataNetlinkActor::run(data_actor)),
            control_task: spawn(ControlNetlinkActor::run(control_actor)),
        }
    }

    async fn stop_actors(actors: RunningActors) {
        actors
            .close_sender
            .send(NetlinkCommand::Close)
            .await
            .unwrap();
        drop(actors.close_sender);
        timeout(Duration::from_secs(1), actors.data_task)
            .await
            .expect("data actor did not stop")
            .expect("data actor panicked");
        timeout(Duration::from_secs(1), actors.control_task)
            .await
            .expect("control actor did not stop")
            .expect("control actor panicked");
    }

    async fn wait_for_connection_attempts(expected: usize) {
        wait_for_value(
            data_netlink::test::connection_attempts,
            expected,
            "data connection attempt",
        )
        .await;
    }

    async fn advance_control_ticks(count: usize) {
        for _ in 0..count {
            advance(Duration::from_millis(10)).await;
            tokio::task::yield_now().await;
        }
    }

    async fn settle_actor_tasks() {
        for _ in 0..16 {
            tokio::task::yield_now().await;
        }
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

    #[tokio::test(start_paused = true)]
    #[serial_test::serial]
    async fn test_family_reconnects_after_one_minute_outage() {
        const FAMILY: &str = "test_family";
        const GROUP: &str = "test_group";
        const OLD_FAMILY_ID: u16 = 0x20;
        const OLD_GROUP_ID: u32 = 0x100;
        const NEW_FAMILY_ID: u16 = 0x21;
        const NEW_GROUP_ID: u32 = 0x101;

        let mut actors = start_actors(FAMILY, GROUP, OLD_FAMILY_ID, OLD_GROUP_ID).await;
        assert_eq!(
            data_netlink::test::current_subscription(),
            Some((OLD_FAMILY_ID, OLD_GROUP_ID))
        );
        assert!(data_netlink::test::send_kernel_data(
            OLD_FAMILY_ID,
            OLD_GROUP_ID,
            b"before-outage"
        ));
        assert_eq!(
            recv_payload(&mut actors.buffer_receiver).await,
            "before-outage"
        );

        inject_family_deleted(FAMILY, OLD_FAMILY_ID);
        wait_for_connection_attempts(2).await;
        assert_eq!(data_netlink::test::live_socket_count(), 0);
        assert_eq!(data_netlink::test::current_subscription(), None);

        advance_control_ticks(5_900).await;
        assert_eq!(data_netlink::test::connection_attempts(), 2);
        assert_eq!(data_netlink::test::live_socket_count(), 0);

        advance_control_ticks(100).await;
        inject_family_registered(FAMILY, GROUP, NEW_FAMILY_ID, NEW_GROUP_ID);
        wait_for_connection_attempts(3).await;
        assert_eq!(
            data_netlink::test::current_subscription(),
            Some((NEW_FAMILY_ID, NEW_GROUP_ID))
        );
        assert_eq!(data_netlink::test::live_socket_count(), 1);
        assert!(!data_netlink::test::send_kernel_data(
            OLD_FAMILY_ID,
            OLD_GROUP_ID,
            b"stale-after-minute"
        ));
        assert!(data_netlink::test::send_kernel_data(
            NEW_FAMILY_ID,
            NEW_GROUP_ID,
            b"after-minute"
        ));
        assert_eq!(
            recv_payload(&mut actors.buffer_receiver).await,
            "after-minute"
        );

        stop_actors(actors).await;
    }

    #[tokio::test]
    #[serial_test::serial]
    async fn test_family_reconnects_quickly_with_new_ids() {
        const FAMILY: &str = "test_family";
        const GROUP: &str = "test_group";
        const OLD_FAMILY_ID: u16 = 0x30;
        const OLD_GROUP_ID: u32 = 0x200;
        const NEW_FAMILY_ID: u16 = 0x44;
        const NEW_GROUP_ID: u32 = 0x2ff;

        let mut actors = start_actors(FAMILY, GROUP, OLD_FAMILY_ID, OLD_GROUP_ID).await;
        // Model a fast kernel replacement: by the time DELFAMILY is consumed, the
        // registry already exposes the new generation and both notifications are queued.
        inject_family_deleted(FAMILY, OLD_FAMILY_ID);
        inject_family_registered(FAMILY, GROUP, NEW_FAMILY_ID, NEW_GROUP_ID);
        wait_for_connection_attempts(2).await;

        assert_eq!(
            data_netlink::test::successful_connections(),
            vec![(OLD_FAMILY_ID, OLD_GROUP_ID), (NEW_FAMILY_ID, NEW_GROUP_ID)]
        );
        assert_eq!(
            data_netlink::test::current_subscription(),
            Some((NEW_FAMILY_ID, NEW_GROUP_ID))
        );
        assert_eq!(data_netlink::test::live_socket_count(), 1);
        assert!(!data_netlink::test::send_kernel_data(
            OLD_FAMILY_ID,
            OLD_GROUP_ID,
            b"stale-id"
        ));
        assert!(data_netlink::test::send_kernel_data(
            NEW_FAMILY_ID,
            NEW_GROUP_ID,
            b"new-id"
        ));
        assert_eq!(recv_payload(&mut actors.buffer_receiver).await, "new-id");

        stop_actors(actors).await;
    }

    #[tokio::test]
    #[serial_test::serial]
    async fn test_data_socket_failure_reconnects_and_receives_again() {
        const FAMILY: &str = "test_family";
        const GROUP: &str = "test_group";
        const FAMILY_ID: u16 = 0x50;
        const GROUP_ID: u32 = 0x300;

        let mut actors = start_actors(FAMILY, GROUP, FAMILY_ID, GROUP_ID).await;
        assert!(data_netlink::test::send_kernel_data(
            FAMILY_ID,
            GROUP_ID,
            b"before-failure"
        ));
        assert_eq!(
            recv_payload(&mut actors.buffer_receiver).await,
            "before-failure"
        );

        data_netlink::test::fail_current_socket_on_next_recv();
        assert!(data_netlink::test::send_kernel_data(
            FAMILY_ID,
            GROUP_ID,
            b"trigger-failure"
        ));
        wait_for_connection_attempts(2).await;
        wait_for_value(
            data_netlink::test::registered_socket_count,
            2,
            "replacement socket registration",
        )
        .await;
        assert_eq!(data_netlink::test::live_socket_count(), 1);
        assert_eq!(
            data_netlink::test::successful_connections(),
            vec![(FAMILY_ID, GROUP_ID), (FAMILY_ID, GROUP_ID)]
        );

        assert!(data_netlink::test::send_kernel_data(
            FAMILY_ID,
            GROUP_ID,
            b"after-failure"
        ));
        assert_eq!(
            recv_payload(&mut actors.buffer_receiver).await,
            "after-failure"
        );

        stop_actors(actors).await;
    }

    #[tokio::test(start_paused = true)]
    #[serial_test::serial]
    async fn test_periodic_family_check_recovers_missed_notifications() {
        const FAMILY: &str = "test_family";
        const GROUP: &str = "test_group";
        const OLD_FAMILY_ID: u16 = 0x60;
        const OLD_GROUP_ID: u32 = 0x400;
        const NEW_FAMILY_ID: u16 = 0x61;
        const NEW_GROUP_ID: u32 = 0x401;

        let actors = start_actors(FAMILY, GROUP, OLD_FAMILY_ID, OLD_GROUP_ID).await;
        settle_actor_tasks().await;
        data_netlink::test::unregister_mock_family(FAMILY, OLD_FAMILY_ID);

        advance_control_ticks(99).await;
        assert_eq!(data_netlink::test::connection_attempts(), 1);
        advance_control_ticks(1).await;
        assert_eq!(data_netlink::test::connection_attempts(), 1);

        data_netlink::test::register_mock_family(FAMILY, GROUP, NEW_FAMILY_ID, NEW_GROUP_ID);
        advance_control_ticks(99).await;
        assert_eq!(data_netlink::test::connection_attempts(), 1);
        advance_control_ticks(1).await;
        settle_actor_tasks().await;

        assert_eq!(data_netlink::test::connection_attempts(), 2);
        assert_eq!(
            data_netlink::test::current_subscription(),
            Some((NEW_FAMILY_ID, NEW_GROUP_ID))
        );
        stop_actors(actors).await;
    }

    #[tokio::test(start_paused = true)]
    #[serial_test::serial]
    async fn test_soft_reconnect_uses_sixty_second_elapsed_timeout() {
        const FAMILY: &str = "test_family";
        const GROUP: &str = "test_group";

        let actors = start_actors(FAMILY, GROUP, 0x70, 0x500).await;
        settle_actor_tasks().await;
        advance(Duration::from_secs(59)).await;
        settle_actor_tasks().await;
        assert_eq!(data_netlink::test::connection_attempts(), 1);

        advance(Duration::from_secs(1)).await;
        settle_actor_tasks().await;
        assert_eq!(data_netlink::test::connection_attempts(), 2);
        assert_eq!(data_netlink::test::live_socket_count(), 1);
        stop_actors(actors).await;
    }

    #[tokio::test(start_paused = true)]
    #[serial_test::serial]
    async fn test_control_socket_recreates_after_three_minutes_elapsed() {
        reset_control_mock("test_family");
        CONTROL_SOCKET_ERROR.store(true, Ordering::SeqCst);
        let (command_sender, command_receiver) = channel(10);
        let actor = ControlNetlinkActor::new("test_family", command_sender);
        assert_eq!(CONTROL_SOCKET_CONNECTIONS.load(Ordering::SeqCst), 1);
        let task = spawn(ControlNetlinkActor::run(actor));
        settle_actor_tasks().await;

        advance(Duration::from_secs(179)).await;
        settle_actor_tasks().await;
        assert_eq!(CONTROL_SOCKET_CONNECTIONS.load(Ordering::SeqCst), 1);

        advance(Duration::from_secs(1)).await;
        settle_actor_tasks().await;
        assert_eq!(CONTROL_SOCKET_CONNECTIONS.load(Ordering::SeqCst), 2);

        drop(command_receiver);
        advance(Duration::from_millis(10)).await;
        timeout(Duration::from_secs(1), task)
            .await
            .expect("control actor did not stop")
            .expect("control actor panicked");
    }

    /// Tests control message parsing functionality.
    #[test]
    fn test_control_message_parsing() {
        for command in [CTRL_CMD_NEWFAMILY, CTRL_CMD_DELFAMILY] {
            let message = control_message(command, "test_family", 0x20);
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

    #[test]
    fn test_multiple_control_messages_in_one_datagram() {
        let mut datagram = control_message(CTRL_CMD_DELFAMILY, "test_family", 0x20);
        datagram.extend(control_message(CTRL_CMD_NEWFAMILY, "test_family", 0x21));

        assert_eq!(
            ControlNetlinkActor::parse_control_datagram(&datagram, "test_family").unwrap(),
            vec![FamilyEvent::Unregistered, FamilyEvent::Registered]
        );
    }

    #[test]
    fn test_valid_event_before_malformed_trailing_message_is_kept() {
        let mut datagram = control_message(CTRL_CMD_DELFAMILY, "test_family", 0x20);
        datagram.extend_from_slice(&u32::MAX.to_le_bytes());
        datagram.resize(datagram.len() + NETLINK_HEADER_SIZE - 4, 0);

        assert_eq!(
            ControlNetlinkActor::parse_control_datagram(&datagram, "test_family").unwrap(),
            vec![FamilyEvent::Unregistered]
        );
    }

    #[tokio::test(start_paused = true)]
    #[serial_test::serial]
    async fn test_missing_control_socket_is_retried_after_three_minutes() {
        reset_control_mock("test_family");
        CONTROL_SOCKET_FAILURES_REMAINING.store(1, Ordering::SeqCst);
        let (command_sender, command_receiver) = channel(10);
        let actor = ControlNetlinkActor::new("test_family", command_sender);
        assert!(actor.control_socket.is_none());
        assert_eq!(CONTROL_SOCKET_CONNECTIONS.load(Ordering::SeqCst), 1);
        let task = spawn(ControlNetlinkActor::run(actor));
        settle_actor_tasks().await;

        advance(Duration::from_secs(179)).await;
        settle_actor_tasks().await;
        assert_eq!(CONTROL_SOCKET_CONNECTIONS.load(Ordering::SeqCst), 1);

        advance(Duration::from_secs(1)).await;
        settle_actor_tasks().await;
        assert_eq!(CONTROL_SOCKET_CONNECTIONS.load(Ordering::SeqCst), 2);

        drop(command_receiver);
        advance(Duration::from_millis(10)).await;
        timeout(Duration::from_secs(1), task)
            .await
            .expect("control actor did not stop")
            .expect("control actor panicked");
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
