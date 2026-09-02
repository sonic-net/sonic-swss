/// Shared netlink utilities for family and multicast group resolution.
///
/// This module provides common functionality used by both ControlNetlinkActor
/// and DataNetlinkActor to avoid code duplication.

#[cfg(not(test))]
use netlink_sys::Socket;

#[cfg(not(test))]
use std::io;
#[cfg(not(test))]
use std::os::fd::AsRawFd;
#[cfg(not(test))]
use std::sync::atomic::{AtomicU32, Ordering};
#[cfg(not(test))]
use std::time::{Duration, Instant};

#[cfg(not(test))]
use log::{debug, info, warn};
#[cfg(not(test))]
use netlink_packet_core::{NetlinkMessage, NetlinkPayload, NLM_F_REQUEST};
#[cfg(not(test))]
use netlink_packet_generic::{
    ctrl::{
        nlas::{GenlCtrlAttrs, McastGrpAttrs},
        GenlCtrl, GenlCtrlCmd,
    },
    GenlMessage,
};
#[cfg(not(test))]
use netlink_sys::{protocols::NETLINK_GENERIC, SocketAddr};

#[cfg(not(test))]
use crate::message::netlink::NetlinkSubscription;

#[cfg(not(test))]
const RESOLVER_TIMEOUT: Duration = Duration::from_millis(500);
#[cfg(not(test))]
const MAX_RESOLVER_RESPONSE_SIZE: usize = 1024 * 1024;
#[cfg(not(test))]
static NEXT_SEQUENCE: AtomicU32 = AtomicU32::new(1);

/// Sets SO_RCVBUF on a netlink socket to reduce ENOBUFS under high HFT load.
///
/// Logs the actual buffer size granted by the kernel after setting, since Linux
/// may cap it at net.core.rmem_max and doubles it internally.
#[cfg(not(test))]
pub fn set_socket_rcvbuf(socket: &Socket, bytes: usize) {
    if bytes == 0 {
        return;
    }
    let v: libc::c_int = match bytes.try_into() {
        Ok(v) => v,
        Err(_) => {
            warn!(
                "netlink_rcvbuf {} exceeds c_int::MAX, clamping to {}",
                bytes,
                libc::c_int::MAX
            );
            libc::c_int::MAX
        }
    };
    if let Err(e) = socket.set_rx_buf_sz(v) {
        warn!("Failed to set netlink SO_RCVBUF to {}: {:?}", bytes, e);
        return;
    }
    match socket.get_rx_buf_sz() {
        Ok(actual) => info!(
            "Netlink SO_RCVBUF: requested={} bytes, actual={} bytes{}",
            bytes,
            actual,
            if actual < bytes { " (capped by net.core.rmem_max — consider raising it)" } else { "" }
        ),
        Err(e) => warn!("Failed to read back SO_RCVBUF: {:?}", e),
    }
}

