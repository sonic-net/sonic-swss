mod ipfix_test_helpers;

use std::sync::Arc;

use tokio::sync::mpsc::{channel, Receiver, Sender};
use tokio::time::{timeout, Duration};

use countersyncd::actor::ipfix::IpfixActor;
use countersyncd::message::{
    buffer::SocketBufferMessage, ipfix::IPFixTemplatesMessage, saistats::SAIStatsBatchMessage,
};
use ipfix_test_helpers::{
    generate_ipfix_records, generate_ipfix_templates, generate_ipfix_templates_with_counter_widths,
    generate_object_metadata,
};

type ReceivedRecord = (u64, Vec<(u32, u32, u64)>);

fn template_message(key: &str, templates: Vec<u8>, counters: usize) -> IPFixTemplatesMessage {
    let (object_names, object_ids) = generate_object_metadata(counters);
    IPFixTemplatesMessage::new(
        key.to_string(),
        Arc::new(templates),
        Some(object_names),
        Some(object_ids),
    )
}

async fn apply_template(sender: &Sender<IPFixTemplatesMessage>, update: IPFixTemplatesMessage) {
    sender.send(update).await.unwrap();
    // Capacity 1: reservation waits for the preceding update to leave the queue.
    // The actor handles that update synchronously before reading more data.
    drop(sender.reserve().await.unwrap());
}

async fn receive_records(
    receiver: &mut Receiver<SAIStatsBatchMessage>,
    expected_records: usize,
) -> Vec<ReceivedRecord> {
    let mut records = Vec::with_capacity(expected_records);
    while records.len() < expected_records {
        let batch = timeout(Duration::from_secs(2), receiver.recv())
            .await
            .expect("timed out waiting for SAI stats batch")
            .expect("SAI stats channel closed early");
        let mut counters = 0;
        for record in batch.iter() {
            counters += record.stats.len();
            records.push((
                record.observation_time,
                record
                    .stats
                    .iter()
                    .map(|stat| (stat.type_id, stat.stat_id, stat.counter))
                    .collect(),
            ));
        }
        assert_eq!(batch.counter_count(), counters);
    }
    assert_eq!(records.len(), expected_records);
    records
}

async fn assert_data(
    sender: &Sender<SocketBufferMessage>,
    receiver: &mut Receiver<SAIStatsBatchMessage>,
    templates: &[&[u8]],
    expected: &[(u64, usize)],
) {
    sender
        .send(Arc::new(generate_ipfix_records(&templates.concat())))
        .await
        .unwrap();
    let expected: Vec<ReceivedRecord> = expected
        .iter()
        .map(|&(time, counters)| {
            (
                time,
                (0..counters)
                    .map(|index| (index as u32 + 1, index as u32 + 1, time + index as u64))
                    .collect(),
            )
        })
        .collect();
    assert_eq!(receive_records(receiver, expected.len()).await, expected);
}

#[tokio::test]
async fn unknown_data_is_dropped_before_install_and_known_data_keeps_flowing() {
    let (buffer_sender, buffer_receiver) = channel::<SocketBufferMessage>(2);
    let (template_sender, template_receiver) = channel(1);
    let (saistats_sender, mut receiver) = channel(4);
    let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
    actor.add_recipient(saistats_sender);
    let actor_handle = tokio::spawn(IpfixActor::run(actor));

    let healthy = generate_ipfix_templates(1, 300);
    let template = generate_ipfix_templates(2, 301);
    apply_template(
        &template_sender,
        template_message("healthy", healthy.clone(), 1),
    )
    .await;
    buffer_sender
        .send(Arc::new(generate_ipfix_records(&template)))
        .await
        .unwrap();
    // A separate known input on the same domain-0 FIFO proves the unknown data
    // was processed while its template was unavailable, without blocking peers.
    assert_data(&buffer_sender, &mut receiver, &[&healthy], &[(1, 1)]).await;

    apply_template(
        &template_sender,
        template_message("new", template.clone(), 2),
    )
    .await;
    // Installation must not replay the earlier data ahead of this known probe.
    assert_data(&buffer_sender, &mut receiver, &[&healthy], &[(1, 1)]).await;
    assert_data(
        &buffer_sender,
        &mut receiver,
        &[&template, &healthy],
        &[(1, 2), (2, 1)],
    )
    .await;

    drop(buffer_sender);
    drop(template_sender);
    assert!(actor_handle.await.unwrap().is_err());
    assert!(receiver.recv().await.is_none());
}

