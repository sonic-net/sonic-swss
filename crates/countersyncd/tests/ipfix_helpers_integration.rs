mod ipfix_test_helpers;

use std::{collections::HashMap, sync::Arc};

use tokio::sync::mpsc::channel;
use tokio::time::{sleep, timeout, Duration};

use countersyncd::actor::{aggregator::AggregatorActor, ipfix::IpfixActor};
use countersyncd::message::{
    buffer::SocketBufferMessage,
    aggregator::{AggregatorConfig, AggregatorConfigMessage, AggregatorStatsMessage},
    ipfix::IPFixTemplatesMessage,
    saistats::SAIStatsMessage,
};

async fn start_ipfix_aggregator_pipeline(
    template_capacity: usize,
    buffer_capacity: usize,
    config_capacity: usize,
    stats_capacity: usize,
) -> (
    tokio::sync::mpsc::Sender<IPFixTemplatesMessage>,
    tokio::sync::mpsc::Sender<SocketBufferMessage>,
    tokio::sync::mpsc::Sender<AggregatorConfigMessage>,
    tokio::sync::mpsc::Receiver<SAIStatsMessage>,
) {
    let (buffer_sender, buffer_receiver) = channel::<SocketBufferMessage>(buffer_capacity);
    let (template_sender, template_receiver) = channel(template_capacity);
    let (aggregator_config_sender, aggregator_config_receiver) = channel(config_capacity);
    let (aggregator_stats_sender, aggregator_stats_receiver) =
        channel::<AggregatorStatsMessage>(stats_capacity);
    let (saistats_sender, saistats_receiver) = channel::<SAIStatsMessage>(stats_capacity);

    let mut ipfix = IpfixActor::new(template_receiver, buffer_receiver);
    ipfix.add_recipient(aggregator_stats_sender);
    tokio::task::spawn_blocking(move || {
        let rt = tokio::runtime::Runtime::new().expect("ipfix runtime");
        rt.block_on(async move {
            IpfixActor::run(ipfix).await;
        });
    });

    let mut aggregator = AggregatorActor::new(aggregator_config_receiver, aggregator_stats_receiver);
    aggregator.add_recipient(saistats_sender);
    tokio::spawn(async move {
        AggregatorActor::run(aggregator).await;
    });

    (
        template_sender,
        buffer_sender,
        aggregator_config_sender,
        saistats_receiver,
    )
}

async fn send_single_counter_template(
    template_sender: &tokio::sync::mpsc::Sender<IPFixTemplatesMessage>,
    key: &str,
    template_id: u16,
) -> Vec<u8> {
    let templates = ipfix_test_helpers::generate_ipfix_templates(1, template_id);
    template_sender
        .send(IPFixTemplatesMessage::new(
            key.to_string(),
            Arc::new(templates.clone()),
            Some(vec!["Ethernet0".to_string()]),
            Some(vec![1]),
        ))
        .await
        .expect("template send should succeed");
    sleep(Duration::from_millis(50)).await;

    templates
}

async fn send_record_with_observation_time(
    buffer_sender: &tokio::sync::mpsc::Sender<SocketBufferMessage>,
    templates: &[u8],
    observation_time: u64,
) {
    let records = ipfix_test_helpers::generate_ipfix_records_with_observation_times(
        templates,
        std::iter::once(observation_time),
    );
    buffer_sender
        .send(Arc::new(records))
        .await
        .expect("record send should succeed");
}

#[tokio::test]
async fn ipfix_aggregator_downsamples_10us_stream_to_100us() {
    let key = "aggregated_downsample|PORT";
    let (template_sender, buffer_sender, config_sender, mut saistats_receiver) =
        start_ipfix_aggregator_pipeline(1, 25, 1, 10).await;

    let templates = send_single_counter_template(&template_sender, key, 500).await;
    config_sender
        .send(AggregatorConfigMessage::new(
            key.to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(100),
            }),
        ))
        .await
        .expect("aggregator config send should succeed");

    for observation_time in (0..=200_000).step_by(10_000) {
        send_record_with_observation_time(&buffer_sender, &templates, observation_time).await;
    }

    let first = timeout(Duration::from_secs(2), saistats_receiver.recv())
        .await
        .expect("first aggregated sample should arrive")
        .expect("first aggregated sample channel should be open");
    let second = timeout(Duration::from_secs(2), saistats_receiver.recv())
        .await
        .expect("second aggregated sample should arrive")
        .expect("second aggregated sample channel should be open");

    assert_eq!(first.observation_time, 90_000);
    assert_eq!(first.stats.len(), 1);
    assert_eq!(first.stats[0].counter, 90_000);
    assert_eq!(second.observation_time, 190_000);
    assert_eq!(second.stats.len(), 1);
    assert_eq!(second.stats[0].counter, 190_000);
    assert!(
        timeout(Duration::from_millis(100), saistats_receiver.recv())
            .await
            .is_err(),
        "10us input samples should only produce completed 100us windows"
    );
}

