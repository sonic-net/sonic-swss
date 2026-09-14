//! Configurable ordered OTLP sender workers. Each lane is a disjoint series
//! shard with one awaited export; lanes overlap I/O on current-thread runtimes.
use super::{OtelActor, OtelActorConfig, OtelActorExportError};
use crate::message::saistats::{
    SAIStat, SAIStatMetadata, SAIStatRef, SAIStatsBatch, SAIStatsBatchMessage, SAIStatsView,
};
use opentelemetry::ExportError;
use std::{sync::Arc, thread};
use tokio::sync::{mpsc, oneshot, watch};

/// Fixed-at-startup concurrency. Changing the shard count requires draining the
/// old pool first; this module intentionally does not auto-resize live streams.
#[derive(Debug, Clone)]
pub struct OtelWorkerConfig {
    pub threads: usize,
    /// Maximum simultaneous exports per worker, implemented as independent,
    /// single-flight lanes rather than overlapping requests for one series.
    pub in_flight_per_worker: usize,
    /// Empty means let the OS schedule workers. Otherwise one CPU ID per thread.
    pub cpu_ids: Vec<usize>,
    /// Bounded input batches per lane (not number of data points).
    pub queue_capacity: usize,
}

impl Default for OtelWorkerConfig {
    fn default() -> Self {
        Self {
            threads: 1,
            in_flight_per_worker: 1,
            cpu_ids: Vec::new(),
            queue_capacity: 8,
        }
    }
}

impl OtelWorkerConfig {
    pub fn validate(&self) -> Result<(), String> {
        if !(1..=64).contains(&self.threads) {
            return Err("OTel worker threads must be in 1..=64".into());
        }
        if !(1..=64).contains(&self.in_flight_per_worker)
            || self.lanes() > 1024
        {
            return Err(
                "OTel in-flight per worker must be in 1..=64, with at most 1024 total lanes".into(),
            );
        }
        if !(1..=64).contains(&self.queue_capacity) {
            return Err("OTel worker queue capacity must be in 1..=64".into());
        }
        if !self.cpu_ids.is_empty() {
            if self.cpu_ids.len() != self.threads {
                return Err("OTel worker CPU list must contain one CPU ID per thread".into());
            }
            let available = core_affinity::get_core_ids().ok_or("cannot determine allowed CPUs")?;
            for (index, id) in self.cpu_ids.iter().enumerate() {
                if self.cpu_ids[..index].contains(id) {
                    return Err(format!("duplicate OTel worker CPU ID {id}"));
                }
                if !available.iter().any(|cpu| cpu.id == *id) {
                    return Err(format!("OTel worker CPU ID {id} is unavailable"));
                }
            }
        }
        Ok(())
    }

    pub fn lanes(&self) -> usize {
        self.threads * self.in_flight_per_worker
    }
}

/// Stable routing independent of timestamp/value. Only call with lanes > 0.
pub fn series_shard(stat: &SAIStat, lanes: usize) -> usize {
    shard_ref(stat.into(), lanes)
}
fn shard_ref(stat: SAIStatRef<'_>, lanes: usize) -> usize {
    assert!(lanes > 0);
    let mut hash = 0xcbf29ce484222325u64;
    for byte in stat
        .object_name
        .bytes()
        .chain(stat.type_id.to_le_bytes())
        .chain(stat.stat_id.to_le_bytes())
    {
        hash = (hash ^ u64::from(byte)).wrapping_mul(0x100000001b3);
    }
    (hash % lanes as u64) as usize
}

pub struct OtelWorkerPool {
    input: mpsc::Receiver<SAIStatsBatchMessage>,
    actor_config: OtelActorConfig,
    workers: OtelWorkerConfig,
    shutdown: Option<oneshot::Sender<()>>,
}

struct CancelOnDrop(watch::Sender<bool>);
impl Drop for CancelOnDrop {
    fn drop(&mut self) {
        let _ = self.0.send(true);
    }
}

