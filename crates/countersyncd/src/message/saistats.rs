//! SAI statistics exchanged between countersyncd actors.

use std::{ops::Range, sync::Arc};

/// Base of the SAI extension identifier range.
pub const EXTENSIONS_RANGE_BASE: u32 = 0x2000_0000;

/// Decode the SONiC HFT private enterprise-number layout.
///
/// The high half contains the SAI object type and the low half contains the
/// statistic ID. The top bit in each half selects the SAI extension range.
pub fn decode_sai_ids(enterprise_number: u32) -> (u32, u32) {
    let mut type_id = (enterprise_number & 0x7fff_0000) >> 16;
    let mut stat_id = enterprise_number & 0x0000_7fff;

    if enterprise_number & 0x8000_0000 != 0 {
        type_id = type_id.saturating_add(EXTENSIONS_RANGE_BASE);
    }
    if enterprise_number & 0x0000_8000 != 0 {
        stat_id = stat_id.saturating_add(EXTENSIONS_RANGE_BASE);
    }

    (type_id, stat_id)
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct SAIStat {
    /// Shared because a template reuses the same object name for every sample.
    pub object_name: Arc<str>,
    pub type_id: u32,
    pub stat_id: u32,
    pub counter: u64,
}

impl SAIStat {
    #[allow(dead_code)]
    pub fn new(object_name: impl Into<Arc<str>>, type_id: u32, stat_id: u32, counter: u64) -> Self {
        Self {
            object_name: object_name.into(),
            type_id,
            stat_id,
            counter,
        }
    }
}

#[derive(Debug, Clone)]
pub struct SAIStats {
    pub observation_time: u64,
    pub stats: Vec<SAIStat>,
}

impl PartialEq for SAIStats {
    fn eq(&self, other: &Self) -> bool {
        if self.observation_time != other.observation_time || self.stats.len() != other.stats.len()
        {
            return false;
        }
        let mut counts = std::collections::HashMap::with_capacity(self.stats.len());
        for stat in &self.stats {
            *counts.entry(stat).or_insert(0usize) += 1;
        }
        for stat in &other.stats {
            let Some(count) = counts.get_mut(stat) else {
                return false;
            };
            if *count == 0 {
                return false;
            }
            *count -= 1;
        }
        counts.values().all(|count| *count == 0)
    }
}

impl Eq for SAIStats {}

impl SAIStats {
    #[allow(dead_code)]
    pub fn new(observation_time: u64, stats: Vec<SAIStat>) -> Self {
        Self {
            observation_time,
            stats,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct SAIStatsRecord {
    observation_time: u64,
    stats: Range<usize>,
}

/// A borrowed record in a [`SAIStatsBatch`].
#[derive(Debug, Clone, Copy)]
pub struct SAIStatsRef<'a> {
    pub observation_time: u64,
    pub stats: &'a [SAIStat],
}

/// Flat representation of many samples.
///
/// Keeping one stat vector per channel item avoids a vector and Arc allocation
/// for every IPFIX record while preserving record boundaries and wire order.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct SAIStatsBatch {
    records: Vec<SAIStatsRecord>,
    stats: Vec<SAIStat>,
}

impl SAIStatsBatch {
    pub fn with_capacity(records: usize, stats: usize) -> Self {
        Self {
            records: Vec::with_capacity(records),
            stats: Vec::with_capacity(stats),
        }
    }

    pub fn from_stats(stats: SAIStats) -> Self {
        let mut batch = Self::with_capacity(1, stats.stats.len());
        batch.push_record(stats.observation_time, stats.stats);
        batch
    }

    pub fn push_record(&mut self, observation_time: u64, stats: impl IntoIterator<Item = SAIStat>) {
        let start = self.stats.len();
        self.stats.extend(stats);
        let end = self.stats.len();
        self.records.push(SAIStatsRecord {
            observation_time,
            stats: start..end,
        });
    }

    pub fn reserve(&mut self, records: usize, counters: usize) {
        self.records.reserve(records);
        self.stats.reserve(counters);
    }

    #[allow(dead_code)]
    pub fn record_count(&self) -> usize {
        self.records.len()
    }

    pub fn counter_count(&self) -> usize {
        self.stats.len()
    }

    pub fn is_empty(&self) -> bool {
        self.records.is_empty()
    }

    pub fn iter(&self) -> impl ExactSizeIterator<Item = SAIStatsRef<'_>> {
        self.records.iter().map(|record| SAIStatsRef {
            observation_time: record.observation_time,
            stats: &self.stats[record.stats.clone()],
        })
    }
}

impl From<SAIStats> for SAIStatsBatch {
    fn from(value: SAIStats) -> Self {
        Self::from_stats(value)
    }
}

pub type SAIStatsBatchMessage = Arc<SAIStatsBatch>;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes_sai_ids_with_distinct_halves() {
        assert_eq!(decode_sai_ids(0x1234_0567), (0x1234, 0x0567));
        assert_eq!(
            decode_sai_ids(0x9234_8567),
            (
                EXTENSIONS_RANGE_BASE + 0x1234,
                EXTENSIONS_RANGE_BASE + 0x0567
            )
        );
    }

    #[test]
    fn flat_batch_preserves_record_boundaries_and_order() {
        let mut batch = SAIStatsBatch::with_capacity(2, 3);
        batch.push_record(
            10,
            [
                SAIStat::new("Ethernet0", 1, 2, 3),
                SAIStat::new("Ethernet4", 1, 2, 4),
            ],
        );
        batch.push_record(20, [SAIStat::new("Ethernet8", 1, 2, 5)]);

        let records: Vec<_> = batch.iter().collect();
        assert_eq!(batch.record_count(), 2);
        assert_eq!(batch.counter_count(), 3);
        assert_eq!(records[0].observation_time, 10);
        assert_eq!(records[0].stats[1].object_name.as_ref(), "Ethernet4");
        assert_eq!(records[1].observation_time, 20);
        assert_eq!(records[1].stats[0].counter, 5);
    }

    #[test]
    fn equality_preserves_duplicate_multiplicity() {
        let duplicate = SAIStat::new("Ethernet0", 1, 2, 3);
        let left = SAIStats::new(1, vec![duplicate.clone(), duplicate]);
        let right = SAIStats::new(
            1,
            vec![
                SAIStat::new("Ethernet0", 1, 2, 3),
                SAIStat::new("Ethernet4", 1, 2, 3),
            ],
        );
        assert_ne!(left, right);

        let reordered = SAIStats::new(
            1,
            vec![
                SAIStat::new("Ethernet4", 1, 2, 3),
                SAIStat::new("Ethernet0", 1, 2, 3),
            ],
        );
        assert_eq!(right, reordered);
    }
}
