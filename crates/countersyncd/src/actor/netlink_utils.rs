/// Shared netlink utilities for family and multicast group resolution.
///
/// This module provides common functionality used by both ControlNetlinkActor
/// and DataNetlinkActor to avoid code duplication.

use netlink_sys::Socket;

#[cfg(not(test))]
use std::io;
#[cfg(not(test))]
use std::os::fd::AsRawFd;

#[cfg(not(test))]
use log::{debug, warn};
#[cfg(not(test))]
use netlink_packet_core::{NetlinkMessage, NetlinkPayload, NLM_F_ACK, NLM_F_REQUEST};
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

/// Creates a netlink socket for family/group resolution.
///
/// The socket is configured in blocking mode for request-response operations.
///
/// # Returns
///
/// Some(socket) if creation is successful, None otherwise
#[cfg(not(test))]
pub fn create_nl_resolver() -> Option<Socket> {
    match Socket::new(NETLINK_GENERIC) {
        Ok(mut socket) => {
            let addr = SocketAddr::new(0, 0);
            if let Err(e) = socket.bind(&addr) {
                warn!("Failed to bind resolver socket: {:?}", e);
                return None;
            }
            // Set to blocking mode for request-response operations
            if let Err(e) = socket.set_non_blocking(false) {
                warn!("Failed to set resolver socket to blocking mode: {:?}", e);
                return None;
            }
            debug!("Created netlink socket for family/group resolution (blocking mode)");
            Some(socket)
        }
        Err(e) => {
            warn!("Failed to create netlink socket: {:?}", e);
            None
        }
    }
}

/// Mock netlink resolver for testing.
#[cfg(test)]
#[allow(dead_code)]
pub fn create_nl_resolver() -> Option<Socket> {
    None
}