#[tokio::test]
async fn aggregator_delete_while_streaming_forwards_later_samples() {
    let key = "aggregator_delete_while_streaming|PORT";
    let (template_sender, buffer_sender, config_sender, mut saistats_receiver) =
        start_ipfix_aggregator_pipeline(1, 10, 2, 10).await;

    let templates = send_single_counter_template(&template_sender, key, 501).await;
    config_sender
        .send(AggregatorConfigMessage::new(
            key.to_string(),
            Some(AggregatorConfig {
                reporting_rate: Some(100),
            }),
        ))
        .await
        .expect("aggregator config send should succeed");

    send_record_with_observation_time(&buffer_sender, &templates, 0).await;
    send_record_with_observation_time(&buffer_sender, &templates, 10_000).await;
    assert!(
        timeout(Duration::from_millis(100), saistats_receiver.recv())
            .await
            .is_err(),
        "samples in the current reporting window should be buffered before deletion"
    );

    config_sender
        .send(AggregatorConfigMessage::delete(key.to_string()))
        .await
        .expect("aggregator delete send should succeed");
    sleep(Duration::from_millis(20)).await;

    send_record_with_observation_time(&buffer_sender, &templates, 20_000).await;
    let forwarded = timeout(Duration::from_secs(2), saistats_receiver.recv())
        .await
        .expect("post-delete sample should be forwarded")
        .expect("post-delete sample channel should be open");

    assert_eq!(forwarded.observation_time, 20_000);
    assert_eq!(forwarded.stats.len(), 1);
    assert_eq!(forwarded.stats[0].counter, 20_000);
}

