pub mod saibuffer;
pub mod saiport;
pub mod saiqueue;
/// SAI (Switch Abstraction Interface) type definitions
///
/// This module contains Rust definitions for SAI enums that correspond to C header files.
/// All enums support efficient bidirectional conversion between integers and strings.
pub mod saitypes;

// Re-export commonly used types
pub use saibuffer::{SaiBufferPoolStat, SaiIngressPriorityGroupStat};
pub use saiport::SaiPortStat;
pub use saiqueue::SaiQueueStat;
pub use saitypes::SaiObjectType;