/// Resolves a generic netlink family name to its ID.
///
/// # Arguments
///
/// * `socket` - The netlink socket to use for resolution
/// * `family_name` - The name of the generic netlink family
///
/// # Returns
///
/// Ok(family_id) if successful, Err otherwise
#[cfg(not(test))]
pub fn resolve_family_id(socket: &mut Socket, family_name: &str) -> Result<u16, io::Error> {
    debug!(
        "resolve_family_id: Starting resolution for family '{}'",
        family_name
    );

    // Drain any pending data from socket before sending request
    // This prevents reading stale responses
    let mut drain_buf = vec![0; 8192];
    loop {
        match socket.recv_from(&mut drain_buf, libc::MSG_DONTWAIT) {
            Ok((n, _addr)) if n > 0 => {
                debug!(
                    "resolve_family_id: Drained {} stale bytes from socket",
                    n
                );
                continue;
            }
            _ => break,
        }
    }

    // Create a GET_FAMILY request
    let mut genlmsg: GenlMessage<GenlCtrl> = GenlMessage::from_payload(GenlCtrl {
        cmd: GenlCtrlCmd::GetFamily,
        nlas: vec![GenlCtrlAttrs::FamilyName(family_name.to_owned())],
    });
    genlmsg.finalize();

    let mut nlmsg = NetlinkMessage::from(genlmsg);
    nlmsg.header.flags = NLM_F_REQUEST | NLM_F_ACK; // Request with ACK
    nlmsg.header.sequence_number = 1; // Set sequence number
    nlmsg.finalize();

    // Send the request
    let mut buf = vec![0; nlmsg.buffer_len()];
    nlmsg.serialize(&mut buf[..]);
    debug!(
        "resolve_family_id: Sending request of {} bytes (seq={})",
        buf.len(),
        nlmsg.header.sequence_number
    );

    // Debug: print request hex
    let req_preview = buf.len().min(36);
    let hex_str: Vec<String> = buf[..req_preview]
        .iter()
        .map(|b| format!("{:02x}", b))
        .collect();
    debug!("resolve_family_id: Request hex: {}", hex_str.join(" "));

    let kernel_addr = SocketAddr::new(0, 0);
    let sent_bytes = socket.send_to(&buf[..], &kernel_addr, 0)?;
    debug!(
        "resolve_family_id: Sent {} bytes to kernel",
        sent_bytes
    );

    // Receive response using raw recv syscall to avoid netlink-sys buffer issues
    let mut response_buf = vec![0u8; 8192];
    debug!("resolve_family_id: Calling raw recv() to receive response...");

    let fd = socket.as_raw_fd();
    let bytes_read = unsafe {
        libc::recv(
            fd,
            response_buf.as_mut_ptr() as *mut libc::c_void,
            response_buf.len(),
            0,
        )
    };

    if bytes_read < 0 {
        return Err(io::Error::last_os_error());
    }
    let bytes_read = bytes_read as usize;
    debug!(
        "resolve_family_id: Received {} bytes from kernel",
        bytes_read
    );

    if bytes_read == 0 {
        return Err(io::Error::new(
            io::ErrorKind::UnexpectedEof,
            "No response received from kernel",
        ));
    }

    // Debug: print first 36 bytes of response in hex
    let resp_preview = bytes_read.min(36);
    let resp_hex: Vec<String> = response_buf[..resp_preview]
        .iter()
        .map(|b| format!("{:02x}", b))
        .collect();
    debug!("resolve_family_id: Response hex: {}", resp_hex.join(" "));

    // Debug: check netlink header
    if bytes_read >= 16 {
        let nl_len = u32::from_le_bytes([
            response_buf[0],
            response_buf[1],
            response_buf[2],
            response_buf[3],
        ]);
        let nl_type = u16::from_le_bytes([response_buf[4], response_buf[5]]);
        let nl_flags = u16::from_le_bytes([response_buf[6], response_buf[7]]);
        let nl_seq = u32::from_le_bytes([
            response_buf[8],
            response_buf[9],
            response_buf[10],
            response_buf[11],
        ]);
        let nl_pid = u32::from_le_bytes([
            response_buf[12],
            response_buf[13],
            response_buf[14],
            response_buf[15],
        ]);
        debug!(
            "resolve_family_id: Netlink header - len:{} type:{} flags:{} seq:{} pid:{}",
            nl_len, nl_type, nl_flags, nl_seq, nl_pid
        );
    }

    // Parse the response
    let response = NetlinkMessage::<GenlMessage<GenlCtrl>>::deserialize(&response_buf[..bytes_read])
        .map_err(|e| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Failed to parse response: {:?}", e),
            )
        })?;

    match response.payload {
        NetlinkPayload::InnerMessage(genlmsg) => {
            for nla in &genlmsg.payload.nlas {
                if let GenlCtrlAttrs::FamilyId(id) = nla {
                    return Ok(*id);
                }
            }
            Err(io::Error::new(
                io::ErrorKind::NotFound,
                "Family ID not found in response",
            ))
        }
        NetlinkPayload::Error(err) => Err(io::Error::new(
            io::ErrorKind::Other,
            format!("Netlink error: {:?}", err),
        )),
        _ => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Unexpected response type",
        )),
    }
}

