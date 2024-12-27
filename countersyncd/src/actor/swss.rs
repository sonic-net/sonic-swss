use swss_common::{SubscriberStateTable, DbConnector};
use super::super::message::ipfix::IPFixTemplates;

use tokio::sync::mpsc::Sender;

const SOCK_PATH: &str = "/var/run/redis/redis.sock";
const STATE_DB_ID: i32 = 6;
const STATE_STREAM_TELEMETRY_SESSION_TABLE: &str = "STREAM_TELEMETRY_SESSION";

pub struct SwssActor {
    pub session_table: SubscriberStateTable,
    template_recipient: Sender<IPFixTemplates>,
}

impl SwssActor {
    pub fn new(template_recipient: Sender<IPFixTemplates>) -> Self {
        let connect = DbConnector::new_unix(STATE_DB_ID, SOCK_PATH, 0);
        let session_table = SubscriberStateTable::new(connect, STATE_STREAM_TELEMETRY_SESSION_TABLE, None, None);
        SwssActor {
            session_table,
            template_recipient,
        }
    }

    pub async fn run(mut actor: SwssActor) {
        loop {
            let items = actor.session_table.pops();
            println!("SwssActor: {:?}", items);
        }
    }
}

// #[cfg(test)]
// mod test {
//     use super::*;

//     use tokio::{spawn, sync::mpsc::channel};

//     #[tokio::test]
//     async fn test_swss() {
//         let (template_sender, template_recipient) = channel(1);
//         let swss_actor = SwssActor::new(template_sender);
//         SwssActor::run(swss_actor).await;
//     }
// }
