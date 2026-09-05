mod ipfix_test_helpers;

use std::{collections::HashMap, sync::Arc};

use tokio::sync::mpsc::channel;
use tokio::time::{timeout, Duration};

use countersyncd::actor::ipfix::IpfixActor;
use countersyncd::message::{
    buffer::SocketBufferMessage,
    ipfix::{IPFixTemplateOperation, IPFixTemplatesMessage},
    saistats::SAIStatsBatchMessage,
};

type ReceivedRecord = (u64, Vec<(u32, u32, u64)>);

fn template_message(key: &str, templates: Vec<u8>, counters: usize) -> IPFixTemplatesMessage {
    let (object_names, object_ids) = ipfix_test_helpers::generate_object_metadata(counters);
    IPFixTemplatesMessage::new(
        key.to_string(),
        Arc::new(templates),
        Some(object_names),
        Some(object_ids),
    )
}

async fn receive_records(
    receiver: &mut tokio::sync::mpsc::Receiver<SAIStatsBatchMessage>,
    expected_records: usize,
) -> Vec<ReceivedRecord> {
    let mut records = Vec::with_capacity(expected_records);

    while records.len() < expected_records {
        let batch = timeout(Duration::from_secs(2), receiver.recv())
            .await
            .expect("timed out waiting for SAI stats batch")
            .expect("SAI stats channel closed early");
        let batch_counter_count = batch.counter_count();
        assert!(
            batch_counter_count <= 8192,
            "SAI stats batch exceeded the configured counter limit"
        );
        let mut iterated_counters = 0usize;
        for record in batch.iter() {
            iterated_counters += record.stats.len();
            records.push((
                record.observation_time,
                record
                    .stats
                    .iter()
                    .map(|stat| (stat.type_id, stat.stat_id, stat.counter))
                    .collect(),
            ));
        }
        assert_eq!(batch_counter_count, iterated_counters);
    }

    assert_eq!(
        records.len(),
        expected_records,
        "unexpected logical record count"
    );
    records
}

async fn send_template_barrier(
    template_sender: &tokio::sync::mpsc::Sender<IPFixTemplatesMessage>,
    buffer_sender: &tokio::sync::mpsc::Sender<SocketBufferMessage>,
    receiver: &mut tokio::sync::mpsc::Receiver<SAIStatsBatchMessage>,
    key: &str,
    template_id: u16,
) {
    let template = ipfix_test_helpers::generate_ipfix_templates(1, template_id);
    let record = ipfix_test_helpers::generate_ipfix_records(&template);
    template_sender
        .send(template_message(key, template, 1))
        .await
        .expect("barrier template send should succeed");
    let template_barrier = template_sender
        .reserve()
        .await
        .expect("template barrier reserve should succeed");
    buffer_sender
        .send(Arc::new(record))
        .await
        .expect("barrier record send should succeed");
    assert_eq!(receive_records(receiver, 1).await[0].1.len(), 1);
    drop(template_barrier);
}

