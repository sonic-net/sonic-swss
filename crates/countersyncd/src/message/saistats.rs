//! SAI (Switch Abstraction Interface) Statistics Message Types
//!
//! This module defines the data structures for representing SAI statistics
//! extracted from IPFIX data records. SAI statistics contain information
//! about switch hardware counters and performance metrics.

use std::sync::Arc;

use byteorder::{ByteOrder, NetworkEndian};
use ipfixrw::parser::{DataRecordValue, FieldSpecifier};

/// Represents a single SAI statistic entry containing counter information.
///
/// SAI statistics are extracted from IPFIX data records and contain
/// information about switch hardware counters and their current values.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct SAIStat {
    /// Object name corresponding to the label ID (1-based index from object_names)
    pub object_name: String,
    /// SAI object type identifier (with possible extensions)
    pub type_id: u32,
    /// SAI statistic identifier (with possible extensions)
    pub stat_id: u32,
    /// Current counter value
    pub counter: u64,
}

/// Base value for extended SAI identifiers.
///
/// When the extension bit is set in the enterprise number,
/// this value is added to the base type_id or stat_id to create
/// an extended identifier space.
const EXTENSIONS_RANGE_BASE: u32 = 0x2000_0000;

impl SAIStat {
    /// Creates a SAIStat directly from IPFIX field specifier and data record value.
    ///
    /// # Arguments
    ///
    /// * `field_spec` - IPFIX field specifier containing identifiers
    /// * `value` - IPFIX data record value containing counter data
    /// * `object_names` - Vector of object names (1-based indexing)
    ///
    /// # Returns
    ///
    /// A new SAIStat instance with decoded identifiers and resolved object name
    pub fn from_ipfix(
        field_spec: &FieldSpecifier,
        value: &DataRecordValue,
        object_names: &[String],
    ) -> Self {
        let enterprise_number = field_spec.enterprise_number.unwrap_or(0);
        let label = field_spec.information_element_identifier;

        // Extract extension flags from enterprise number
        let type_id_extension = (enterprise_number & 0x8000_0000) != 0;
        let stat_id_extension = (enterprise_number & 0x0000_8000) != 0;

        // Extract base identifiers from enterprise number
        let mut type_id = (enterprise_number & 0x7FFF_0000) >> 16;
        let mut stat_id = enterprise_number & 0x0000_7FFF;

        // Apply extensions if flags are set
        if type_id_extension {
            type_id = type_id.saturating_add(EXTENSIONS_RANGE_BASE);
        }

        if stat_id_extension {
            stat_id = stat_id.saturating_add(EXTENSIONS_RANGE_BASE);
        }

        // Extract counter value from data record
        let counter = match value {
            DataRecordValue::Bytes(bytes) => {
                if bytes.len() >= 8 {
                    NetworkEndian::read_u64(bytes)
                } else {
                    // Handle shorter byte arrays by padding with zeros
                    let mut padded = [0u8; 8];
                    let copy_len = std::cmp::min(bytes.len(), 8);
                    padded[8 - copy_len..].copy_from_slice(&bytes[..copy_len]);
                    NetworkEndian::read_u64(&padded)
                }
            }
            _ => {
                // For non-byte values, default to 0
                // Could potentially handle other DataRecordValue variants here
                0
            }
        };

        // Resolve object name from label
        let object_name = if label > 0 && (label as usize) <= object_names.len() {
            // Convert 1-based label to 0-based index
            object_names[(label - 1) as usize].clone()
        } else {
            // Fallback to label number if object name not found
            format!("unknown_{}", label)
        };

        SAIStat {
            object_name,
            type_id,
            stat_id,
            counter,
        }
    }
}

/// Collection of SAI statistics with an associated observation timestamp.
///
/// This structure represents a snapshot of multiple SAI statistics
/// collected at a specific point in time, as indicated by the observation_time.
#[derive(Debug, Clone)]
pub struct SAIStats {
    /// Timestamp when these statistics were observed (typically from IPFIX observation time field)
    pub observation_time: u64,
    /// Vector of individual SAI statistic entries
    pub stats: Vec<SAIStat>,
}

impl SAIStats {
    /// Creates a new SAIStats instance.
    ///
    /// # Arguments
    ///
    /// * `observation_time` - Timestamp when statistics were collected
    /// * `stats` - Vector of SAI statistics
    ///
    /// # Returns
    ///
    /// A new SAIStats instance
    pub fn new(observation_time: u64, stats: Vec<SAIStat>) -> Self {
        Self {
            observation_time,
            stats,
        }
    }

    /// Returns the number of statistics in this collection.
    #[allow(dead_code)]
    pub fn len(&self) -> usize {
        self.stats.len()
    }

    /// Returns true if this collection contains no statistics.
    #[allow(dead_code)]
    pub fn is_empty(&self) -> bool {
        self.stats.is_empty()
    }