#[tokio::test]
async fn ipfix_templates_delete_and_readd_schema_change() {
    let (buffer_sender, buffer_receiver) = channel::<SocketBufferMessage>(5);
    let (template_sender, template_receiver) = channel(1);
    let (saistats_sender, mut saistats_receiver) = channel::<AggregatorStatsMessage>(10);

    let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
    actor.add_recipient(saistats_sender);

    let actor_handle = tokio::task::spawn_blocking(move || {
        let rt = tokio::runtime::Runtime::new().expect("ipfix runtime");
        rt.block_on(async move {
            IpfixActor::run(actor).await;
        });
    });

    let max_counters = ipfix_test_helpers::max_counters_per_template();
    // Prepare five templates across three keys with varying counter counts (small → max)
    let template_defs = vec![
        ("helper_key_a", 300u16, 2usize),
        ("helper_key_a", 301u16, 3usize),
        ("helper_key_b", 302u16, 16usize),
        ("helper_key_b", 303u16, 128usize),
        ("helper_key_c", 304u16, max_counters),
    ];
    let delete_key = "helper_key_b";
    let mut all_templates_bytes = Vec::new();
    let mut templates_by_key: HashMap<&str, Vec<u8>> = HashMap::new();
    let mut key_order: Vec<&str> = Vec::new();

    for (key, template_id, counters) in &template_defs {
        if !key_order.contains(key) {
            key_order.push(*key);
        }

        let template = ipfix_test_helpers::generate_ipfix_templates(*counters, *template_id);
        all_templates_bytes.extend_from_slice(&template);
        templates_by_key
            .entry(*key)
            .or_default()
            .extend_from_slice(&template);
    }

    let deleted_key_templates = templates_by_key
        .get(delete_key)
        .cloned()
        .unwrap_or_default();

    for key in key_order {
        if let Some(bytes) = templates_by_key.get(key) {
            template_sender
                .send(IPFixTemplatesMessage::new(
                    key.to_string(),
                    Arc::new(bytes.clone()),
                    Some(vec!["Obj0".to_string(), "Obj1".to_string()]),
                    Some(vec![1, 2]),
                ))
                .await
                .expect("template send should succeed");
        }
    }

    // Allow actor to process templates
    sleep(Duration::from_millis(50)).await;

    // Generate matching records for all templates across all keys
    let records = ipfix_test_helpers::generate_ipfix_records(&all_templates_bytes);
    buffer_sender
        .send(Arc::new(records))
        .await
        .expect("record send should succeed");

    let expected_counts: Vec<usize> = template_defs.iter().map(|(_, _, c)| *c).collect();

    let mut received = Vec::new();
    for _ in 0..expected_counts.len() {
        if let Ok(Some(stats_msg)) = timeout(Duration::from_secs(2), saistats_receiver.recv()).await {
            let stats = Arc::try_unwrap(stats_msg.stats).expect("unwrap stats Arc");
            received.push(stats);
        } else {
            break;
        }
    }

    assert_eq!(received.len(), expected_counts.len(), "should receive one stats message per template");

    for (i, stats) in received.iter().enumerate() {
        let expected_count = expected_counts[i];
        let expected_obs_time = (i as u64) + 1;

        assert_eq!(stats.observation_time, expected_obs_time, "observation time mismatch for message {}", i);
        assert_eq!(stats.stats.len(), expected_count, "counter count mismatch for message {}", i);

        let mut got: Vec<(u32, u32, u64)> = stats
            .stats
            .iter()
            .map(|s| (s.type_id, s.stat_id, s.counter))
            .collect();
        got.sort_by(|a, b| a.1.cmp(&b.1));

        let mut probe_indices = vec![0];
        if expected_count > 1 {
            probe_indices.push(expected_count / 2);
            probe_indices.push(expected_count - 1);
        }

        probe_indices.sort_unstable();
        probe_indices.dedup();

        for idx in probe_indices {
            let (type_id, stat_id, counter) = got[idx];
            let expected_idx = (idx + 1) as u32;

            assert_eq!(type_id, expected_idx, "type_id mismatch at stat {} for message {}", idx, i);
            assert_eq!(stat_id, expected_idx, "stat_id mismatch at stat {} for message {}", idx, i);
            assert_eq!(counter, expected_obs_time + idx as u64, "counter mismatch at stat {} for message {}", idx, i);
        }
    }

    // Deleting one key's templates should cause subsequent data for that key to be dropped
    template_sender
        .send(IPFixTemplatesMessage::delete(delete_key.to_string()))
        .await
        .expect("template delete should succeed");

    sleep(Duration::from_millis(20)).await;

    let deleted_records = ipfix_test_helpers::generate_ipfix_records(&deleted_key_templates);
    buffer_sender
        .send(Arc::new(deleted_records))
        .await
        .expect("record send after delete should succeed");

    // Give the actor a moment to process, then ensure no stats arrive
    sleep(Duration::from_millis(50)).await;
    assert!(
        saistats_receiver.try_recv().is_err(),
        "records for deleted templates should be dropped"
    );

    // Re-add the deleted key with the same template IDs but different shapes
    let readd_template_defs = vec![
        (delete_key, 302u16, 4usize),
        (delete_key, 303u16, 6usize),
    ];

    let mut readd_templates_bytes = Vec::new();
    for (_, template_id, counters) in &readd_template_defs {
        let template = ipfix_test_helpers::generate_ipfix_templates(*counters, *template_id);
        readd_templates_bytes.extend_from_slice(&template);
    }

    template_sender
        .send(IPFixTemplatesMessage::new(
            delete_key.to_string(),
            Arc::new(readd_templates_bytes.clone()),
            Some(vec!["ObjA".to_string(), "ObjB".to_string()]),
            Some(vec![1, 2]),
        ))
        .await
        .expect("template re-add should succeed");

    sleep(Duration::from_millis(50)).await;

    let readd_records = ipfix_test_helpers::generate_ipfix_records(&readd_templates_bytes);
    buffer_sender
        .send(Arc::new(readd_records))
        .await
        .expect("record send after re-add should succeed");

    let expected_readd_counts: Vec<usize> = readd_template_defs.iter().map(|(_, _, c)| *c).collect();
    let mut readd_received = Vec::new();
    for _ in 0..expected_readd_counts.len() {
        if let Ok(Some(stats_msg)) = timeout(Duration::from_secs(2), saistats_receiver.recv()).await {
            let stats = Arc::try_unwrap(stats_msg.stats).expect("unwrap stats Arc");
            readd_received.push(stats);
        } else {
            break;
        }
    }

    assert_eq!(
        readd_received.len(),
        expected_readd_counts.len(),
        "should receive one stats message per re-added template"
    );

    for (i, stats) in readd_received.iter().enumerate() {
        let expected_count = expected_readd_counts[i];
        let expected_obs_time = (i as u64) + 1;

        assert_eq!(stats.observation_time, expected_obs_time, "observation time mismatch after re-add for message {}", i);
        assert_eq!(stats.stats.len(), expected_count, "counter count mismatch after re-add for message {}", i);

        let mut got: Vec<(u32, u32, u64)> = stats
            .stats
            .iter()
            .map(|s| (s.type_id, s.stat_id, s.counter))
            .collect();
        got.sort_by(|a, b| a.1.cmp(&b.1));

        let mut probe_indices = vec![0];
        if expected_count > 1 {
            probe_indices.push(expected_count / 2);
            probe_indices.push(expected_count - 1);
        }

        probe_indices.sort_unstable();
        probe_indices.dedup();

        for idx in probe_indices {
            let (type_id, stat_id, counter) = got[idx];
            let expected_idx = (idx + 1) as u32;

            assert_eq!(type_id, expected_idx, "type_id mismatch at stat {} after re-add for message {}", idx, i);
            assert_eq!(stat_id, expected_idx, "stat_id mismatch at stat {} after re-add for message {}", idx, i);
            assert_eq!(counter, expected_obs_time + idx as u64, "counter mismatch at stat {} after re-add for message {}", idx, i);
        }
    }

    drop(buffer_sender);
    drop(template_sender);
    drop(saistats_receiver);

    actor_handle.await.expect("actor should finish");
}
