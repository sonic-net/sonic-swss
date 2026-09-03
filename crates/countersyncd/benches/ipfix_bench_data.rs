use std::collections::BTreeMap;
use std::sync::Arc;

use byteorder::{ByteOrder, NetworkEndian};
use countersyncd::message::{buffer::SocketBufferMessage, ipfix::IPFixTemplatesMessage};

#[path = "../tests/ipfix_test_helpers.rs"]
mod ipfix_test_helpers;

const TARGET_TOTAL_COUNTERS: usize = 4_000_000;
const READINESS_TEMPLATE_ID: u16 = u16::MAX;
pub const PAYLOAD_POOL_RECORDS: usize = 16;

#[derive(Clone, Debug)]
pub struct TemplateSpec {
    pub key: String,
    pub template_id: u16,
    pub counters: usize,
}

#[derive(Clone, Debug)]
pub struct DatasetSpec {
    pub name: &'static str,
    pub templates: Vec<TemplateSpec>,
}

#[derive(Clone)]
#[allow(dead_code)]
pub struct PreparedTemplate {
    pub spec: TemplateSpec,
    pub base_record: Arc<Vec<u8>>,
    pub payload_pool: Arc<[SocketBufferMessage]>,
    pub records: usize,
}

#[derive(Clone)]
#[allow(dead_code)]
pub struct PreparedDataset {
    pub spec: DatasetSpec,
    pub templates: Vec<PreparedTemplate>,
    pub template_messages: Vec<IPFixTemplatesMessage>,
    pub expected_messages: usize,
    pub expected_counters: usize,
    pub readiness_record: SocketBufferMessage,
}

impl DatasetSpec {
    pub fn total_counters_per_iteration(&self) -> usize {
        compute_distribution(self).2
    }

    #[allow(dead_code)]
    pub fn total_messages_per_iteration(&self) -> usize {
        compute_distribution(self).1
    }
}

impl PreparedDataset {
    pub fn new(spec: DatasetSpec) -> Self {
        let mut templates = Vec::with_capacity(spec.templates.len());
        let mut key_to_templates: BTreeMap<String, (Vec<u8>, usize)> = BTreeMap::new();

        let (records_per_template, expected_messages, expected_counters) =
            compute_distribution(&spec);

        for (idx, tmpl) in spec.templates.iter().enumerate() {
            let tmpl_bytes =
                ipfix_test_helpers::generate_ipfix_templates(tmpl.counters, tmpl.template_id);
            let (template_bytes, max_counters) = key_to_templates
                .entry(tmpl.key.clone())
                .or_insert_with(|| (Vec::new(), 0));
            template_bytes.extend_from_slice(&tmpl_bytes);
            *max_counters = (*max_counters).max(tmpl.counters);

            let base_record = ipfix_test_helpers::generate_ipfix_records(&tmpl_bytes);
            let payload_pool = prepare_payload_pool(&base_record, tmpl, records_per_template[idx]);

            templates.push(PreparedTemplate {
                spec: tmpl.clone(),
                base_record: Arc::clone(&payload_pool[0]),
                payload_pool: payload_pool.into(),
                records: records_per_template[idx],
            });
        }

        let mut template_messages: Vec<_> = key_to_templates
            .into_iter()
            .map(|(key, (bytes, counters))| {
                let (object_names, object_ids) =
                    ipfix_test_helpers::generate_object_metadata(counters);
                IPFixTemplatesMessage::new(
                    key,
                    Arc::new(bytes),
                    Some(object_names),
                    Some(object_ids),
                )
            })
            .collect();

        let readiness_template_id = (256..=READINESS_TEMPLATE_ID)
            .rev()
            .find(|candidate| {
                spec.templates
                    .iter()
                    .all(|template| template.template_id != *candidate)
            })
            .expect("dataset must leave one template ID for the readiness probe");
        let readiness_template_bytes =
            ipfix_test_helpers::generate_ipfix_templates(1, readiness_template_id);
        let readiness_record = Arc::new(ipfix_test_helpers::generate_ipfix_records(
            &readiness_template_bytes,
        ));
        let (readiness_names, readiness_ids) = ipfix_test_helpers::generate_object_metadata(1);
        let readiness_template = IPFixTemplatesMessage::new(
            "__benchmark_readiness_probe".to_string(),
            Arc::new(readiness_template_bytes),
            Some(readiness_names),
            Some(readiness_ids),
        );
        template_messages.push(readiness_template);

        Self {
            spec,
            templates,
            template_messages,
            expected_messages,
            expected_counters,
            readiness_record,
        }
    }
}