    /// Returns an iterator over the statistics.
    #[allow(dead_code)]
    pub fn iter(&self) -> std::slice::Iter<SAIStat> {
        self.stats.iter()
    }
}

impl PartialEq for SAIStats {
    /// Compares two SAIStats instances for equality.
    ///
    /// Two SAIStats are considered equal if they have the same observation_time
    /// and contain the same set of statistics (order independent).
    ///
    /// # Arguments
    ///
    /// * `other` - The other SAIStats instance to compare with
    ///
    /// # Returns
    ///
    /// true if the instances are equal, false otherwise
    fn eq(&self, other: &Self) -> bool {
        // Quick checks first
        if self.observation_time != other.observation_time {
            return false;
        }

        if self.stats.len() != other.stats.len() {
            return false;
        }

        // For small collections, use the existing approach
        if self.stats.len() <= 10 {
            return self.stats.iter().all(|stat| other.stats.contains(stat));
        }

        // For larger collections, use a more efficient approach
        use std::collections::HashSet;
        let self_set: HashSet<&SAIStat> = self.stats.iter().collect();
        let other_set: HashSet<&SAIStat> = other.stats.iter().collect();
        self_set == other_set
    }
}

/// Type alias for Arc-wrapped SAIStats to enable efficient sharing between actors.
///
/// This type is used for passing SAI statistics messages between different
/// parts of the system without expensive cloning operations.
pub type SAIStatsMessage = Arc<SAIStats>;

/// Extension trait for creating SAIStatsMessage instances.
#[allow(dead_code)]
pub trait SAIStatsMessageExt {
    /// Creates a new SAIStatsMessage from SAIStats.
    ///
    /// # Arguments
    ///
    /// * `stats` - The SAIStats instance to wrap in an Arc
    ///
    /// # Returns
    ///
    /// A new SAIStatsMessage (Arc<SAIStats>)
    fn into_message(self) -> SAIStatsMessage;

    /// Creates a new SAIStatsMessage with the given observation time and statistics.
    ///
    /// # Arguments
    ///
    /// * `observation_time` - Timestamp when statistics were collected
    /// * `stats` - Vector of SAI statistics
    ///
    /// # Returns
    ///
    /// A new SAIStatsMessage (Arc<SAIStats>)
    fn from_parts(observation_time: u64, stats: Vec<SAIStat>) -> SAIStatsMessage;
}

impl SAIStatsMessageExt for SAIStats {
    fn into_message(self) -> SAIStatsMessage {
        Arc::new(self)
    }

