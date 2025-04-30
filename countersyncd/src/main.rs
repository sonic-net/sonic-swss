mod message;
mod actor;

use tokio::{spawn, sync::mpsc::channel};

use actor::{netlink::{NetlinkActor, get_genl_family_group}, ipfix::IpfixActor};

#[tokio::main]
async fn main() {
    let (_command_sender, command_receiver) = channel(1);
    let (socket_sender, socket_receiver) = channel(1);
    let (_ipfix_template_sender, ipfix_template_receiver) = channel(1);
    let (saistats_sender, _saistats_receiver) = channel(1);

    let (family, group) = get_genl_family_group();

    let mut netlink = NetlinkActor::new(family.as_str(), group.as_str(), command_receiver);
    netlink.add_recipient(socket_sender);
    let mut ipfix = IpfixActor::new(ipfix_template_receiver, socket_receiver);
    ipfix.add_recipient(saistats_sender);

    let netlink_handle = spawn(NetlinkActor::run(netlink));
    let ipfix_handle = spawn(IpfixActor::run(ipfix));

    netlink_handle.await.unwrap();
    ipfix_handle.await.unwrap();
}
