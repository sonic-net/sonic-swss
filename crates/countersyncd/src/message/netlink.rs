#[derive(Debug)]
pub struct SocketConnect {
    pub family: String,
    pub group: String,
}

#[allow(dead_code)]
#[derive(Debug)]
pub enum NetlinkCommand {
    Close,
    Reconnect,
    SocketConnect(SocketConnect),
}