#[tokio::test]
async fn deactivate_is_reversible_but_delete_reuse_is_fail_closed() {
    let (buffer_sender, buffer_receiver) = channel::<SocketBufferMessage>(5);
    let (template_sender, template_receiver) = channel(1);
    let (saistats_sender, mut saistats_receiver) = channel(10);
    let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
    actor.add_recipient(saistats_sender);
    let actor_handle = tokio::spawn(IpfixActor::run(actor));

    let template = ipfix_test_helpers::generate_ipfix_templates(1, 300);
    let record = Arc::new(ipfix_test_helpers::generate_ipfix_records(&template));
    template_sender
        .send(template_message("session", template.clone(), 1))
        .await
        .unwrap();
    send_template_barrier(
        &template_sender,
        &buffer_sender,
        &mut saistats_receiver,
        "install_barrier",
        301,
    )
    .await;

    template_sender
        .send(IPFixTemplatesMessage::deactivate("session".to_string()))
        .await
        .unwrap();
    template_sender
        .send(template_message("session", template, 1))
        .await
        .unwrap();
    let rejected_barrier = template_sender.reserve().await.unwrap();
    drop(rejected_barrier);
    send_template_barrier(
        &template_sender,
        &buffer_sender,
        &mut saistats_receiver,
        "reactivate_barrier",
        302,
    )
    .await;
    buffer_sender.send(Arc::clone(&record)).await.unwrap();
    assert_eq!(
        receive_records(&mut saistats_receiver, 1).await[0].1.len(),
        1
    );

    let delete = IPFixTemplatesMessage::delete("session".to_string());
    assert_eq!(delete.operation, IPFixTemplateOperation::Delete);
    template_sender.send(delete).await.unwrap();
    let delete_barrier = template_sender.reserve().await.unwrap();
    drop(delete_barrier);
    buffer_sender.send(record).await.unwrap();
    let barrier_record = ipfix_test_helpers::generate_ipfix_records(
        &ipfix_test_helpers::generate_ipfix_templates(1, 302),
    );
    buffer_sender.send(Arc::new(barrier_record)).await.unwrap();
    assert_eq!(receive_records(&mut saistats_receiver, 1).await.len(), 1);
    assert!(
        timeout(Duration::from_millis(50), saistats_receiver.recv())
            .await
            .is_err(),
        "late data for a deleted template ID must not be emitted"
    );

    let template = ipfix_test_helpers::generate_ipfix_templates(1, 300);
    template_sender
        .send(template_message("session", template, 1))
        .await
        .unwrap();
    let restart = actor_handle.await.unwrap().unwrap_err();
    assert!(restart.to_string().starts_with("restart required:"));
    drop(buffer_sender);
    drop(template_sender);

    // A fresh actor models the in-place exec and startup table reconciliation.
    let (buffer_sender, buffer_receiver) = channel::<SocketBufferMessage>(1);
    let (template_sender, template_receiver) = channel(1);
    let (saistats_sender, mut saistats_receiver) = channel(1);
    let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
    actor.add_recipient(saistats_sender);
    let actor_handle = tokio::spawn(IpfixActor::run(actor));
    let template = ipfix_test_helpers::generate_ipfix_templates(1, 300);
    let record = ipfix_test_helpers::generate_ipfix_records(&template);
    template_sender
        .send(template_message("session", template, 1))
        .await
        .unwrap();
    let barrier = template_sender.reserve().await.unwrap();
    buffer_sender.send(Arc::new(record)).await.unwrap();
    assert_eq!(receive_records(&mut saistats_receiver, 1).await.len(), 1);
    drop(barrier);

    drop(buffer_sender);
    drop(template_sender);
    assert!(actor_handle.await.unwrap().is_err());
}