fn run_worker(
    worker: usize,
    cpu: Option<usize>,
    config: OtelActorConfig,
    receivers: Vec<mpsc::Receiver<SAIStatsBatchMessage>>,
    mut cancellation: watch::Receiver<bool>,
) -> Result<(), String> {
    if let Some(id) = cpu {
        if !core_affinity::set_for_current(core_affinity::CoreId { id }) {
            return Err(format!("cannot pin OTel worker {worker} to CPU {id}"));
        }
    }
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .map_err(|e| e.to_string())?;
    runtime.block_on(async {
        let lanes = receivers.into_iter().map(|rx| {
            let config = config.clone();
            async move {
                let (shutdown, _) = oneshot::channel();
                let actor = OtelActor::new(rx, config, shutdown)
                    .await
                    .map_err(|e| e.to_string())?;
                // A production pool must never inherit the unordered benchmark
                // environment override. Each lane remains single-flight.
                #[cfg(feature = "benchmark")]
                let actor = {
                    let mut actor = actor;
                    actor.max_in_flight = 1;
                    actor
                };
                actor.run().await.map_err(|e| e.to_string())
            }
        });
        tokio::select! {
            biased;
            _ = async {
                loop {
                    if *cancellation.borrow_and_update() { break; }
                    if cancellation.changed().await.is_err() { break; }
                }
            } => Err("OTel worker cancelled".into()),
            result = futures_util::future::try_join_all(lanes) => result.map(|_| ()),
        }
    })
}

impl OtelWorkerPool {
    pub fn new(
        input: mpsc::Receiver<SAIStatsBatchMessage>,
        actor_config: OtelActorConfig,
        workers: OtelWorkerConfig,
        shutdown: oneshot::Sender<()>,
    ) -> Result<Self, String> {
        workers.validate()?;
        if actor_config.max_counters_per_export == 0 || actor_config.flush_timeout.is_zero() {
            return Err("OTel batch size and flush timeout must be positive".into());
        }
        Ok(Self {
            input,
            actor_config,
            workers,
            shutdown: Some(shutdown),
        })
    }

    pub async fn run(mut self) -> Result<(), Box<dyn ExportError>> {
        let (cancel_tx, cancel_rx) = watch::channel(false);
        let cancel = CancelOnDrop(cancel_tx);
        let (done_tx, mut done_rx) = mpsc::unbounded_channel::<Result<(), String>>();
        let mut senders = Vec::new();
        let mut handles = Vec::new();
        let mut error = None;
        let mut plans = Vec::new();
        for worker in 0..self.workers.threads {
            let mut receivers = Vec::new();
            for _ in 0..self.workers.in_flight_per_worker {
                let (tx, rx) = mpsc::channel(self.workers.queue_capacity);
                senders.push(tx);
                receivers.push(rx);
            }
            let cpu = self.workers.cpu_ids.get(worker).copied();
            let config = self.actor_config.clone();
            let cancellation = cancel_rx.clone();
            let done = done_tx.clone();
            let spawned = thread::Builder::new()
                .name(format!("otel-worker-{worker}"))
                .spawn(move || {
                    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                        run_worker(worker, cpu, config, receivers, cancellation)
                    }))
                    .unwrap_or_else(|_| Err(format!("OTel worker {worker} panicked")));
                    let _ = done.send(result);
                });
            match spawned {
                Ok(handle) => handles.push(handle),
                Err(e) => {
                    error = Some(format!("cannot start OTel worker: {e}"));
                    break;
                }
            }
        }
        drop(done_tx);
        let mut completed = 0;
        if error.is_none() {
            loop {
                tokio::select! {
                    result=done_rx.recv() => {
                        completed+=1;
                        error=Some(result.and_then(Result::err).unwrap_or_else(||"OTel worker stopped while input was open".into()));
                        break;
                    }
                    batch=self.input.recv() => {
                        let Some(batch)=batch else {break};
                        let result=tokio::select! {
                            result=route_batch(&batch,&senders,self.actor_config.max_counters_per_export,&mut plans)=>result,
                            result=done_rx.recv()=>{
                                completed+=1;
                                Err(result.and_then(Result::err).unwrap_or_else(||"OTel worker stopped during routing".into()))
                            }
                        };
                        if let Err(e)=result {error=Some(e);break;}
                    }
                }
            }
        }
        drop(senders); // Normal shutdown: workers flush their tail, await ACKs.
        if error.is_some() {
            let _ = cancel.0.send(true);
        }
        while completed < handles.len() {
            match done_rx.recv().await {
                Some(result) => {
                    completed += 1;
                    if let Err(e) = result {
                        if error.is_none() {
                            error = Some(e);
                            let _ = cancel.0.send(true);
                        }
                    }
                }
                None => {
                    error.get_or_insert_with(|| "missing OTel worker completion".into());
                    break;
                }
            }
        }
        // Join OS threads without blocking the daemon's async runtime thread.
        let joined = tokio::task::spawn_blocking(move || {
            handles
                .into_iter()
                .fold(true, |ok, handle| handle.join().is_ok() && ok)
        })
        .await;
        if !matches!(joined, Ok(true)) {
            error.get_or_insert_with(|| "failed to join OTel workers".into());
        }
        if let Some(shutdown) = self.shutdown.take() {
            let _ = shutdown.send(());
        }
        match error {
            Some(e) => Err(Box::new(OtelActorExportError(e))),
            None => Ok(()),
        }
    }
}

