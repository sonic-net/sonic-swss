#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct NetlinkSubscription {
    pub family_id: u16,
    pub group_id: u32,
}

#[derive(Debug)]
pub enum NetlinkCommand {
    /// Used by integration tests and the privileged state-test example for orderly shutdown.
    #[allow(dead_code)]
    Close,
    Connect(NetlinkSubscription),
    Reconnect(NetlinkSubscription),
    Disconnect,
}