#[cfg(not(test))]
pub(crate) fn set_socket_recv_timeout(socket: &Socket, timeout: Duration) -> io::Result<()> {
    let timeout = if timeout.is_zero() {
        return Err(io::Error::new(
            io::ErrorKind::TimedOut,
            "resolver receive deadline elapsed",
        ));
    } else if timeout < Duration::from_micros(1) {
        Duration::from_micros(1)
    } else {
        timeout
    };
    let timeout = libc::timeval {
        tv_sec: timeout.as_secs().try_into().map_err(|_| {
            io::Error::new(io::ErrorKind::InvalidInput, "socket timeout is too large")
        })?,
        tv_usec: timeout.subsec_micros().try_into().map_err(|_| {
            io::Error::new(io::ErrorKind::InvalidInput, "socket timeout is too precise")
        })?,
    };
    let result = unsafe {
        libc::setsockopt(
            socket.as_raw_fd(),
            libc::SOL_SOCKET,
            libc::SO_RCVTIMEO,
            (&timeout as *const libc::timeval).cast(),
            std::mem::size_of::<libc::timeval>() as libc::socklen_t,
        )
    };
    if result < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

/// Creates a netlink socket for family/group resolution.
///
/// The socket is configured in blocking mode for request-response operations.
///
/// # Returns
///
/// A configured socket, or the socket setup error.
#[cfg(not(test))]
pub fn create_nl_resolver() -> io::Result<Socket> {
    let mut socket = Socket::new(NETLINK_GENERIC)?;
    socket.bind(&SocketAddr::new(0, 0))?;
    socket.set_non_blocking(false)?;
    set_socket_recv_timeout(&socket, RESOLVER_TIMEOUT)?;
    debug!("Created netlink socket for family/group resolution (blocking mode)");
    Ok(socket)
}

#[cfg(not(test))]
fn next_sequence() -> u32 {
    loop {
        let sequence = NEXT_SEQUENCE.fetch_add(1, Ordering::Relaxed);
        if sequence != 0 {
            return sequence;
        }
    }
}

#[cfg(not(test))]
fn receive_matching_response(
    socket: &Socket,
    sequence: u32,
) -> Result<NetlinkMessage<GenlMessage<GenlCtrl>>, io::Error> {
    let deadline = Instant::now() + RESOLVER_TIMEOUT;

    loop {
        let now = Instant::now();
        if now >= deadline {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                format!("Timed out waiting for netlink response sequence {sequence}"),
            ));
        }
        set_socket_recv_timeout(socket, deadline.duration_since(now))?;

        let (buffer, source) = match socket.recv_from_full() {
            Ok(response) => response,
            Err(error)
                if matches!(
                    error.kind(),
                    io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut
                ) =>
            {
                continue;
            }
            Err(error) => return Err(error),
        };
        if source.port_number() != 0 {
            debug!(
                "Ignoring netlink response sequence {} from userspace port {}",
                sequence,
                source.port_number()
            );
            continue;
        }
        if buffer.len() > MAX_RESOLVER_RESPONSE_SIZE {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!(
                    "Netlink response is {} bytes, maximum supported is {}",
                    buffer.len(),
                    MAX_RESOLVER_RESPONSE_SIZE
                ),
            ));
        }

        let response = NetlinkMessage::<GenlMessage<GenlCtrl>>::deserialize(&buffer).map_err(
            |error| {
                io::Error::new(
                    io::ErrorKind::InvalidData,
                    format!("Failed to parse netlink response: {error:?}"),
                )
            },
        )?;
        if response.header.sequence_number != sequence {
            debug!(
                "Ignoring stale netlink response sequence {}, expected {}",
                response.header.sequence_number, sequence
            );
            continue;
        }
        return Ok(response);
    }
}

#[cfg(not(test))]
fn query_family(
    socket: &mut Socket,
    family_name: &str,
) -> Result<GenlMessage<GenlCtrl>, io::Error> {
    let sequence = next_sequence();
    let mut genlmsg: GenlMessage<GenlCtrl> = GenlMessage::from_payload(GenlCtrl {
        cmd: GenlCtrlCmd::GetFamily,
        nlas: vec![GenlCtrlAttrs::FamilyName(family_name.to_owned())],
    });
    genlmsg.finalize();

    let mut request = NetlinkMessage::from(genlmsg);
    request.header.flags = NLM_F_REQUEST;
    request.header.sequence_number = sequence;
    request.finalize();

    let mut buffer = vec![0; request.buffer_len()];
    request.serialize(&mut buffer);
    socket.send_to(&buffer, &SocketAddr::new(0, 0), 0)?;

    match receive_matching_response(socket, sequence)?.payload {
        NetlinkPayload::InnerMessage(message) => Ok(message),
        NetlinkPayload::Error(error) => Err(error.into()),
        payload => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("Unexpected netlink response payload: {payload:?}"),
        )),
    }
}

/// Resolves a family and one of its multicast groups in a single correlated request.
#[cfg(not(test))]
pub fn resolve_family_group(
    socket: &mut Socket,
    family_name: &str,
    group_name: &str,
) -> Result<NetlinkSubscription, io::Error> {
    let response = query_family(socket, family_name)?;
    let mut family_id = None;
    let mut group_id = None;

    for attribute in response.payload.nlas {
        match attribute {
            GenlCtrlAttrs::FamilyId(id) => family_id = Some(id),
            GenlCtrlAttrs::McastGroups(groups) => {
                for group in groups {
                    let mut name = None;
                    let mut id = None;
                    for attribute in group {
                        match attribute {
                            McastGrpAttrs::Name(value) => name = Some(value),
                            McastGrpAttrs::Id(value) => id = Some(value),
                        }
                    }
                    if name.as_deref() == Some(group_name) {
                        group_id = id;
                    }
                }
            }
            _ => {}
        }
    }

    Ok(NetlinkSubscription {
        family_id: family_id
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "Family ID missing"))?,
        group_id: group_id.ok_or_else(|| {
            io::Error::new(
                io::ErrorKind::NotFound,
                format!("Multicast group '{group_name}' not found in family '{family_name}'"),
            )
        })?,
    })
}