#[tokio::test]
async fn ipfix_templates_delete_and_readd_schema_change() {
    let (buffer_sender, buffer_receiver) = channel::<SocketBufferMessage>(5);
    let (template_sender, template_receiver) = channel(1);
    let (saistats_sender, mut saistats_receiver) = channel(10);

    let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
    actor.add_recipient(saistats_sender);

    let actor_handle = tokio::spawn(IpfixActor::run(actor));

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

    for key in key_order {
        if let Some(bytes) = templates_by_key.get(key) {
            let max_counters = template_defs
                .iter()
                .filter(|(template_key, _, _)| template_key == &key)
                .map(|(_, _, counters)| *counters)
                .max()
                .expect("key has a template");
            template_sender
                .send(template_message(key, bytes.clone(), max_counters))
                .await
                .expect("template send should succeed");
        }
    }

    let readiness_template = ipfix_test_helpers::generate_ipfix_templates(1, 305);
    let readiness_record = ipfix_test_helpers::generate_ipfix_records(&readiness_template);
    template_sender
        .send(template_message("initial_readiness", readiness_template, 1))
        .await
        .expect("readiness template send should succeed");
    let template_barrier = template_sender
        .reserve()
        .await
        .expect("readiness template barrier should succeed");
    buffer_sender
        .send(Arc::new(readiness_record))
        .await
        .expect("readiness record send should succeed");
    let readiness = receive_records(&mut saistats_receiver, 1).await;
    assert_eq!(readiness[0].1.len(), 1);
    drop(template_barrier);

    // Generate matching records for all templates across all keys
    let records = ipfix_test_helpers::generate_ipfix_records(&all_templates_bytes);
    buffer_sender
        .send(Arc::new(records))
        .await
        .expect("record send should succeed");

    let expected_counts: Vec<usize> = template_defs.iter().map(|(_, _, c)| *c).collect();

    let received = receive_records(&mut saistats_receiver, expected_counts.len()).await;

    assert_eq!(
        received.len(),
        expected_counts.len(),
        "should receive one logical record per template"
    );

    for (i, (observation_time, stats)) in received.iter().enumerate() {
        let expected_count = expected_counts[i];
        let expected_obs_time = (i as u64) + 1;

        assert_eq!(
            *observation_time, expected_obs_time,
            "observation time mismatch for message {}",
            i
        );
        assert_eq!(
            stats.len(),
            expected_count,
            "counter count mismatch for message {}",
            i
        );

        let mut got = stats.clone();
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

            assert_eq!(
                type_id, expected_idx,
                "type_id mismatch at stat {} for message {}",
                idx, i
            );
            assert_eq!(
                stat_id, expected_idx,
                "stat_id mismatch at stat {} for message {}",
                idx, i
            );
            assert_eq!(
                counter,
                expected_obs_time + idx as u64,
                "counter mismatch at stat {} for message {}",
                idx,
                i
            );
        }
    }

    // Deleting one key's templates should cause subsequent data for that key to be dropped
    template_sender
        .send(IPFixTemplatesMessage::delete(delete_key.to_string()))
        .await
        .expect("template delete should succeed");

    let template_barrier = template_sender.reserve().await.unwrap();
    drop(template_barrier);
    // Reuse the already-installed readiness template as a data-channel barrier;
    // installing any new template in the deleted domain must fail closed.
    let barrier_record = ipfix_test_helpers::generate_ipfix_records(
        &ipfix_test_helpers::generate_ipfix_templates(1, 305),
    );
    buffer_sender
        .send(Arc::new(barrier_record))
        .await
        .expect("barrier record send should succeed");

    let barrier = receive_records(&mut saistats_receiver, 1).await;
    assert_eq!(barrier[0].1.len(), 1);

    // A destructive re-add must use both fresh IDs and a fresh domain.
    let readd_template_defs = vec![(delete_key, 307u16, 4usize), (delete_key, 308u16, 6usize)];

    let mut readd_templates_bytes = Vec::new();
    for (_, template_id, counters) in &readd_template_defs {
        let template = ipfix_test_helpers::generate_ipfix_templates(*counters, *template_id);
        let mut template = template;
        template[12..16].copy_from_slice(&1u32.to_be_bytes());
        readd_templates_bytes.extend_from_slice(&template);
    }

    template_sender
        .send(template_message(
            delete_key,
            readd_templates_bytes.clone(),
            readd_template_defs
                .iter()
                .map(|(_, _, counters)| *counters)
                .max()
                .expect("re-added templates are non-empty"),
        ))
        .await
        .expect("template re-add should succeed");
    let template_barrier = template_sender
        .reserve()
        .await
        .expect("template channel should remain open");
    drop(template_barrier);

    let readd_records = ipfix_test_helpers::generate_ipfix_records(&readd_templates_bytes);
    let mut readd_records = readd_records;
    let mut offset = 0usize;
    while offset < readd_records.len() {
        let len =
            u16::from_be_bytes([readd_records[offset + 2], readd_records[offset + 3]]) as usize;
        readd_records[offset + 12..offset + 16].copy_from_slice(&1u32.to_be_bytes());
        offset += len;
    }
    buffer_sender
        .send(Arc::new(readd_records))
        .await
        .expect("record send after re-add should succeed");

    let expected_readd_counts: Vec<usize> =
        readd_template_defs.iter().map(|(_, _, c)| *c).collect();
    let readd_received = receive_records(&mut saistats_receiver, expected_readd_counts.len()).await;

    assert_eq!(
        readd_received.len(),
        expected_readd_counts.len(),
        "should receive one stats message per re-added template"
    );

    for (i, (observation_time, stats)) in readd_received.iter().enumerate() {
        let expected_count = expected_readd_counts[i];
        let expected_obs_time = (i as u64) + 1;

        assert_eq!(
            *observation_time, expected_obs_time,
            "observation time mismatch after re-add for message {}",
            i
        );
        assert_eq!(
            stats.len(),
            expected_count,
            "counter count mismatch after re-add for message {}",
            i
        );

        let mut got = stats.clone();
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

            assert_eq!(
                type_id, expected_idx,
                "type_id mismatch at stat {} after re-add for message {}",
                idx, i
            );
            assert_eq!(
                stat_id, expected_idx,
                "stat_id mismatch at stat {} after re-add for message {}",
                idx, i
            );
            assert_eq!(
                counter,
                expected_obs_time + idx as u64,
                "counter mismatch at stat {} after re-add for message {}",
                idx,
                i
            );
        }
    }

    drop(buffer_sender);
    drop(template_sender);
    drop(saistats_receiver);

    actor_handle
        .await
        .expect("actor task should join")
        .expect_err("actor should report closed input channels");
}