/// Resolves a generic netlink multicast group to its ID.
///
/// # Arguments
///
/// * `socket` - The netlink socket to use for resolution
/// * `family_name` - The name of the generic netlink family
/// * `group_name` - The name of the multicast group
///
/// # Returns
///
/// Ok(group_id) if successful, Err otherwise
#[cfg(not(test))]
pub fn resolve_multicast_group(
    socket: &mut Socket,
    family_name: &str,
    group_name: &str,
) -> Result<u32, io::Error> {
    debug!(
        "resolve_multicast_group: Starting for family '{}', group '{}'",
        family_name, group_name
    );

    // Drain any pending data from socket before sending request
    let mut drain_buf = vec![0; 8192];
    loop {
        match socket.recv_from(&mut drain_buf, libc::MSG_DONTWAIT) {
            Ok((n, _addr)) if n > 0 => {
                debug!(
                    "resolve_multicast_group: Drained {} stale bytes from socket",
                    n
                );
                continue;
            }
            _ => break,
        }
    }

    // Create a GET_FAMILY request (this will include multicast group info in response)
    let mut genlmsg: GenlMessage<GenlCtrl> = GenlMessage::from_payload(GenlCtrl {
        cmd: GenlCtrlCmd::GetFamily,
        nlas: vec![GenlCtrlAttrs::FamilyName(family_name.to_owned())],
    });
    genlmsg.finalize();

    let mut nlmsg = NetlinkMessage::from(genlmsg);
    nlmsg.header.flags = NLM_F_REQUEST | NLM_F_ACK;
    nlmsg.header.sequence_number = 1;
    nlmsg.finalize();

    // Send the request
    let mut buf = vec![0; nlmsg.buffer_len()];
    nlmsg.serialize(&mut buf[..]);
    debug!("resolve_multicast_group: Sending {} bytes", buf.len());

    let kernel_addr = SocketAddr::new(0, 0);
    socket.send_to(&buf[..], &kernel_addr, 0)?;

    // Receive response using raw recv syscall to avoid netlink-sys buffer issues
    let mut response_buf = vec![0u8; 8192];

    let fd = socket.as_raw_fd();
    let bytes_read = unsafe {
        libc::recv(
            fd,
            response_buf.as_mut_ptr() as *mut libc::c_void,
            response_buf.len(),
            0,
        )
    };

    if bytes_read < 0 {
        return Err(io::Error::last_os_error());
    }
    let bytes_read = bytes_read as usize;
    debug!("resolve_multicast_group: Received {} bytes", bytes_read);

    if bytes_read == 0 {
        return Err(io::Error::new(
            io::ErrorKind::UnexpectedEof,
            "No response received from kernel",
        ));
    }

    // Parse the response
    let response = NetlinkMessage::<GenlMessage<GenlCtrl>>::deserialize(&response_buf[..bytes_read])
        .map_err(|e| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                format!("Failed to parse response: {:?}", e),
            )
        })?;

    match response.payload {
        NetlinkPayload::InnerMessage(genlmsg) => {
            debug!(
                "resolve_multicast_group: Parsing {} attributes",
                genlmsg.payload.nlas.len()
            );
            // Look for multicast groups
            for nla in &genlmsg.payload.nlas {
                if let GenlCtrlAttrs::McastGroups(groups) = nla {
                    debug!(
                        "resolve_multicast_group: Found {} multicast groups",
                        groups.len()
                    );
                    for group_nlas in groups {
                        let mut found_name = None;
                        let mut found_id = None;

                        for group_attr in group_nlas {
                            match group_attr {
                                McastGrpAttrs::Name(name) => {
                                    debug!(
                                        "resolve_multicast_group: Found group name '{}'",
                                        name
                                    );
                                    if name == group_name {
                                        found_name = Some(name.clone());
                                    }
                                }
                                McastGrpAttrs::Id(id) => {
                                    debug!("resolve_multicast_group: Found group id {}", id);
                                    found_id = Some(*id);
                                }
                            }
                        }

                        if found_name.is_some() && found_id.is_some() {
                            let group_id = found_id.unwrap();
                            debug!(
                                "resolve_multicast_group: Successfully resolved '{}' to ID {}",
                                group_name, group_id
                            );
                            return Ok(group_id);
                        }
                    }
                }
            }
            Err(io::Error::new(
                io::ErrorKind::NotFound,
                format!("Multicast group '{}' not found", group_name),
            ))
        }
        NetlinkPayload::Error(err) => Err(io::Error::new(
            io::ErrorKind::Other,
            format!("Netlink error: {:?}", err),
        )),
        _ => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Unexpected response type",
        )),
    }
}
