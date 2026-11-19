/// One IPFIX message with one 8-byte counter per distinct template. Only the
/// first Set carries a timestamp; subsequent Sets inherit it in wire order.
pub fn prepare(template_count: usize, sequence: u64) -> (Vec<u8>, Vec<u8>) {
    assert!((1..=4096).contains(&template_count));
    let header = |length: usize| {
        let mut bytes = vec![0; 16];
        bytes[..2].copy_from_slice(&10u16.to_be_bytes());
        bytes[2..4].copy_from_slice(&u16::try_from(length).unwrap().to_be_bytes());
        bytes
    };
    let mut templates = Vec::new();
    let mut payload = header(24 + 12 * template_count);
    for index in 0..template_count {
        let id = 256 + index as u16;
        let timestamp = index == 0;
        let mut template = header(if timestamp { 36 } else { 32 });
        template.extend_from_slice(&2u16.to_be_bytes());
        template.extend_from_slice(&(if timestamp { 20u16 } else { 16 }).to_be_bytes());
        template.extend_from_slice(&id.to_be_bytes());
        template.extend_from_slice(&(if timestamp { 2u16 } else { 1 }).to_be_bytes());
        if timestamp {
            template.extend_from_slice(&325u16.to_be_bytes());
            template.extend_from_slice(&8u16.to_be_bytes());
        }
        template.extend_from_slice(&0x8001u16.to_be_bytes());
        template.extend_from_slice(&8u16.to_be_bytes());
        template.extend_from_slice(&0x0001_0000u32.to_be_bytes());
        templates.extend(template);
        payload.extend_from_slice(&id.to_be_bytes());
        payload.extend_from_slice(&(if timestamp { 20u16 } else { 12 }).to_be_bytes());
        if timestamp {
            payload.extend_from_slice(&(1_700_000_000_000_000_000 + sequence).to_be_bytes());
        }
        payload.extend_from_slice(&(sequence.wrapping_mul(1_000_003) + index as u64).to_be_bytes());
    }
    (templates, payload)
}
