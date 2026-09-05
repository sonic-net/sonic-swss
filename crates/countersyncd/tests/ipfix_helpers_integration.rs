mod ipfix_test_helpers;

use std::sync::Arc;

use tokio::sync::mpsc::{channel, Receiver, Sender};
use tokio::time::{timeout, Duration};

use countersyncd::actor::ipfix::IpfixActor;
use countersyncd::message::{
    buffer::SocketBufferMessage,
    ipfix::{IPFixOwnerUpdate, IPFixTemplatesMessage},
    saistats::SAIStatsBatchMessage,
};
use ipfix_test_helpers::{
    generate_ipfix_records, generate_ipfix_templates, generate_ipfix_templates_with_counter_widths,
    generate_object_metadata,
};

type ReceivedRecord = (u64, Vec<(u32, u32, u64)>);

fn template_message(key: &str, templates: Vec<u8>, counters: usize) -> IPFixTemplatesMessage {
    let (object_names, object_ids) = generate_object_metadata(counters);
    IPFixTemplatesMessage::Owner(IPFixOwnerUpdate::new(
        key.to_string(),
        Arc::new(templates),
        Some(object_names),
        Some(object_ids),
    ))
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
    for remove in [IPFixOwnerUpdate::delete, IPFixOwnerUpdate::deactivate] {
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

        let pending = [
            generate_ipfix_templates(4, 303),
            generate_ipfix_templates(5, 304),
        ]
        .concat();
        apply_template(
            &template_sender,
            template_message("target", pending.clone(), 5),
        )
        .await;
        assert_data(
            &buffer_sender,
            &mut receiver,
            &[&original, &healthy],
            &[(1, 2), (2, 3), (3, 1)],
        )
        .await;
        apply_template(
            &template_sender,
            IPFixTemplatesMessage::Owner(remove("target".to_string())),
        )
        .await;
        assert_data(
            &buffer_sender,
            &mut receiver,
            &[&original, &pending, &healthy],
            &[(5, 1)],
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
        apply_template(
            &template_sender,
            IPFixTemplatesMessage::Owner(remove("target".to_string())),
        )
        .await;
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
async fn first_new_key_switches_the_whole_session_snapshot_regardless_of_counter_identity() {
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
    // Unique matches, entirely changed counter lists, and ambiguous matches must
    // all use the session snapshot, not a semantic old-to-new template pairing.
    for (old_a_count, old_b_count, new_a_count, new_b_count) in
        [(2, 3, 2, 3), (2, 3, 4, 5), (2, 2, 2, 2)]
    {
        let old_a = generate_ipfix_templates(old_a_count, 301);
        let old_b = generate_ipfix_templates(old_b_count, 302);
        let new_a = generate_ipfix_templates_with_counter_widths(&vec![1; new_a_count], 303);
        let new_b = generate_ipfix_templates_with_counter_widths(&vec![4; new_b_count], 304);
        apply_template(
            &template_sender,
            template_message("target", [&old_a[..], &old_b[..]].concat(), 3),
        )
        .await;
        apply_template(
            &template_sender,
            template_message("target", [&new_a[..], &new_b[..]].concat(), 5),
        )
        .await;
        assert_data(
            &buffer_sender,
            &mut receiver,
            &[&old_a, &old_b, &healthy],
            &[(1, old_a_count), (2, old_b_count), (3, 1)],
        )
        .await;
        // Old data before the trigger is accepted; both old keys after it are
        // retired, even within the same channel input and before new B is used.
        assert_data(
            &buffer_sender,
            &mut receiver,
            &[&old_b, &new_a, &old_a, &old_b, &healthy],
            &[(1, old_b_count), (2, new_a_count), (5, 1)],
        )
        .await;
        assert_data(
            &buffer_sender,
            &mut receiver,
            &[&old_a, &old_b, &new_b, &new_a, &healthy],
            &[(3, new_b_count), (4, new_a_count), (5, 1)],
        )
        .await;
        apply_template(
            &template_sender,
            IPFixTemplatesMessage::Owner(IPFixOwnerUpdate::delete("target".to_string())),
        )
        .await;
    }

    drop(buffer_sender);
    drop(template_sender);
    assert!(actor_handle.await.unwrap().is_err());
    assert!(receiver.recv().await.is_none());
}

#[tokio::test]
async fn shared_key_does_not_promote_and_latest_pending_snapshot_replaces_unused_pending() {
    let (buffer_sender, buffer_receiver) = channel::<SocketBufferMessage>(1);
    let (template_sender, template_receiver) = channel(1);
    let (saistats_sender, mut receiver) = channel(4);
    let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
    actor.add_recipient(saistats_sender);
    let actor_handle = tokio::spawn(IpfixActor::run(actor));

    let healthy = generate_ipfix_templates(1, 300);
    let shared = generate_ipfix_templates(2, 301);
    let old = generate_ipfix_templates(3, 302);
    let superseded = generate_ipfix_templates(4, 303);
    let latest_a = generate_ipfix_templates(5, 304);
    let latest_b = generate_ipfix_templates(6, 305);
    apply_template(
        &template_sender,
        template_message("healthy", healthy.clone(), 1),
    )
    .await;
    apply_template(
        &template_sender,
        template_message("target", [&shared[..], &old[..]].concat(), 3),
    )
    .await;
    apply_template(
        &template_sender,
        template_message("target", [&shared[..], &superseded[..]].concat(), 4),
    )
    .await;
    assert_data(
        &buffer_sender,
        &mut receiver,
        &[&shared, &old, &healthy],
        &[(1, 2), (2, 3), (3, 1)],
    )
    .await;
    apply_template(
        &template_sender,
        template_message(
            "target",
            [&shared[..], &latest_a[..], &latest_b[..]].concat(),
            6,
        ),
    )
    .await;
    // The old active snapshot survives a second update. The unused pending
    // snapshot is gone, and neither its data nor shared data can trigger cutover.
    assert_data(
        &buffer_sender,
        &mut receiver,
        &[&superseded, &shared, &old, &healthy],
        &[(2, 2), (3, 3), (4, 1)],
    )
    .await;
    assert_data(
        &buffer_sender,
        &mut receiver,
        &[&latest_a, &old, &superseded, &shared, &healthy],
        &[(1, 5), (4, 2), (5, 1)],
    )
    .await;
    // Even unused latest B became active with A. A subsequent snapshot must
    // retain it until that snapshot's first new-key data arrives.
    let next = generate_ipfix_templates(7, 306);
    apply_template(
        &template_sender,
        template_message("target", next.clone(), 7),
    )
    .await;
    assert_data(
        &buffer_sender,
        &mut receiver,
        &[&latest_b, &latest_a, &shared, &old, &superseded, &healthy],
        &[(1, 6), (2, 5), (3, 2), (6, 1)],
    )
    .await;
    assert_data(
        &buffer_sender,
        &mut receiver,
        &[&next, &latest_a, &latest_b, &shared, &healthy],
        &[(1, 7), (5, 1)],
    )
    .await;

    drop(buffer_sender);
    drop(template_sender);
    assert!(actor_handle.await.unwrap().is_err());
    assert!(receiver.recv().await.is_none());
}

#[tokio::test]
async fn malformed_new_key_data_does_not_emit_or_promote_pending_snapshot() {
    let (buffer_sender, buffer_receiver) = channel::<SocketBufferMessage>(1);
    let (template_sender, template_receiver) = channel(1);
    let (saistats_sender, mut receiver) = channel(4);
    let mut actor = IpfixActor::new(template_receiver, buffer_receiver);
    actor.add_recipient(saistats_sender);
    let actor_handle = tokio::spawn(IpfixActor::run(actor));

    let healthy = generate_ipfix_templates(1, 300);
    let old_a = generate_ipfix_templates(2, 301);
    let old_b = generate_ipfix_templates(3, 302);
    let new_a = generate_ipfix_templates_with_counter_widths(&[1, 2, 3, 4], 303);
    let new_b = generate_ipfix_templates(5, 304);
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
        template_message("target", [&new_a[..], &new_b[..]].concat(), 5),
    )
    .await;

    let mut short_record = generate_ipfix_records(&new_a);
    short_record.pop();
    let message_len = u16::try_from(short_record.len()).unwrap();
    short_record[2..4].copy_from_slice(&message_len.to_be_bytes());
    short_record[18..20].copy_from_slice(&(message_len - 16).to_be_bytes());
    let mut empty_set = generate_ipfix_records(&new_a);
    empty_set.truncate(20);
    empty_set[2..4].copy_from_slice(&20u16.to_be_bytes());
    empty_set[18..20].copy_from_slice(&4u16.to_be_bytes());
    let mut invalid_later_message = generate_ipfix_records(&new_a);
    invalid_later_message.extend_from_slice(&short_record);
    let mut invalid_framing = generate_ipfix_records(&new_a);
    invalid_framing.push(0xff);
    for malformed in [
        short_record,
        empty_set,
        invalid_later_message,
        invalid_framing,
    ] {
        buffer_sender.send(Arc::new(malformed)).await.unwrap();
        // A separate FIFO probe proves the bad input was dropped, without a
        // timing-based assertion, and proves every old key is still active.
        assert_data(
            &buffer_sender,
            &mut receiver,
            &[&old_a, &old_b, &healthy],
            &[(1, 2), (2, 3), (3, 1)],
        )
        .await;
    }
    assert_data(
        &buffer_sender,
        &mut receiver,
        &[&new_a, &old_a, &old_b, &new_b, &healthy],
        &[(1, 4), (4, 5), (5, 1)],
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