#[tokio::test]
async fn template_defined_counter_widths_reach_sai_stats() {
    let (buffer_sender, buffer_receiver) = channel::<SocketBufferMessage>(1);
    let (template_sender, template_receiver) = channel(1);
    let (saistats_sender, mut saistats_receiver) = channel(1);
    let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
    actor.add_recipient(saistats_sender);
    let actor_handle = tokio::spawn(IpfixActor::run(actor));

    let template = ipfix_test_helpers::generate_ipfix_templates_with_counter_widths(
        &[1, 2, 3, 4, 5, 6, 7, 8],
        400,
    );
    let record = ipfix_test_helpers::generate_ipfix_records(&template);
    template_sender
        .send(template_message("mixed-width", template, 8))
        .await
        .unwrap();
    let barrier = template_sender.reserve().await.unwrap();
    buffer_sender.send(Arc::new(record)).await.unwrap();
    let records = receive_records(&mut saistats_receiver, 1).await;
    drop(barrier);

    assert_eq!(records[0].1.len(), 8);
    assert_eq!(
        records[0]
            .1
            .iter()
            .map(|(_, _, counter)| *counter)
            .collect::<Vec<_>>(),
        vec![1, 2, 3, 4, 5, 6, 7, 8]
    );

    drop(buffer_sender);
    drop(template_sender);
    assert!(actor_handle.await.unwrap().is_err());
}

fn repeat_single_record(mut message: Vec<u8>, count: usize) -> Vec<u8> {
    let record = message[20..].to_vec();
    let set_len = 4 + record.len() * count;
    let message_len = 16 + set_len;
    assert!(message_len <= usize::from(u16::MAX));
    message.truncate(20);
    for observation_time in 1..=count {
        let start = message.len();
        message.extend_from_slice(&record);
        message[start..start + 8]
            .copy_from_slice(&u64::try_from(observation_time).unwrap().to_be_bytes());
    }
    message[2..4].copy_from_slice(&u16::try_from(message_len).unwrap().to_be_bytes());
    message[18..20].copy_from_slice(&u16::try_from(set_len).unwrap().to_be_bytes());
    message
}

#[tokio::test]
async fn reduced_width_records_are_split_at_batch_boundaries() {
    const COUNTERS: usize = 8_000;
    const RECORDS: usize = 8;
    let (buffer_sender, buffer_receiver) = channel::<SocketBufferMessage>(1);
    let (template_sender, template_receiver) = channel(1);
    let (saistats_sender, mut saistats_receiver) = channel(8);
    let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
    actor.add_recipient(saistats_sender);
    let actor_handle = tokio::spawn(IpfixActor::run(actor));

    let template =
        ipfix_test_helpers::generate_ipfix_templates_with_counter_widths(&vec![1; COUNTERS], 400);
    let records = repeat_single_record(
        ipfix_test_helpers::generate_ipfix_records(&template),
        RECORDS,
    );
    template_sender
        .send(template_message("wide", template, COUNTERS))
        .await
        .unwrap();
    let barrier = template_sender.reserve().await.unwrap();
    buffer_sender.send(Arc::new(records)).await.unwrap();

    let records = receive_records(&mut saistats_receiver, RECORDS).await;
    drop(barrier);
    assert_eq!(records.len(), RECORDS);
    assert!(records.iter().all(|(_, stats)| stats.len() == COUNTERS));

    drop(buffer_sender);
    drop(template_sender);
    assert!(actor_handle.await.unwrap().is_err());
}

#[tokio::test]
async fn deferred_reduced_width_records_are_split_at_batch_boundaries() {
    const COUNTERS: usize = 8_000;
    const RECORDS: usize = 8;
    let (buffer_sender, buffer_receiver) = channel::<SocketBufferMessage>(2);
    let (template_sender, template_receiver) = channel(1);
    let (saistats_sender, mut saistats_receiver) = channel(8);
    let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
    actor.add_recipient(saistats_sender);
    let actor_handle = tokio::spawn(IpfixActor::run(actor));

    let template =
        ipfix_test_helpers::generate_ipfix_templates_with_counter_widths(&vec![1; COUNTERS], 400);
    let records = repeat_single_record(
        ipfix_test_helpers::generate_ipfix_records(&template),
        RECORDS,
    );
    buffer_sender.send(Arc::new(records)).await.unwrap();
    tokio::task::yield_now().await;
    template_sender
        .send(template_message("wide", template, COUNTERS))
        .await
        .unwrap();

    let records = receive_records(&mut saistats_receiver, RECORDS).await;
    assert_eq!(records.len(), RECORDS);
    assert!(records.iter().all(|(_, stats)| stats.len() == COUNTERS));
    drop(buffer_sender);
    drop(template_sender);
    assert!(actor_handle.await.unwrap().is_err());
}