async fn route_batch(
    batch: &SAIStatsBatchMessage,
    senders: &[mpsc::Sender<SAIStatsBatchMessage>],
    limit: usize,
    plans: &mut Vec<RoutePlan>,
) -> Result<(), String> {
    if senders.len() == 1 {
        // Already bounded production upstream batches need no cloning of stats.
        if batch.counter_count() <= limit {
            return senders[0]
                .send(batch.clone())
                .await
                .map_err(|_| "OTel lane closed".into());
        }
    }
    let mut buckets: Vec<Vec<SAIStat>> = (0..senders.len()).map(|_| Vec::new()).collect();
    let mut pending: Vec<SAIStatsBatch> = (0..senders.len())
        .map(|_| SAIStatsBatch::default())
        .collect();
    for record in batch.iter() {
        if let SAIStatsView::Shared { metadata, values } = record.stats {
            // Pin the complete generation in the plan: pointer reuse cannot alias
            // a retired template. Bound cached plans; queued batches own metadata.
            let index = match plans.iter().position(|p| Arc::ptr_eq(&p.source, metadata)) {
                Some(i) => i,
                None => {
                    if plans.len() >= 64 {
                        plans.clear();
                    }
                    let mut indices = vec![Vec::new(); senders.len()];
                    for (i, m) in metadata.iter().enumerate() {
                        indices[shard_ref(
                            (m, 0).into(),
                            senders.len(),
                        )]
                        .push(i);
                    }
                    let lanes = indices
                        .into_iter()
                        .map(|indices| {
                            let meta: Arc<[SAIStatMetadata]> = indices
                                .iter()
                                .map(|&i| metadata[i].clone())
                                .collect::<Vec<_>>()
                                .into();
                            (indices, meta)
                        })
                        .collect();
                    plans.push(RoutePlan {
                        source: metadata.clone(),
                        lanes,
                    });
                    plans.len() - 1
                }
            };
            for (lane, (indices, meta)) in plans[index].lanes.iter().enumerate() {
                if indices.is_empty() {
                    continue;
                }
                if pending[lane].counter_count() + indices.len() > limit
                    && pending[lane].counter_count() > 0
                {
                    senders[lane]
                        .send(Arc::new(std::mem::take(&mut pending[lane])))
                        .await
                        .map_err(|_| "OTel lane closed".to_string())?;
                }
                // Keep record descriptors intact. Large individual records are
                // split into bounded metadata slices, only when required.
                if indices.len() > limit {
                    for (idx, metadata) in indices.chunks(limit).zip(meta.chunks(limit)) {
                        let mut out = SAIStatsBatch::default();
                        out.push_shared_record(
                            record.observation_time,
                            Arc::from(metadata),
                            idx.iter().map(|&i| values[i]),
                        );
                        senders[lane]
                            .send(Arc::new(out))
                            .await
                            .map_err(|_| "OTel lane closed".to_string())?;
                    }
                } else {
                    pending[lane].push_shared_record(
                        record.observation_time,
                        meta.clone(),
                        indices.iter().map(|&i| values[i]),
                    );
                }
            }
            continue;
        }
        let SAIStatsView::Owned(stats) = record.stats else {
            unreachable!()
        };
        // Bound routing buffers to one export-sized batch per lane, plus this
        // chunk, even if an incoming record is larger than the threshold.
        for chunk in stats.chunks(limit) {
            for stat in chunk {
                buckets[series_shard(stat, senders.len())].push(stat.clone());
            }
            for ((bucket, out), sender) in buckets.iter_mut().zip(&mut pending).zip(senders) {
                let mut stats = bucket.drain(..);
                while stats.len() > 0 {
                    let take = (limit - out.counter_count()).min(stats.len());
                    out.push_record(record.observation_time, stats.by_ref().take(take));
                    if out.counter_count() == limit {
                        sender
                            .send(Arc::new(std::mem::take(out)))
                            .await
                            .map_err(|_| "OTel lane closed".to_string())?;
                    }
                }
            }
        }
    }
    for (out, sender) in pending.into_iter().zip(senders) {
        if out.counter_count() > 0 {
            sender
                .send(Arc::new(out))
                .await
                .map_err(|_| "OTel lane closed".to_string())?;
        }
    }
    Ok(())
}

struct RoutePlan {
    source: Arc<[SAIStatMetadata]>,
    lanes: Vec<(Vec<usize>, Arc<[SAIStatMetadata]>)>,
}
