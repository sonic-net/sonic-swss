/// SAI (Switch Abstraction Interface) type definitions
/// 
/// This module contains Rust definitions for SAI enums that correspond to C header files.
/// All enums support efficient bidirectional conversion between integers and strings.

pub mod saitypes;
pub mod saiport;
pub mod saibuffer;
pub mod saiqueue;

// Re-export commonly used types
pub use saitypes::SaiObjectType;
pub use saiport::SaiPortStat;
pub use saibuffer::{SaiBufferPoolStat, SaiIngressPriorityGroupStat};
pub use saiqueue::SaiQueueStat;