fn prepare_payload_pool(
    base_record: &[u8],
    template: &TemplateSpec,
    records: usize,
) -> Vec<SocketBufferMessage> {
    let pool_len = records.clamp(1, PAYLOAD_POOL_RECORDS);
    (0..pool_len)
        .map(|sequence| {
            let mut payload = base_record.to_vec();
            let observation_time = ((template.template_id as u64) << 32)
                | u64::try_from(sequence + 1).expect("payload sequence fits u64");
            NetworkEndian::write_u64(&mut payload[20..28], observation_time);

            for (index, field) in payload[28..].chunks_exact_mut(8).enumerate() {
                let field_index = u64::try_from(index + 1).expect("field index fits u64");
                let value = observation_time
                    .wrapping_mul(0x9e37_79b9_7f4a_7c15)
                    .wrapping_add(field_index)
                    .rotate_left((index % 64) as u32);
                NetworkEndian::write_u64(field, value);
            }
            Arc::new(payload)
        })
        .collect()
}

pub fn datasets() -> Vec<DatasetSpec> {
    vec![
        DatasetSpec {
            name: "one_template_two_counters",
            templates: vec![TemplateSpec {
                key: "d1_key".to_string(),
                template_id: 300,
                counters: 2,
            }],
        },
        DatasetSpec {
            name: "one_template_eight_thousand",
            templates: vec![TemplateSpec {
                key: "d2_key".to_string(),
                template_id: 400,
                counters: 8000,
            }],
        },
        DatasetSpec {
            name: "five_keys_one_template_each",
            templates: (0..5)
                .map(|idx| TemplateSpec {
                    key: format!("d3_key_{idx}"),
                    template_id: 500 + idx as u16,
                    counters: 8000,
                })
                .collect(),
        },
        DatasetSpec {
            name: "five_keys_four_templates_each",
            templates: {
                let mut tpls = Vec::with_capacity(20);
                for key_idx in 0..5 {
                    for tpl_idx in 0..4 {
                        tpls.push(TemplateSpec {
                            key: format!("d4_key_{key_idx}"),
                            template_id: 600 + (key_idx * 4 + tpl_idx) as u16,
                            counters: 8000,
                        });
                    }
                }
                tpls
            },
        },
        DatasetSpec {
            name: "five_keys_six_templates_mixed",
            templates: {
                let mut tpls = Vec::with_capacity(30);
                for key_idx in 0..5 {
                    for tpl_idx in 0..6 {
                        let counters = if tpl_idx < 3 { 8000 } else { 10 };
                        tpls.push(TemplateSpec {
                            key: format!("d5_key_{key_idx}"),
                            template_id: 700 + (key_idx * 6 + tpl_idx) as u16,
                            counters,
                        });
                    }
                }
                tpls
            },
        },
    ]
}

fn compute_distribution(spec: &DatasetSpec) -> (Vec<usize>, usize, usize) {
    if spec.templates.is_empty() {
        return (Vec::new(), 0, 0);
    }

    let mut records = Vec::with_capacity(spec.templates.len());
    let mut total_messages = 0usize;
    let mut total_counters = 0usize;

    let mut remaining_counters = TARGET_TOTAL_COUNTERS;
    let mut remaining_templates = spec.templates.len();

    for (idx, tmpl) in spec.templates.iter().enumerate() {
        let base_share = remaining_counters / remaining_templates;
        let counters_for_template = base_share;
        let records_for_template = if counters_for_template == 0 {
            0
        } else {
            (counters_for_template + tmpl.counters - 1) / tmpl.counters
        };

        let produced_counters = records_for_template * tmpl.counters;

        records.push(records_for_template);
        total_messages += records_for_template;
        total_counters += produced_counters;

        if idx + 1 < spec.templates.len() {
            remaining_counters = remaining_counters.saturating_sub(counters_for_template);
            remaining_templates -= 1;
        }
    }

    (records, total_messages, total_counters)
}
