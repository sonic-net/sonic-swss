#[path = "../../../src/message/otel.rs"]
pub mod otel_message;
#[path = "../../../src/sai/mod.rs"]
pub mod sai;
#[path = "../../../src/message/saistats.rs"]
pub mod saistats;
pub mod message {
    pub use crate::otel_message as otel;
    pub use crate::saistats;
    pub use crate::{buffer, ipfix_message as ipfix};
}
#[path = "../../../src/message/buffer.rs"]
pub mod buffer;
#[path = "../../../src/message/ipfix.rs"]
pub mod ipfix_message;
#[path = "../../../src/actor/otel.rs"]
pub mod otel_actor;
#[path = "../../../src/utilities/mod.rs"]
pub mod utilities;
pub mod actor {
    pub use crate::otel_actor as otel;
    pub use crate::real_actors::{ipfix, stats_reporter};
}
pub mod real_actors {
    pub use crate::message;
    pub mod ipfix {
        include!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../src/actor/ipfix.rs"
        ));
    }
    pub mod stats_reporter {
        include!(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../src/actor/stats_reporter.rs"
        ));
    }
}
