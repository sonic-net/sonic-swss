mod ipfix_test_helpers;

use std::{collections::HashMap, sync::Arc};

use tokio::sync::mpsc::channel;
use tokio::time::{sleep, timeout, Duration};

use countersyncd::actor::ipfix::IpfixActor;
use countersyncd::message::{
    buffer::SocketBufferMessage,
    ipfix::IPFixTemplatesMessage,
};

#[tokio::test]
async fn ipfix_templates_delete_and_readd_schema_change() {
    let (buffer_sender, buffer_receiver) = channel::<SocketBufferMessage>(5);
    let (template_sender, template_receiver) = channel(1);
    let (saistats_sender, mut saistats_receiver) = channel(10);

    let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
    actor.add_recipient(saistats_sender);

    let actor_handle = tokio::spawn(async move {
        IpfixActor::run(actor).await;
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
            let stats = Arc::try_unwrap(stats_msg).expect("unwrap stats Arc");
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
            let stats = Arc::try_unwrap(stats_msg).expect("unwrap stats Arc");
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
