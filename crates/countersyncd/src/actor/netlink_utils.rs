/// Shared netlink utilities for family and multicast group resolution.
///
/// This module provides common functionality used by both ControlNetlinkActor
/// and DataNetlinkActor to avoid code duplication.

#[cfg(not(test))]
use netlink_sys::Socket;

use std::io;
#[cfg(not(test))]
use std::os::fd::AsRawFd;
use std::os::fd::RawFd;
#[cfg(not(test))]
use std::sync::atomic::{AtomicU32, Ordering};
#[cfg(not(test))]
use std::time::{Duration, Instant};

use log::debug;
#[cfg(not(test))]
use log::{info, warn};
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

/// How the receive buffer size was applied to a socket.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct RcvbufOutcome {
    /// True when SO_RCVBUFFORCE succeeded; false when SO_RCVBUF was used instead.
    pub forced: bool,
    /// Receive buffer size reported by the kernel after the update.
    pub granted: usize,
}

fn set_int_sockopt(fd: RawFd, option: libc::c_int, value: libc::c_int) -> io::Result<()> {
    // SAFETY: `value` outlives the call and the length matches its type.
    let result = unsafe {
        libc::setsockopt(
            fd,
            libc::SOL_SOCKET,
            option,
            (&value as *const libc::c_int).cast(),
            std::mem::size_of::<libc::c_int>() as libc::socklen_t,
        )
    };
    if result < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(())
    }
}

fn get_rcvbuf(fd: RawFd) -> io::Result<usize> {
    let mut value: libc::c_int = 0;
    let mut len = std::mem::size_of::<libc::c_int>() as libc::socklen_t;
    // SAFETY: `value` and `len` are valid for writes and `len` matches `value`.
    let result = unsafe {
        libc::getsockopt(
            fd,
            libc::SOL_SOCKET,
            libc::SO_RCVBUF,
            (&mut value as *mut libc::c_int).cast(),
            &mut len,
        )
    };
    if result < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(value.max(0) as usize)
    }
}

/// Applies `bytes` as the socket receive buffer size.
///
/// SO_RCVBUFFORCE is tried first because SO_RCVBUF is silently capped at
/// net.core.rmem_max. SO_RCVBUFFORCE needs CAP_NET_ADMIN; without it the call
/// falls back to SO_RCVBUF. Linux doubles the requested value internally.
pub(crate) fn apply_socket_rcvbuf(fd: RawFd, bytes: usize) -> io::Result<RcvbufOutcome> {
    let value = libc::c_int::try_from(bytes).unwrap_or(libc::c_int::MAX);
    let forced = match set_int_sockopt(fd, libc::SO_RCVBUFFORCE, value) {
        Ok(()) => true,
        Err(force_err) => {
            debug!("SO_RCVBUFFORCE({bytes}) failed ({force_err}); falling back to SO_RCVBUF");
            set_int_sockopt(fd, libc::SO_RCVBUF, value)?;
            false
        }
    };
    Ok(RcvbufOutcome {
        forced,
        granted: get_rcvbuf(fd)?,
    })
}

/// Sets the receive buffer of a netlink socket to absorb HFT bursts.
///
/// HFT data arrives at a fixed rate. When countersyncd is briefly starved of
/// CPU, messages queue in this buffer; once it is full the kernel drops them
/// (ENOBUFS). Logs the size granted by the kernel.
#[cfg(not(test))]
pub fn set_socket_rcvbuf(socket: &Socket, bytes: usize) {
    if bytes == 0 {
        return;
    }
    if bytes > libc::c_int::MAX as usize {
        warn!(
            "netlink_rcvbuf {} exceeds c_int::MAX, clamping to {}",
            bytes,
            libc::c_int::MAX
        );
    }
    match apply_socket_rcvbuf(socket.as_raw_fd(), bytes) {
        Ok(RcvbufOutcome { forced, granted }) => {
            let method = if forced {
                "SO_RCVBUFFORCE"
            } else {
                "SO_RCVBUF"
            };
            // Linux stores twice the requested value (see socket(7)).
            let expected = bytes.min(libc::c_int::MAX as usize / 2) * 2;
            if granted < expected {
                warn!(
                    "Netlink receive buffer: requested={} bytes via {}, kernel granted only {} of {} bytes \
                     (capped by net.core.rmem_max; raise it or grant CAP_NET_ADMIN)",
                    bytes, method, granted, expected
                );
            } else {
                info!(
                    "Netlink receive buffer: requested={} bytes via {}, kernel granted={} bytes",
                    bytes, method, granted
                );
            }
        }
        Err(e) => warn!("Failed to set netlink receive buffer to {}: {:?}", bytes, e),
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
/// The socket is configured in blocking mode for request-response operations. Its receive timeout
/// bounds the receive phase if the kernel response is lost; `receive_matching_response` shortens
/// that timeout against one overall deadline when it skips unrelated responses.
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

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::UdpSocket;
    use std::os::fd::AsRawFd;

    #[test]
    fn test_apply_socket_rcvbuf_grants_requested_size() {
        // A UDP socket shares the SOL_SOCKET receive-buffer code path with netlink.
        let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
        let requested = 1 << 20;
        let outcome = apply_socket_rcvbuf(socket.as_raw_fd(), requested).unwrap();
        assert_eq!(outcome.granted, get_rcvbuf(socket.as_raw_fd()).unwrap());
        if outcome.forced {
            // SO_RCVBUFFORCE bypasses net.core.rmem_max; Linux doubles the value.
            assert!(outcome.granted >= requested * 2, "{outcome:?}");
        } else {
            assert!(outcome.granted > 0, "{outcome:?}");
        }
    }

    #[test]
    fn test_apply_socket_rcvbuf_clamps_oversized_request() {
        let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
        let outcome = apply_socket_rcvbuf(socket.as_raw_fd(), usize::MAX).unwrap();
        assert!(outcome.granted > 0, "{outcome:?}");
    }

    #[test]
    fn test_apply_socket_rcvbuf_reports_invalid_fd() {
        assert!(apply_socket_rcvbuf(-1, 1 << 20).is_err());
    }
}
