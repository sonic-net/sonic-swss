use std::{
    collections::LinkedList,
    io,
    sync::Arc,
    thread::sleep,
    time::Duration,
    vec,
    fs::File, io::prelude::*
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
const RECONNECT_MAX_ATTEMPTS: u64 = 5;
const BUFFER_SIZE: usize = 0xFFFF;

pub struct NetlinkActor {
    family: String,
    group: String,
    socket: Option<SocketType>,
    buffer_recipients: LinkedList<Sender<SocketBufferMessage>>,
    command_recipient: Receiver<NetlinkCommand>,
}

impl NetlinkActor {
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

    pub fn add_recipient(&mut self, recipient: Sender<SocketBufferMessage>) {
        self.buffer_recipients.push_back(recipient);
    }

    #[cfg(not(test))]
    fn connect(family: &str, group: &str) -> Option<SocketType> {
        let (sock, _) = NlRouter::connect(
            NlFamily::Generic,
            // 0 is pid of kernel -> socket is connected to kernel
            Some(0),
            Groups::empty(),
        )
        .unwrap();

        let group_id = sock.resolve_nl_mcast_group(family, group);
        if group_id.is_err() {
            warn!("Failed to resolve group id");
            return None;
        }

        let socket = SocketType::connect(
            NlFamily::Generic,
            // 0 is pid of kernel -> socket is connected to kernel
            Some(0),
            Groups::empty(),
        )
        .unwrap();

        match socket.add_mcast_membership(Groups::new_groups(&[group_id.unwrap()])) {
            Ok(_) => Some(socket),
            Err(e) => {
                warn!("Failed to add mcast membership: {:?}", e);
                None
            }
        }
    }

    #[cfg(test)]
    fn connect(_: &str, _: &str) -> Option<SocketType> {
        let sock = SocketType::new();
        match sock.valid {
            false => None,
            true => Some(sock),
        }
    }

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

    fn reset(&mut self, family: &str, group: &str) {
        self.family = family.to_string();
        self.group = group.to_string();
        self.reconnect();
    }

    async fn try_recv(socket: Option<&mut SocketType>) -> Result<SocketBufferMessage, io::Error> {
        match socket {
            None => {
                sleep(Duration::from_millis(RECONNECT_INTERVAL_MS));
                Err(io::Error::new(io::ErrorKind::Other, "No socket"))
            }
            Some(socket) => {
                let mut buffer = Arc::new(vec![0; BUFFER_SIZE]);
                match socket.recv(Arc::get_mut(&mut buffer).unwrap(), Msg::empty()) {
                    Err(e) => {
                        Err(e)
                    }
                    Ok(size) => {
                        if size.0 == 0 {
                            return Err(io::Error::new(
                                io::ErrorKind::Other,
                                "No more data to receive",
                            ));
                        }
                        Arc::get_mut(&mut buffer).unwrap().resize(size.0, 0);
                        Ok(buffer)
                    }
                }
            }
        }
    }

    pub async fn run(mut actor: NetlinkActor) {
        loop {
            select! {
                ret = NetlinkActor::try_recv(actor.socket.as_mut()) => {
                    match ret {
                        Ok(buffer) => {
                            for recipient in &actor.buffer_recipients {
                                recipient.send(buffer.clone()).await.unwrap();
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

#[cfg(test)]
const SONIC_CONSTANTS: &str = "tests/data/constants.yml";
#[cfg(not(test))]
const SONIC_CONSTANTS: &str = "/etc/sonic/constants.yml";

pub fn get_genl_family_group() -> (String, String) {
    let mut fd = File::open(SONIC_CONSTANTS).unwrap();
    let mut yaml_str = String::new();
    fd.read_to_string(&mut yaml_str).unwrap();
    let constants = &YamlLoader::load_from_str(&yaml_str).unwrap()[0];

    let stream_telemetry = &constants["constants"]["stream_telemetry"];
    (
        stream_telemetry["genl_family"].as_str().unwrap().to_string(),
        stream_telemetry["genl_multicast_group"].as_str().unwrap().to_string()
    )
}

#[cfg(test)]
mod test {
    use super::*;

    use tokio::{spawn, sync::mpsc::channel};

    const PARTIALLY_VALID_MESSAGES: [&str; 4] = [
        "PARTIALLY_VALID1",
        "PARTIALLY_VALID2",
        "", // Empty String needs to simulate the reconnect
        "PARTIALLY_VALID3",
    ];

    const VALID_MESSAGES: [&str; 2] = ["VALID1", "VALID2"];

    static mut SOCKET_COUNT: usize = 0;

    pub struct MockSocket {
        pub valid: bool,
        budget: usize,
        messages: Vec<String>,
    }

    impl MockSocket {
        pub fn new() -> Self {
            unsafe {
                SOCKET_COUNT += 1;
                if SOCKET_COUNT == 1 {
                    return MockSocket {
                        valid: true,
                        budget: PARTIALLY_VALID_MESSAGES.len(),
                        messages: PARTIALLY_VALID_MESSAGES
                            .iter()
                            .map(|s| s.to_string())
                            .collect(),
                    };
                } else {
                    return MockSocket {
                        valid: SOCKET_COUNT <= 2, // 2 is the maximum number of sockets
                        budget: VALID_MESSAGES.len(),
                        messages: VALID_MESSAGES.iter().map(|s| s.to_string()).collect(),
                    };
                }
            }
        }

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
                Err(io::Error::new(io::ErrorKind::Other, "Error Message"))
            }
        }
    }

    #[tokio::test]
    async fn test_netlink() {
        let (_command_sender, command_reciver) = channel(1);
        let (buffer_sender, mut buffer_reciver) = channel(1);
        let mut actor = NetlinkActor::new("family", "group", command_reciver);

        actor.add_recipient(buffer_sender);
        let task = spawn(NetlinkActor::run(actor));

        let mut recv_messages = Vec::new();
        for _ in 0..4 {
            let buffer = buffer_reciver.recv().await.unwrap();
            recv_messages.push(String::from_utf8(buffer.to_vec()).unwrap());
        }

        let mut expect_messages = Vec::new();
        for msg in PARTIALLY_VALID_MESSAGES.iter() {
            if msg.is_empty() {
                break;
            }
            expect_messages.push(msg.to_string());
        }
        for msg in VALID_MESSAGES.iter() {
            expect_messages.push(msg.to_string());
        }

        assert_eq!(recv_messages, expect_messages);
        assert!(unsafe { SOCKET_COUNT } > 1);

        task.await.unwrap();
    }

    #[test]
    fn test_family_group_pase() {
        let (family, group) = get_genl_family_group();
        assert_eq!(family, "sonic_stel");
        assert_eq!(group, "ipfix");
    }
}
