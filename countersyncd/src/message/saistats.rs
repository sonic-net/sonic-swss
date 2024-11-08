use ipfixrw::parser::{FieldSpecifier, DataRecordValue};
use byteorder::{ByteOrder, NetworkEndian};
use std::sync::Arc;

#[derive(Debug, PartialEq)]
pub struct SAIStat {
    pub label: u16,
    pub type_id: u32,
    pub stat_id: u32,
    pub counter: u64,
}

const EXTENSIONS_RANGE_BASE: u32 = 0x2000_0000;

impl From<(&FieldSpecifier, &DataRecordValue)> for SAIStat {

    fn from(item: (&FieldSpecifier, &DataRecordValue)) -> Self {
        let (key, value) = item;
        let enterprise_number = key.enterprise_number.unwrap_or(0);

        let type_id_extension: bool = enterprise_number & 0x8000_0000 != 0;
        let stats_id_extension: bool = enterprise_number & 0x0000_8000 != 0;

        let mut type_id = (enterprise_number & 0x7FFF_0000) >> 16;
        let mut stat_id = enterprise_number & 0x0000_7FFF;

        if type_id_extension {
            type_id += EXTENSIONS_RANGE_BASE;
        }

        if stats_id_extension {
            stat_id += EXTENSIONS_RANGE_BASE;
        }

        let counter = match value {
            DataRecordValue::Bytes(counter) => NetworkEndian::read_u64(counter),
            _ => 0,
        };

        let label = key.information_element_identifier;

        SAIStat{
            label,
            type_id,
            stat_id,
            counter,
        }
    }

}

#[derive(Debug)]
pub struct SAIStats {
    pub observation_time: u64,
    pub stats: Vec<SAIStat>,
}

impl PartialEq for SAIStats {
    fn eq(&self, other: &Self) -> bool {
        if self.observation_time != other.observation_time {
            return false
        }
        if self.stats.len() != other.stats.len() {
            return false
        }
        for s in self.stats.iter() {
            if !other.stats.contains(s) {
                return false
            }
        }
        true
    }
}

pub type SAIStatsMessage = Arc<SAIStats>;
