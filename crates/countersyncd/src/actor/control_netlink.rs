use std::time::Duration;

use log::{debug, info, warn};
use netlink_packet_core::{NetlinkBuffer, NETLINK_HEADER_LEN};
use netlink_packet_generic::{
    constants::{
        CTRL_ATTR_FAMILY_ID, CTRL_ATTR_FAMILY_NAME, CTRL_CMD_DELFAMILY,
        CTRL_CMD_NEWFAMILY,
    },
    GenlBuffer,
};
use netlink_packet_utils::{
    nla::NlasIterator,
    parsers::{parse_string, parse_u16},
};

#[cfg(not(test))]
use netlink_sys::{protocols::NETLINK_GENERIC, Socket, SocketAddr};
#[cfg(test)]
use netlink_sys::SocketAddr;
use tokio::{
    io::unix::AsyncFd,
    select,
    sync::mpsc::Sender,
    time::{interval, MissedTickBehavior},
};

use std::io;

use super::super::message::netlink::{NetlinkCommand, NetlinkSubscription};
#[cfg(not(test))]
use super::netlink_utils;

#[cfg(not(test))]
type SocketType = Socket;
#[cfg(test)]
type SocketType = test::MockSocket;

/// Size of the mock buffer used for receiving netlink messages.
#[cfg(test)]
const BUFFER_SIZE: usize = 0xFFFF;
/// Interval for periodic family existence checks (in milliseconds)
const FAMILY_CHECK_INTERVAL_MS: u64 = 1_000_u64;
/// Interval for heartbeat logging.
const HEARTBEAT_LOG_INTERVAL_SECS: u64 = 60;
/// Interval for control socket recreation attempts.
const CONTROL_SOCKET_RECREATE_INTERVAL_SECS: u64 = 3 * 60;
/// Generic Netlink controller family ID (`GENL_ID_CTRL`).
const GENL_ID_CTRL: u16 = 0x10;
const MAX_CONTROL_DATAGRAMS_PER_WAKE: usize = 64;
/// Netlink control notify multicast group ID
#[cfg(not(test))]
const NLCTRL_NOTIFY_GROUP_ID: u32 = GENL_ID_CTRL as u32;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum FamilyEvent {
    Registered(u16),
    Unregistered(u16),
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum SubscriptionState {
    Unknown,
    Absent,
    Present(NetlinkSubscription),
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
    /// The multicast group required by the data actor.
    group: String,
    /// Control socket for monitoring family registration/unregistration
    control_socket: Option<SocketType>,
    /// Channel for sending commands to data netlink actor
    command_sender: Sender<NetlinkCommand>,
    /// Reusable netlink socket for family existence checks
    #[cfg(not(test))]
    resolver: Option<Socket>,
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
    pub fn new(family: &str, group: &str, command_sender: Sender<NetlinkCommand>) -> Self {
        let mut actor = ControlNetlinkActor {
            family: family.to_string(),
            group: group.to_string(),
            control_socket: None,
            command_sender,
            #[cfg(not(test))]
            resolver: None,
        };

        actor.control_socket = Self::connect_control_socket();

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

    #[cfg(not(test))]
    async fn resolve_subscription(&mut self) -> Result<Option<NetlinkSubscription>, io::Error> {
        let family = self.family.clone();
        let group = self.group.clone();
        let resolver = self.resolver.take();
        let (resolver, result) = tokio::task::spawn_blocking(move || {
            let mut resolver = match resolver {
                Some(resolver) => resolver,
                None => match netlink_utils::create_nl_resolver() {
                    Ok(resolver) => resolver,
                    Err(error) => return (None, Err(error)),
                },
            };
            let result = netlink_utils::resolve_family_group(&mut resolver, &family, &group);
            (Some(resolver), result)
        })
        .await
        .map_err(|error| {
            io::Error::new(
                io::ErrorKind::Other,
                format!("resolver task failed: {error}"),
            )
        })?;
        self.resolver = resolver;

        match result {
            Ok(subscription) => Ok(Some(subscription)),
            Err(error) if error.raw_os_error() == Some(libc::ENOENT) => Ok(None),
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(None),
            Err(error) => Err(error),
        }
    }

    #[cfg(test)]
    async fn resolve_subscription(&mut self) -> Result<Option<NetlinkSubscription>, io::Error> {
        Ok(test::resolved_subscription())
    }

    #[cfg(not(test))]
    fn recv_control_datagram(
        socket: &mut SocketType,
    ) -> Result<(Vec<u8>, SocketAddr), io::Error> {
        socket.recv_from_full()
    }

    #[cfg(test)]
    fn recv_control_datagram(
        socket: &mut SocketType,
    ) -> Result<(Vec<u8>, SocketAddr), io::Error> {
        socket.recv_from_full()
    }

    /// Attempts to receive a control message from the control socket.
    ///
    /// Returns all target-family changes found in one receive,
    /// or Err if there was an error receiving.
    async fn try_recv_control(
        socket: &mut SocketType,
        target_family: &str,
    ) -> Result<(Vec<FamilyEvent>, bool), io::Error> {
        debug!("Attempting to receive control message");
        let mut events = Vec::new();

        for _ in 0..MAX_CONTROL_DATAGRAMS_PER_WAKE {
            match Self::recv_control_datagram(socket) {
                Ok((buffer, source)) => {
                    if buffer.is_empty() {
                        return Err(io::Error::new(
                            io::ErrorKind::InvalidData,
                            "Received empty control netlink datagram",
                        ));
                    }
                    if source.port_number() != 0 {
                        return Err(io::Error::new(
                            io::ErrorKind::PermissionDenied,
                            format!(
                                "Ignoring control netlink datagram from userspace port {}",
                                source.port_number()
                            ),
                        ));
                    }

                    debug!("Received control message of {} bytes", buffer.len());

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
                    return Ok((events, true));
                }
                Err(error) if !events.is_empty() => {
                    warn!(
                        "Control socket failed after {} valid event(s): {:?}",
                        events.len(),
                        error
                    );
                    return Ok((events, false));
                }
                Err(error) => {
                    debug!("Control socket error: {:?}", error);
                    return Err(error);
                }
            }
        }

        Ok((events, false))
    }

    fn parse_control_datagram(
        buffer: &[u8],
        target_family: &str,
    ) -> Result<Vec<FamilyEvent>, io::Error> {
        let mut events = Vec::new();
        let mut offset = 0usize;

        while offset + NETLINK_HEADER_LEN <= buffer.len() {
            let packet = match NetlinkBuffer::new_checked(&buffer[offset..]) {
                Ok(packet) => packet,
                Err(error) if !events.is_empty() => {
                    warn!(
                        "Discarding malformed trailing control message after {} valid event(s): {}",
                        events.len(),
                        error
                    );
                    break;
                }
                Err(error) => {
                    return Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("Invalid control netlink message at offset {offset}: {error}"),
                    ));
                }
            };
            let nl_len = packet.length() as usize;
            let message_end = offset
                .checked_add(nl_len)
                .filter(|end| *end <= buffer.len());
            let Some(message_end) = message_end.filter(|_| nl_len >= NETLINK_HEADER_LEN) else {
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
        let netlink = NetlinkBuffer::new_checked(buffer).map_err(|error| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Failed to parse control netlink header: {error}"),
            )
        })?;
        if netlink.message_type() != GENL_ID_CTRL {
            return Ok(None);
        }
        let generic = GenlBuffer::new_checked(netlink.payload()).map_err(|error| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Failed to parse Generic Netlink header: {error}"),
            )
        })?;
        let registered = match generic.cmd() {
            CTRL_CMD_NEWFAMILY => true,
            CTRL_CMD_DELFAMILY => false,
            _ => return Ok(None),
        };

        let mut family_id = None;
        let mut family_name = None;
        for attribute in NlasIterator::new(generic.payload()) {
            let attribute = attribute.map_err(|error| {
                io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("Failed to parse control attribute: {error}"),
                )
            })?;
            match attribute.kind() {
                CTRL_ATTR_FAMILY_ID => {
                    family_id = Some(parse_u16(attribute.value()).map_err(|error| {
                        io::Error::new(
                            io::ErrorKind::InvalidData,
                            format!("Invalid control family ID: {error}"),
                        )
                    })?);
                }
                CTRL_ATTR_FAMILY_NAME => {
                    if attribute.value().last() != Some(&0) {
                        return Err(io::Error::new(
                            io::ErrorKind::InvalidData,
                            "Control family name is not NUL-terminated",
                        ));
                    }
                    family_name = Some(parse_string(attribute.value()).map_err(|error| {
                        io::Error::new(
                            io::ErrorKind::InvalidData,
                            format!("Invalid control family name: {error}"),
                        )
                    })?);
                }
                _ => {}
            }
        }
        if family_name.as_deref() != Some(target_family) {
            return Ok(None);
        }
        let family_id = family_id.ok_or_else(|| {
            io::Error::new(io::ErrorKind::InvalidData, "Control event has no family ID")
        })?;
        Ok(Some(if registered {
            FamilyEvent::Registered(family_id)
        } else {
            FamilyEvent::Unregistered(family_id)
        }))
    }

    async fn apply_state(
        &self,
        state: &mut SubscriptionState,
        next: SubscriptionState,
        force_reconnect: bool,
    ) -> bool {
        let command = match next {
            SubscriptionState::Present(subscription) if force_reconnect => {
                Some(NetlinkCommand::Reconnect(subscription))
            }
            SubscriptionState::Present(subscription) => Some(NetlinkCommand::Connect(subscription)),
            SubscriptionState::Absent if *state != SubscriptionState::Absent => {
                Some(NetlinkCommand::Disconnect)
            }
            _ => None,
        };
        *state = next;
        if let Some(command) = command {
            if let Err(error) = self.command_sender.send(command).await {
                warn!("Failed to update data netlink subscription: {error:?}");
                return false;
            }
        }
        true
    }

    async fn reconcile(&mut self, state: &mut SubscriptionState, force_reconnect: bool) -> bool {
        match self.resolve_subscription().await {
            Ok(Some(subscription)) => {
                self.apply_state(
                    state,
                    SubscriptionState::Present(subscription),
                    force_reconnect,
                )
                .await
            }
            Ok(None) => {
                self.apply_state(state, SubscriptionState::Absent, false)
                    .await
            }
            Err(error) => {
                warn!(
                    "Could not reconcile family '{}' and group '{}'; preserving {:?}: {:?}",
                    self.family, self.group, state, error
                );
                true
            }
        }
    }

    /// Continuously monitors for netlink family status changes.
    /// The loop will monitor the family and send reconnection commands when needed.
    ///
    /// # Arguments
    ///
    /// * `actor` - The ControlNetlinkActor instance to run
    pub async fn run(mut actor: ControlNetlinkActor) {
        debug!("Starting ControlNetlinkActor for family '{}'", actor.family);
        let mut state = SubscriptionState::Unknown;
        let mut family_checks = interval(Duration::from_millis(FAMILY_CHECK_INTERVAL_MS));
        family_checks.set_missed_tick_behavior(MissedTickBehavior::Delay);
        family_checks.tick().await;
        let mut heartbeat = interval(Duration::from_secs(HEARTBEAT_LOG_INTERVAL_SECS));
        heartbeat.set_missed_tick_behavior(MissedTickBehavior::Delay);
        heartbeat.tick().await;
        let mut recreate = interval(Duration::from_secs(CONTROL_SOCKET_RECREATE_INTERVAL_SECS));
        recreate.set_missed_tick_behavior(MissedTickBehavior::Delay);
        recreate.tick().await;

        if !actor.reconcile(&mut state, false).await {
            return;
        }
        let mut control_socket = actor
            .control_socket
            .take()
            .and_then(|socket| match AsyncFd::new(socket) {
                Ok(socket) => Some(socket),
                Err(error) => {
                    warn!("Failed to register control netlink socket: {error:?}");
                    None
                }
            });

        loop {
            select! {
                biased;
                readiness = async { control_socket.as_mut().unwrap().readable_mut().await }, if control_socket.is_some() => {
                    match readiness {
                        Ok(mut guard) => match Self::try_recv_control(guard.get_inner_mut(), &actor.family).await {
                            Ok((events, drained)) => {
                                if drained {
                                    guard.clear_ready();
                                }
                                if let Some(final_event) = events.last().copied() {
                                    let event_id = match final_event {
                                        FamilyEvent::Registered(id) | FamilyEvent::Unregistered(id) => id,
                                    };
                                    let current_id = match state {
                                        SubscriptionState::Present(subscription) => Some(subscription.family_id),
                                        _ => None,
                                    };
                                    if matches!(final_event, FamilyEvent::Unregistered(_))
                                        && current_id.map_or(true, |id| id == event_id)
                                        && !actor.apply_state(&mut state, SubscriptionState::Absent, false).await
                                    {
                                        break;
                                    }
                                    if matches!(final_event, FamilyEvent::Registered(_))
                                        && !actor.reconcile(&mut state, true).await
                                    {
                                        break;
                                    }
                                }
                            }
                            Err(error) if error.kind() == io::ErrorKind::WouldBlock => guard.clear_ready(),
                            Err(error) if error.kind() == io::ErrorKind::InvalidData
                                || error.kind() == io::ErrorKind::PermissionDenied => {
                                warn!("Dropping invalid control netlink datagram: {error}");
                                if !actor.reconcile(&mut state, false).await {
                                    break;
                                }
                            }
                            Err(error) => {
                                warn!("Control netlink socket failed: {error:?}");
                                control_socket = None;
                            }
                        },
                        Err(error) => {
                            warn!("Control netlink readiness failed: {error:?}");
                            control_socket = None;
                        }
                    }
                }
                _ = family_checks.tick() => {
                    if !actor.reconcile(&mut state, false).await {
                        break;
                    }
                }
                _ = recreate.tick(), if control_socket.is_none() => {
                    if let Some(socket) = Self::connect_control_socket() {
                        match AsyncFd::new(socket) {
                            Ok(socket) => {
                                control_socket = Some(socket);
                                if !actor.reconcile(&mut state, false).await {
                                    break;
                                }
                            }
                            Err(error) => warn!("Failed to register control netlink socket: {error:?}"),
                        }
                    }
                }
                _ = heartbeat.tick() => info!(
                    "ControlNetlinkActor is monitoring family '{}', group '{}', state={:?}",
                    actor.family, actor.group, state
                ),
                _ = actor.command_sender.closed() => break,
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
    use netlink_packet_generic::{
        constants::{
            CTRL_ATTR_FAMILY_ID, CTRL_ATTR_FAMILY_NAME, CTRL_CMD_DELFAMILY,
            CTRL_CMD_NEWFAMILY, GENL_HDRLEN,
        },
    };
    use std::{
        os::fd::{AsRawFd, RawFd},
        os::unix::net::UnixDatagram,
        sync::{
            atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering},
            Mutex,
        },
        time::Duration,
    };
    use tokio::{
        spawn,
        sync::mpsc::{channel, Receiver},
        time::{advance, timeout},
    };

    static TARGET_FAMILY: Mutex<Option<String>> = Mutex::new(None);
    static TARGET_GROUP: Mutex<Option<String>> = Mutex::new(None);
    static CONTROL_SENDER: Mutex<Option<UnixDatagram>> = Mutex::new(None);
    static CONTROL_SOCKET_ERROR: AtomicBool = AtomicBool::new(false);
    static CONTROL_SOCKET_ERROR_ONCE: AtomicBool = AtomicBool::new(false);
    static CONTROL_SOCKET_CONNECTIONS: AtomicUsize = AtomicUsize::new(0);
    static CONTROL_SOCKET_FAILURES_REMAINING: AtomicUsize = AtomicUsize::new(0);
    static CONTROL_SOURCE_PORT: AtomicU32 = AtomicU32::new(0);

    pub(super) fn connect_control_socket() -> Option<MockSocket> {
        CONTROL_SOCKET_CONNECTIONS.fetch_add(1, Ordering::SeqCst);
        let remaining = CONTROL_SOCKET_FAILURES_REMAINING.load(Ordering::SeqCst);
        if remaining > 0 {
            CONTROL_SOCKET_FAILURES_REMAINING.fetch_sub(1, Ordering::SeqCst);
            None
        } else {
            let (sender, socket) = UnixDatagram::pair().expect("create mock control socket");
            socket
                .set_nonblocking(true)
                .expect("set mock control socket nonblocking");
            *CONTROL_SENDER.lock().unwrap() = Some(sender);
            Some(MockSocket { socket })
        }
    }

    /// Mock socket for testing purposes.
    pub struct MockSocket {
        socket: UnixDatagram,
    }

    impl AsRawFd for MockSocket {
        fn as_raw_fd(&self) -> RawFd {
            self.socket.as_raw_fd()
        }
    }

    impl MockSocket {
        pub fn recv_from_full(&mut self) -> Result<(Vec<u8>, SocketAddr), io::Error> {
            if CONTROL_SOCKET_ERROR.load(Ordering::SeqCst) {
                if CONTROL_SOCKET_ERROR_ONCE.swap(false, Ordering::SeqCst) {
                    CONTROL_SOCKET_ERROR.store(false, Ordering::SeqCst);
                }
                return Err(io::Error::new(
                    io::ErrorKind::ConnectionAborted,
                    "simulated control socket failure",
                ));
            }
            let mut message = vec![0; BUFFER_SIZE];
            let size = self.socket.recv(&mut message)?;
            message.truncate(size);
            Ok((
                message,
                SocketAddr::new(CONTROL_SOURCE_PORT.swap(0, Ordering::SeqCst), 0),
            ))
        }
    }

    fn reset_control_mock(target_family: &str, target_group: &str) {
        *TARGET_FAMILY.lock().unwrap() = Some(target_family.to_string());
        *TARGET_GROUP.lock().unwrap() = Some(target_group.to_string());
        *CONTROL_SENDER.lock().unwrap() = None;
        CONTROL_SOCKET_ERROR.store(false, Ordering::SeqCst);
        CONTROL_SOCKET_ERROR_ONCE.store(false, Ordering::SeqCst);
        CONTROL_SOCKET_CONNECTIONS.store(0, Ordering::SeqCst);
        CONTROL_SOCKET_FAILURES_REMAINING.store(0, Ordering::SeqCst);
        CONTROL_SOURCE_PORT.store(0, Ordering::SeqCst);
    }

    pub(super) fn resolved_subscription() -> Option<NetlinkSubscription> {
        let family = TARGET_FAMILY.lock().unwrap();
        let group = TARGET_GROUP.lock().unwrap();
        let (family_id, group_id) =
            data_netlink::test::resolve_mock_family(family.as_deref()?, group.as_deref()?)?;
        Some(NetlinkSubscription {
            family_id,
            group_id,
        })
    }

    fn send_control_message(message: &[u8]) {
        CONTROL_SENDER
            .lock()
            .unwrap()
            .as_ref()
            .expect("mock control socket is connected")
            .send(message)
            .expect("send mock control message");
    }

    fn control_message(command: u8, family: &str, family_id: u16) -> Vec<u8> {
        let family_name = format!("{}\0", family);
        let id_attr_len = 6usize;
        let aligned_id_attr_len = (id_attr_len + 3) & !3;
        let name_attr_len = 4 + family_name.len();
        let aligned_name_attr_len = (name_attr_len + 3) & !3;
        let message_len = NETLINK_HEADER_LEN + GENL_HDRLEN + aligned_id_attr_len
            + aligned_name_attr_len;
        let mut message = vec![0u8; message_len];

        message[0..4].copy_from_slice(&(message_len as u32).to_ne_bytes());
        message[4..6].copy_from_slice(&GENL_ID_CTRL.to_ne_bytes());
        message[16] = command;
        message[20..22].copy_from_slice(&(id_attr_len as u16).to_ne_bytes());
        message[22..24].copy_from_slice(&CTRL_ATTR_FAMILY_ID.to_ne_bytes());
        message[24..26].copy_from_slice(&family_id.to_ne_bytes());
        let name_offset = NETLINK_HEADER_LEN + GENL_HDRLEN + aligned_id_attr_len;
        message[name_offset..name_offset + 2]
            .copy_from_slice(&(name_attr_len as u16).to_ne_bytes());
        message[name_offset + 2..name_offset + 4]
            .copy_from_slice(&CTRL_ATTR_FAMILY_NAME.to_ne_bytes());
        message[name_offset + 4..name_offset + 4 + family_name.len()]
            .copy_from_slice(family_name.as_bytes());
        message
    }

    fn inject_family_deleted(family: &str, family_id: u16) {
        data_netlink::test::unregister_mock_family(family, family_id);
        send_control_message(&control_message(CTRL_CMD_DELFAMILY, family, family_id));
    }

    fn inject_family_registered(family: &str, group: &str, family_id: u16, group_id: u32) {
        data_netlink::test::register_mock_family(family, group, family_id, group_id);
        send_control_message(&control_message(CTRL_CMD_NEWFAMILY, family, family_id));
    }

    async fn wait_for_value(mut value: impl FnMut() -> usize, expected: usize, description: &str) {
        for _ in 0..1_000 {
            if value() >= expected {
                return;
            }
            tokio::task::yield_now().await;
        }
        panic!("timed out waiting for {description}");
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
        reset_control_mock(family, group);

        let (command_sender, command_receiver) = channel(10);
        let close_sender = command_sender.clone();
        let (buffer_sender, buffer_receiver) = channel(4);
        let mut data_actor = DataNetlinkActor::new(family, group, command_receiver, 0);
        data_actor.add_recipient(buffer_sender);
        let control_actor = ControlNetlinkActor::new(family, group, command_sender);

        let actors = RunningActors {
            close_sender,
            buffer_receiver,
            data_task: spawn(DataNetlinkActor::run(data_actor)),
            control_task: spawn(ControlNetlinkActor::run(control_actor)),
        };
        wait_for_value(
            data_netlink::test::live_socket_count,
            1,
            "initial data socket",
        )
        .await;
        actors
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
        reset_control_mock("test_family", "test_group");

        let (command_sender, command_receiver) = channel(10);
        let actor = ControlNetlinkActor::new("test_family", "test_group", command_sender);

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
        for _ in 0..1_000 {
            if data_netlink::test::live_socket_count() == 0 {
                break;
            }
            tokio::task::yield_now().await;
        }
        assert_eq!(data_netlink::test::live_socket_count(), 0);
        assert_eq!(data_netlink::test::current_subscription(), None);
        assert_eq!(data_netlink::test::connection_attempts(), 1);

        advance(Duration::from_secs(59)).await;
        settle_actor_tasks().await;
        assert_eq!(data_netlink::test::connection_attempts(), 1);
        assert_eq!(data_netlink::test::live_socket_count(), 0);

        advance(Duration::from_secs(1)).await;
        inject_family_registered(FAMILY, GROUP, NEW_FAMILY_ID, NEW_GROUP_ID);
        wait_for_connection_attempts(2).await;
        settle_actor_tasks().await;
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
    async fn test_idle_socket_is_not_reconnected() {
        const FAMILY: &str = "test_family";
        const GROUP: &str = "test_group";

        let actors = start_actors(FAMILY, GROUP, 0x70, 0x500).await;
        settle_actor_tasks().await;
        advance(Duration::from_secs(59)).await;
        settle_actor_tasks().await;
        assert_eq!(data_netlink::test::connection_attempts(), 1);

        advance(Duration::from_secs(1)).await;
        settle_actor_tasks().await;
        assert_eq!(data_netlink::test::connection_attempts(), 1);
        assert_eq!(data_netlink::test::live_socket_count(), 1);
        stop_actors(actors).await;
    }

    #[tokio::test(start_paused = true)]
    #[serial_test::serial]
    async fn test_control_socket_recreates_after_three_minutes_elapsed() {
        reset_control_mock("test_family", "test_group");
        CONTROL_SOCKET_ERROR.store(true, Ordering::SeqCst);
        CONTROL_SOCKET_ERROR_ONCE.store(true, Ordering::SeqCst);
        let (command_sender, command_receiver) = channel(10);
        let actor = ControlNetlinkActor::new("test_family", "test_group", command_sender);
        assert_eq!(CONTROL_SOCKET_CONNECTIONS.load(Ordering::SeqCst), 1);
        let task = spawn(ControlNetlinkActor::run(actor));
        settle_actor_tasks().await;
        send_control_message(&[0]);
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
                FamilyEvent::Registered(0x20)
            } else {
                FamilyEvent::Unregistered(0x20)
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
            vec![FamilyEvent::Unregistered(0x20), FamilyEvent::Registered(0x21)]
        );
    }

    #[test]
    fn test_valid_event_before_malformed_trailing_message_is_kept() {
        let mut datagram = control_message(CTRL_CMD_DELFAMILY, "test_family", 0x20);
        datagram.extend_from_slice(&u32::MAX.to_ne_bytes());
        datagram.resize(datagram.len() + NETLINK_HEADER_LEN - 4, 0);

        assert_eq!(
            ControlNetlinkActor::parse_control_datagram(&datagram, "test_family").unwrap(),
            vec![FamilyEvent::Unregistered(0x20)]
        );
    }

    #[tokio::test(start_paused = true)]
    #[serial_test::serial]
    async fn test_missing_control_socket_is_retried_after_three_minutes() {
        reset_control_mock("test_family", "test_group");
        CONTROL_SOCKET_FAILURES_REMAINING.store(1, Ordering::SeqCst);
        let (command_sender, command_receiver) = channel(10);
        let actor = ControlNetlinkActor::new("test_family", "test_group", command_sender);
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

    #[test]
    fn test_family_name_attribute_masks_type_flags() {
        let mut message = control_message(CTRL_CMD_NEWFAMILY, "sonic_stel", 0x20);
        let name_offset = NETLINK_HEADER_LEN + GENL_HDRLEN + 8;
        message[name_offset + 2..name_offset + 4]
            .copy_from_slice(&(CTRL_ATTR_FAMILY_NAME | 0x8000).to_ne_bytes());

        assert_eq!(
            ControlNetlinkActor::parse_control_message(&message, "sonic_stel").unwrap(),
            Some(FamilyEvent::Registered(0x20))
        );
    }

    #[test]
    fn test_malformed_family_name_is_rejected() {
        let mut message = control_message(CTRL_CMD_NEWFAMILY, "sonic_stel", 0x20);
        let name_offset = NETLINK_HEADER_LEN + GENL_HDRLEN + 8;
        let name_len =
            u16::from_ne_bytes(message[name_offset..name_offset + 2].try_into().unwrap());
        message[name_offset + name_len as usize - 1] = b'x';

        assert_eq!(
            ControlNetlinkActor::parse_control_message(&message, "sonic_stel")
                .unwrap_err()
                .kind(),
            io::ErrorKind::InvalidData
        );
    }

    #[tokio::test]
    #[serial_test::serial]
    async fn test_control_message_from_userspace_is_rejected() {
        reset_control_mock("test_family", "test_group");
        let mut socket = ControlNetlinkActor::connect_control_socket().unwrap();
        CONTROL_SOURCE_PORT.store(123, Ordering::SeqCst);
        send_control_message(&control_message(CTRL_CMD_DELFAMILY, "test_family", 0x20));

        assert_eq!(
            ControlNetlinkActor::try_recv_control(&mut socket, "test_family")
                .await
                .unwrap_err()
                .kind(),
            io::ErrorKind::PermissionDenied
        );
    }

    #[tokio::test]
    #[serial_test::serial]
    async fn test_invalid_control_datagram_does_not_delay_queued_notification() {
        const FAMILY: &str = "test_family";
        const GROUP: &str = "test_group";
        let actors = start_actors(FAMILY, GROUP, 0x20, 0x100).await;

        send_control_message(&[1, 2, 3, 4]);
        inject_family_registered(FAMILY, GROUP, 0x21, 0x101);
        wait_for_connection_attempts(2).await;

        assert_eq!(
            data_netlink::test::current_subscription(),
            Some((0x21, 0x101))
        );
        stop_actors(actors).await;
    }
}