    fn from_parts(observation_time: u64, stats: Vec<SAIStat>) -> SAIStatsMessage {
        Arc::new(SAIStats::new(observation_time, stats))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use ipfixrw::parser::{DataRecordValue, FieldSpecifier};

    /// Helper function to create a test field specifier
    fn create_field_spec(element_id: u16, enterprise_number: Option<u32>) -> FieldSpecifier {
        FieldSpecifier::new(enterprise_number, element_id, 8)
    }

    /// Helper function to create test byte data
    fn create_byte_value(value: u64) -> DataRecordValue {
        let mut bytes = [0u8; 8];
        NetworkEndian::write_u64(&mut bytes, value);
        DataRecordValue::Bytes(bytes.to_vec())
    }

    #[test]
    fn test_sai_stat_from_ipfix_basic() {
        let field_spec = create_field_spec(2, Some(0x12340000)); // label 2, type_id 0x1234, stat_id 0
        let value = create_byte_value(12345);
        let object_names = vec!["Ethernet0".to_string(), "Ethernet1".to_string()];

        let stat = SAIStat::from_ipfix(&field_spec, &value, &object_names);

        assert_eq!(stat.object_name, "Ethernet1"); // label 2 -> index 1 (1-based)
        assert_eq!(stat.type_id, 0x1234);
        assert_eq!(stat.stat_id, 0);
        assert_eq!(stat.counter, 12345);
    }

    #[test]
    fn test_sai_stat_from_ipfix_with_extensions() {
        // Test with both extension bits set
        let enterprise_number = 0x80008000 | 0x12340567;
        let field_spec = create_field_spec(1, Some(enterprise_number)); // label 1
        let value = create_byte_value(99999);
        let object_names = vec!["Ethernet0".to_string()];

        let stat = SAIStat::from_ipfix(&field_spec, &value, &object_names);

        assert_eq!(stat.object_name, "Ethernet0"); // label 1 -> index 0 (1-based)
        assert_eq!(stat.type_id, 0x1234 + EXTENSIONS_RANGE_BASE);
        assert_eq!(stat.stat_id, 0x0567 + EXTENSIONS_RANGE_BASE);
        assert_eq!(stat.counter, 99999);
    }

    #[test]
    fn test_sai_stat_from_ipfix_short_bytes() {
        let field_spec = create_field_spec(1, Some(0x00010002));
        let short_bytes = vec![0x12, 0x34]; // Only 2 bytes instead of 8
        let value = DataRecordValue::Bytes(short_bytes);
        let object_names = vec!["Ethernet0".to_string()];

        let stat = SAIStat::from_ipfix(&field_spec, &value, &object_names);

        assert_eq!(stat.object_name, "Ethernet0");
        assert_eq!(stat.counter, 0x1234); // Should be padded correctly
    }

    #[test]
    fn test_sai_stat_from_ipfix_non_bytes() {
        let field_spec = create_field_spec(1, Some(0x00050006));
        let value = DataRecordValue::String("test".to_string());
        let object_names = vec!["Ethernet0".to_string()];

        let stat = SAIStat::from_ipfix(&field_spec, &value, &object_names);

        assert_eq!(stat.object_name, "Ethernet0");
        assert_eq!(stat.counter, 0); // Should default to 0 for non-byte values
    }

    #[test]
    fn test_sai_stat_from_ipfix_invalid_label() {
        let field_spec = create_field_spec(5, Some(0x00010002)); // label 5, out of range
        let value = create_byte_value(1000);
        let object_names = vec!["Ethernet0".to_string(), "Ethernet1".to_string()]; // Only 2 objects

        let stat = SAIStat::from_ipfix(&field_spec, &value, &object_names);

        assert_eq!(stat.object_name, "unknown_5"); // Fallback for invalid label
        assert_eq!(stat.type_id, 1);
        assert_eq!(stat.stat_id, 2);
        assert_eq!(stat.counter, 1000);
    }

    #[test]
    fn test_sai_stat_from_ipfix_zero_label() {
        let field_spec = create_field_spec(0, Some(0x00010002)); // label 0, invalid
        let value = create_byte_value(1000);
        let object_names = vec!["Ethernet0".to_string()];

        let stat = SAIStat::from_ipfix(&field_spec, &value, &object_names);

        assert_eq!(stat.object_name, "unknown_0"); // Fallback for zero label
        assert_eq!(stat.type_id, 1);
        assert_eq!(stat.stat_id, 2);
        assert_eq!(stat.counter, 1000);
    }

    #[test]
    fn test_sai_stats_creation() {
        let stats = vec![
            SAIStat {
                object_name: "Ethernet0".to_string(),
                type_id: 100,
                stat_id: 200,
                counter: 1000,
            },
            SAIStat {
                object_name: "Ethernet1".to_string(),
                type_id: 101,
                stat_id: 201,
                counter: 2000,
            },
        ];

        let sai_stats = SAIStats::new(12345, stats.clone());

        assert_eq!(sai_stats.observation_time, 12345);
        assert_eq!(sai_stats.len(), 2);
        assert!(!sai_stats.is_empty());
        assert_eq!(sai_stats.stats, stats);
    }

    #[test]
    fn test_sai_stats_equality() {
        let stats1 = vec![
            SAIStat {
                object_name: "Ethernet0".to_string(),
                type_id: 100,
                stat_id: 200,
                counter: 1000,
            },
            SAIStat {
                object_name: "Ethernet1".to_string(),
                type_id: 101,
                stat_id: 201,
                counter: 2000,
            },
        ];

        let stats2 = vec![
            SAIStat {
                object_name: "Ethernet1".to_string(),
                type_id: 101,
                stat_id: 201,
                counter: 2000,
            },
            SAIStat {
                object_name: "Ethernet0".to_string(),
                type_id: 100,
                stat_id: 200,
                counter: 1000,
            },
        ];

        let sai_stats1 = SAIStats::new(12345, stats1);
        let sai_stats2 = SAIStats::new(12345, stats2.clone());
        let sai_stats3 = SAIStats::new(12346, stats2);

        assert_eq!(sai_stats1, sai_stats2); // Same content, different order
        assert_ne!(sai_stats1, sai_stats3); // Different observation time
    }

    #[test]
    fn test_sai_stats_message_creation() {
        let stats = vec![SAIStat {
            object_name: "Ethernet0".to_string(),
            type_id: 100,
            stat_id: 200,
            counter: 1000,
        }];

        let message1 = SAIStats::new(12345, stats.clone()).into_message();
        let message2 = SAIStats::from_parts(12345, stats);

        assert_eq!(message1.observation_time, message2.observation_time);
        assert_eq!(message1.stats, message2.stats);
    }

    #[test]
    fn test_extensions_range_overflow() {
        // Test that we handle potential overflow gracefully
        let enterprise_number = 0x80008000 | 0x7FFF7FFF; // Maximum values with extensions
        let field_spec = create_field_spec(1, Some(enterprise_number));
        let value = create_byte_value(555);
        let object_names = vec!["Ethernet0".to_string()];

        let stat = SAIStat::from_ipfix(&field_spec, &value, &object_names);

        // Should use saturating_add to prevent overflow
        assert_eq!(stat.type_id, 0x7FFF + EXTENSIONS_RANGE_BASE);
        assert_eq!(stat.stat_id, 0x7FFF + EXTENSIONS_RANGE_BASE);
        assert_eq!(stat.object_name, "Ethernet0");
    }
}
