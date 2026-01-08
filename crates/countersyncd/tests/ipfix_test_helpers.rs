use byteorder::{ByteOrder, NetworkEndian};

/// Build a single IPFIX template message for testing/benchmarks.
/// Layout follows the described template format: one observationTimeNanoseconds
/// field plus `counters_count` enterprise fields (element IDs are 1-based with
/// enterprise bit set; enterprise number encodes type_id<<16 | stat_id where both
/// IDs equal the 1-based counter index).
pub fn max_counters_per_template() -> usize {
    // message_length = 16 (msg hdr) + 12 (set + template header + obs field) + 8*N <= 65535
    ((u16::MAX as usize - 28) / 8) as usize
}

pub fn generate_ipfix_templates(counters_count: usize, template_id: u16) -> Vec<u8> {
    assert!(template_id >= 256, "template_id must be >= 256");

    // If caller passes 0, use the maximum counters that fit in one IPFIX message.
    let counters_count = if counters_count == 0 {
        max_counters_per_template()
    } else {
        counters_count
    };

    let set_length = 12 + counters_count * 8; // set hdr (4) + template hdr (4) + obs field (4) + N enterprise fields (8 each)
    let message_length = 16 + set_length;
    assert!(message_length <= u16::MAX as usize, "template too large for a single IPFIX message");

    let mut buf = Vec::with_capacity(message_length);

    // IPFIX message header
    buf.extend_from_slice(&10u16.to_be_bytes());
    buf.extend_from_slice(&(message_length as u16).to_be_bytes());
    buf.extend_from_slice(&0u32.to_be_bytes());
    buf.extend_from_slice(&0u32.to_be_bytes());
    buf.extend_from_slice(&0u32.to_be_bytes());

    // Template set header
    buf.extend_from_slice(&2u16.to_be_bytes());
    buf.extend_from_slice(&(set_length as u16).to_be_bytes());

    // Template record header
    buf.extend_from_slice(&template_id.to_be_bytes());
    buf.extend_from_slice(&((counters_count as u16) + 1).to_be_bytes());

    // Field 1: observationTimeNanoseconds (325), length 8
    buf.extend_from_slice(&325u16.to_be_bytes());
    buf.extend_from_slice(&8u16.to_be_bytes());

    // Counter fields
    for idx in 0..counters_count {
        let element_id = 0x8000 | ((idx as u16) + 1);
        buf.extend_from_slice(&element_id.to_be_bytes());
        buf.extend_from_slice(&8u16.to_be_bytes());

        let type_id = (idx as u32) + 1;
        let stat_id = (idx as u32) + 1;
        let enterprise_number = (type_id << 16) | stat_id;
        buf.extend_from_slice(&enterprise_number.to_be_bytes());
    }

    buf
}

/// Generate IPFIX data records that conform to the supplied template bytes.
/// For each template found, emits a data message whose set ID equals the
/// template ID. Observation time starts at 1 and increments per emitted set;
/// each counter is observation_time + counter_index.
pub fn generate_ipfix_records(ipfix_templates: &[u8]) -> Vec<u8> {
    let mut records = Vec::new();
    let mut offset: usize = 0;
    let mut sequence: u32 = 0;

    while offset + 4 <= ipfix_templates.len() {
        let message_len = match get_ipfix_message_length(&ipfix_templates[offset..]) {
            Ok(len) if offset + len as usize <= ipfix_templates.len() => len as usize,
            _ => break,
        };

        let message = &ipfix_templates[offset..offset + message_len];
        if message.len() < 24 {
            offset += message_len;
            continue;
        }

        let set_id = NetworkEndian::read_u16(&message[16..18]);
        if set_id != 2 {
            offset += message_len;
            continue;
        }

        let set_length = NetworkEndian::read_u16(&message[18..20]) as usize;
        if set_length < 8 || 16 + set_length > message.len() {
            offset += message_len;
            continue;
        }

        let mut cursor = 20usize;
        let set_end = 16 + set_length;

        while cursor + 4 <= set_end {
            if cursor + 4 > message.len() {
                break;
            }

            let template_id = NetworkEndian::read_u16(&message[cursor..cursor + 2]);
            let field_count = NetworkEndian::read_u16(&message[cursor + 2..cursor + 4]) as usize;
            cursor += 4;

            if field_count == 0 {
                break;
            }

            let mut fields_consumed = true;
            for _ in 0..field_count {
                if cursor + 4 > set_end {
                    fields_consumed = false;
                    break;
                }
                let field_id = NetworkEndian::read_u16(&message[cursor..cursor + 2]);
                cursor += 4;
                if (field_id & 0x8000) != 0 {
                    if cursor + 4 > set_end {
                        fields_consumed = false;
                        break;
                    }
                    cursor += 4;
                }
            }
            if !fields_consumed {
                break;
            }

            let counters_count = field_count.saturating_sub(1);
            let data_set_length = 4 + 8 + counters_count * 8;
            let message_length = 16 + data_set_length;
            if message_length > u16::MAX as usize {
                continue;
            }

            let mut data_message = Vec::with_capacity(message_length);

            data_message.extend_from_slice(&10u16.to_be_bytes());
            data_message.extend_from_slice(&(message_length as u16).to_be_bytes());
            data_message.extend_from_slice(&0u32.to_be_bytes());
            data_message.extend_from_slice(&sequence.to_be_bytes());
            data_message.extend_from_slice(&0u32.to_be_bytes());

            data_message.extend_from_slice(&template_id.to_be_bytes());
            data_message.extend_from_slice(&(data_set_length as u16).to_be_bytes());

            let observation_time = (sequence as u64) + 1;
            data_message.extend_from_slice(&observation_time.to_be_bytes());

            for idx in 0..counters_count {
                let counter = observation_time + idx as u64;
                data_message.extend_from_slice(&counter.to_be_bytes());
            }

            records.extend_from_slice(&data_message);
            sequence = sequence.wrapping_add(1);
        }

        offset += message_len;
    }

    records
}

fn get_ipfix_message_length(data: &[u8]) -> Result<u16, &'static str> {
    if data.len() < 4 {
        return Err("Data too short for IPFIX header");
    }
    Ok(NetworkEndian::read_u16(&data[2..4]))
}