#[tokio::test]
async fn removal_is_owner_local_and_same_domain_ids_can_be_reinstalled() {
    let (buffer_sender, buffer_receiver) = channel::<SocketBufferMessage>(1);
    let (template_sender, template_receiver) = channel(1);
    let (saistats_sender, mut receiver) = channel(4);
    let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
    actor.add_recipient(saistats_sender);
    let actor_handle = tokio::spawn(IpfixActor::run(actor));

    let healthy = generate_ipfix_templates(1, 300);
    apply_template(
        &template_sender,
        template_message("healthy", healthy.clone(), 1),
    )
    .await;
    for remove in [
        IPFixTemplatesMessage::delete,
        IPFixTemplatesMessage::deactivate,
    ] {
        let original = [
            generate_ipfix_templates(2, 301),
            generate_ipfix_templates(3, 302),
        ]
        .concat();
        apply_template(
            &template_sender,
            template_message("target", original.clone(), 3),
        )
        .await;
        assert_data(
            &buffer_sender,
            &mut receiver,
            &[&original, &healthy],
            &[(1, 2), (2, 3), (3, 1)],
        )
        .await;

        apply_template(&template_sender, remove("target".to_string())).await;
        assert_data(
            &buffer_sender,
            &mut receiver,
            &[&original, &healthy],
            &[(3, 1)],
        )
        .await;

        // Reuse both IDs in domain 0 with changed widths, without replacing the actor.
        let replacement = [
            generate_ipfix_templates_with_counter_widths(&[1, 1], 301),
            generate_ipfix_templates_with_counter_widths(&[4, 4, 4], 302),
        ]
        .concat();
        apply_template(
            &template_sender,
            template_message("target", replacement.clone(), 3),
        )
        .await;
        assert_data(
            &buffer_sender,
            &mut receiver,
            &[&replacement, &healthy],
            &[(1, 2), (2, 3), (3, 1)],
        )
        .await;
        apply_template(&template_sender, remove("target".to_string())).await;
        assert_data(
            &buffer_sender,
            &mut receiver,
            &[&replacement, &healthy],
            &[(3, 1)],
        )
        .await;
    }

    drop(buffer_sender);
    drop(template_sender);
    assert!(actor_handle.await.unwrap().is_err());
    assert!(receiver.recv().await.is_none());
}

#[tokio::test]
async fn active_and_pending_templates_handover_independently() {
    let (buffer_sender, buffer_receiver) = channel::<SocketBufferMessage>(1);
    let (template_sender, template_receiver) = channel(1);
    let (saistats_sender, mut receiver) = channel(4);
    let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
    actor.add_recipient(saistats_sender);
    let actor_handle = tokio::spawn(IpfixActor::run(actor));

    let healthy = generate_ipfix_templates(1, 300);
    let old_a = generate_ipfix_templates(2, 301);
    let old_b = generate_ipfix_templates(3, 302);
    // The helper gives each pair the same ordered object/type/stat identities;
    // different counter counts distinguish A from B despite changed widths.
    let new_a = generate_ipfix_templates_with_counter_widths(&[1, 1], 303);
    let new_b = generate_ipfix_templates_with_counter_widths(&[4, 4, 4], 304);
    apply_template(
        &template_sender,
        template_message("healthy", healthy.clone(), 1),
    )
    .await;
    apply_template(
        &template_sender,
        template_message("target", [&old_a[..], &old_b[..]].concat(), 3),
    )
    .await;
    apply_template(
        &template_sender,
        template_message("target", [&new_a[..], &new_b[..]].concat(), 3),
    )
    .await;
    assert_data(
        &buffer_sender,
        &mut receiver,
        &[&old_a, &old_b, &healthy],
        &[(1, 2), (2, 3), (3, 1)],
    )
    .await;
    // A's first new-ID record retires only old A; old B is still usable.
    assert_data(
        &buffer_sender,
        &mut receiver,
        &[&new_a, &old_a, &old_b, &healthy],
        &[(1, 2), (3, 3), (4, 1)],
    )
    .await;
    assert_data(
        &buffer_sender,
        &mut receiver,
        &[&new_b, &old_a, &old_b, &new_a, &healthy],
        &[(1, 3), (4, 2), (5, 1)],
    )
    .await;

    drop(buffer_sender);
    drop(template_sender);
    assert!(actor_handle.await.unwrap().is_err());
    assert!(receiver.recv().await.is_none());
}

#[tokio::test]
async fn template_defined_counter_widths_reach_sai_stats() {
    let (buffer_sender, buffer_receiver) = channel::<SocketBufferMessage>(1);
    let (template_sender, template_receiver) = channel(1);
    let (saistats_sender, mut receiver) = channel(1);
    let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
    actor.add_recipient(saistats_sender);
    let actor_handle = tokio::spawn(IpfixActor::run(actor));

    let template = generate_ipfix_templates_with_counter_widths(&[1, 2, 3, 4, 5, 6, 7, 8], 400);
    apply_template(
        &template_sender,
        template_message("mixed-width", template.clone(), 8),
    )
    .await;
    assert_data(&buffer_sender, &mut receiver, &[&template], &[(1, 8)]).await;

    drop(buffer_sender);
    drop(template_sender);
    assert!(actor_handle.await.unwrap().is_err());
    assert!(receiver.recv().await.is_none());
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
    let (saistats_sender, mut receiver) = channel(8);
    let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
    actor.add_recipient(saistats_sender);
    let actor_handle = tokio::spawn(IpfixActor::run(actor));

    let template = generate_ipfix_templates_with_counter_widths(&vec![1; COUNTERS], 400);
    let records = repeat_single_record(generate_ipfix_records(&template), RECORDS);
    apply_template(
        &template_sender,
        template_message("wide", template, COUNTERS),
    )
    .await;
    buffer_sender.send(Arc::new(records)).await.unwrap();

    // One record, but not two, fits under the soft batching target.
    for index in 0..RECORDS {
        let batch = timeout(Duration::from_secs(2), receiver.recv())
            .await
            .expect("timed out waiting for split SAI stats batch")
            .expect("SAI stats channel closed early");
        assert_eq!(batch.record_count(), 1);
        assert_eq!(batch.counter_count(), COUNTERS);
        let record = batch.iter().next().unwrap();
        assert_eq!(record.observation_time, (index + 1) as u64);
        assert_eq!(record.stats.len(), COUNTERS);
    }

    drop(buffer_sender);
    drop(template_sender);
    assert!(actor_handle.await.unwrap().is_err());
    assert!(receiver.recv().await.is_none(), "unexpected extra batch");
}
