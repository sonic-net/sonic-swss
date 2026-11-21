//! Parallel encoding. The storage thread numbers each batch and hands it to the
//! lowest-numbered idle encoding thread. One writer thread handles encoded
//! batches and stream closures strictly in number order, so the output does not
//! depend on which thread encoded a batch or when it finished. At most 2N
//! batches are numbered but not yet written, which bounds the encoded batches
//! that wait for an earlier one.

use super::{Batch, Encoder, Writer, Written, recycle_rows};
use arrow_ipc::writer::EncodedData;
use arrow_schema::SchemaRef;
use log::info;
use std::{
    cmp::Reverse,
    collections::{BTreeMap, BinaryHeap},
    sync::{
        Arc, Mutex, PoisonError,
        atomic::{AtomicU64, Ordering},
        mpsc::{self, Receiver, Sender, SyncSender, TryIter},
    },
    thread::{self, JoinHandle},
};

/// Work for the writer thread, in number order.
enum Message {
    Batch {
        generation: u64,
        schema: SchemaRef,
        encoded: EncodedData,
        bound: u64,
        records: usize,
    },
    /// A batch could not be encoded: stop writing.
    Failed(String),
    /// Close the stream of a generation.
    Close(u64),
    /// Reply once every earlier message is handled.
    Sync(Sender<()>),
}

struct Numbered {
    number: u64,
    message: Message,
}

struct Job {
    number: u64,
    batch: Batch,
}

/// What the threads report to the storage thread.
struct Shared {
    failure: Mutex<Option<String>>,
    stopped: Mutex<Option<String>>,
    /// Records the writer discarded after capture stopped.
    discarded: AtomicU64,
    /// The latest generation whose stream reached the size target.
    full_generation: AtomicU64,
    /// Batches encoded by each thread.
    encoded: Vec<AtomicU64>,
}

/// Keep the first reason.
fn report(slot: &Mutex<Option<String>>, reason: String) {
    let mut slot = slot.lock().unwrap_or_else(PoisonError::into_inner);
    slot.get_or_insert(reason);
}

fn reported(slot: &Mutex<Option<String>>) -> Option<String> {
    slot.lock().unwrap_or_else(PoisonError::into_inner).clone()
}

pub(super) struct Pipeline {
    /// A queue per encoding thread; a batch goes only to an idle thread.
    jobs: Vec<SyncSender<Job>>,
    idle: Receiver<usize>,
    ready: BinaryHeap<Reverse<usize>>,
    /// A credit per batch that may be in flight; the writer returns one for
    /// each batch it handles.
    credits: Receiver<()>,
    messages: Option<SyncSender<Numbered>>,
    rows: Receiver<Vec<u64>>,
    shared: Arc<Shared>,
    next_number: u64,
    threads: Vec<JoinHandle<()>>,
}

impl Pipeline {
    pub(super) fn start(writer: Writer, encoders: usize) -> Result<Self, String> {
        let shared = Arc::new(Shared {
            failure: Mutex::new(None),
            stopped: Mutex::new(None),
            discarded: AtomicU64::new(0),
            full_generation: AtomicU64::new(0),
            encoded: (0..encoders).map(|_| AtomicU64::new(0)).collect(),
        });
        // Bounded queues keep backpressure: encoded batches wait for the writer.
        let (messages, received) = mpsc::sync_channel(encoders);
        // One batch being encoded by each thread, and as many encoded ones
        // waiting for the writer or for an earlier batch.
        let (credit, credits) = mpsc::sync_channel(2 * encoders);
        (0..2 * encoders)
            .try_for_each(|_| credit.try_send(()))
            .map_err(|e| e.to_string())?;
        let (idle, announced) = mpsc::channel();
        let (rows, returned) = mpsc::channel();
        let mut pipeline = Self {
            jobs: Vec::with_capacity(encoders),
            idle: announced,
            ready: BinaryHeap::new(),
            credits,
            messages: Some(messages),
            rows: returned,
            shared: Arc::clone(&shared),
            next_number: 0,
            threads: Vec::with_capacity(encoders + 1),
        };
        let writer_shared = Arc::clone(&shared);
        pipeline.spawn("hft-write".into(), move || {
            write_loop(writer, received, credit, &writer_shared)
        })?;
        for index in 0..encoders {
            // A handoff, not a queue: a thread holds at most one batch.
            let (job, jobs) = mpsc::sync_channel(0);
            pipeline.jobs.push(job);
            let encoder = Encoder::new()?;
            let messages = pipeline.messages.clone().ok_or("no writer queue")?;
            let (idle, rows, shared) = (idle.clone(), rows.clone(), Arc::clone(&shared));
            pipeline.spawn(format!("hft-encode-{index}"), move || {
                encode_loop(index, encoder, jobs, idle, messages, rows, &shared)
            })?;
        }
        // Start with every thread ready, so the first batches go to thread 0.
        drop(idle);
        for _ in 0..encoders {
            let index = pipeline.idle.recv().map_err(|_| pipeline.broken())?;
            pipeline.ready.push(Reverse(index));
        }
        Ok(pipeline)
    }

    fn spawn(&mut self, name: String, work: impl FnOnce() + Send + 'static) -> Result<(), String> {
        let thread = thread::Builder::new()
            .name(name)
            .spawn(work)
            .map_err(|e| format!("start local storage thread: {e}"))?;
        self.threads.push(thread);
        Ok(())
    }

    fn take_number(&mut self) -> u64 {
        let number = self.next_number;
        self.next_number += 1;
        number
    }

    /// Hand a batch to the lowest-numbered idle encoding thread, waiting for
    /// one if all are busy, and for the writer if 2N batches are in flight.
    pub(super) fn submit(&mut self, batch: Batch) -> Result<(), String> {
        self.credits.recv().map_err(|_| self.broken())?;
        self.ready.extend(self.idle.try_iter().map(Reverse));
        let index = match self.ready.pop() {
            Some(Reverse(index)) => index,
            None => self.idle.recv().map_err(|_| self.broken())?,
        };
        let number = self.take_number();
        self.jobs[index]
            .send(Job { number, batch })
            .map_err(|_| self.broken())
    }

    /// Close the stream of `generation` after the batches handed over so far.
    pub(super) fn close(&mut self, generation: u64) -> Result<(), String> {
        self.send(Message::Close(generation))
    }

    /// Wait until everything handed over so far is written or discarded.
    pub(super) fn sync(&mut self) -> Result<(), String> {
        let (reply, done) = mpsc::channel();
        self.send(Message::Sync(reply))?;
        done.recv().map_err(|_| self.broken())?;
        self.failure().map_or(Ok(()), Err)
    }

    fn send(&mut self, message: Message) -> Result<(), String> {
        let number = self.take_number();
        let sent = self
            .messages
            .as_ref()
            .is_some_and(|messages| messages.send(Numbered { number, message }).is_ok());
        if sent { Ok(()) } else { Err(self.broken()) }
    }

    fn broken(&self) -> String {
        self.failure()
            .unwrap_or_else(|| "local storage threads stopped".into())
    }

    pub(super) fn failure(&self) -> Option<String> {
        reported(&self.shared.failure)
    }

    pub(super) fn stop_reason(&self) -> Option<String> {
        reported(&self.shared.stopped)
    }

    pub(super) fn full_generation(&self) -> u64 {
        self.shared.full_generation.load(Ordering::Acquire)
    }

    pub(super) fn discarded(&self) -> usize {
        self.shared.discarded.load(Ordering::Acquire) as usize
    }

    /// Row buffers the encoding threads are done with.
    pub(super) fn returned_rows(&self) -> TryIter<'_, Vec<u64>> {
        self.rows.try_iter()
    }

    pub(super) fn encoded_batches(&self) -> Vec<u64> {
        let encoded = self.shared.encoded.iter();
        encoded.map(|count| count.load(Ordering::Relaxed)).collect()
    }
}

impl Drop for Pipeline {
    fn drop(&mut self) {
        // Encoding threads finish their batch and see their queue closed; the
        // writer handles what it received, then sees every sender gone.
        self.jobs.clear();
        self.messages = None;
        for thread in self.threads.drain(..) {
            let _ = thread.join();
        }
        info!(
            "Local storage threads encoded {:?} batches",
            self.encoded_batches()
        );
    }
}

/// Reports a batch whose encoding unwound, so the writer stops at it in order
/// instead of waiting for it.
struct Unfinished<'a> {
    number: Option<u64>,
    messages: &'a SyncSender<Numbered>,
    shared: &'a Shared,
}

impl Drop for Unfinished<'_> {
    fn drop(&mut self) {
        if let Some(number) = self.number {
            let reason = String::from("local storage encoding thread panicked");
            report(&self.shared.failure, reason.clone());
            let message = Message::Failed(reason);
            let _ = self.messages.send(Numbered { number, message });
        }
    }
}

fn encode_loop(
    index: usize,
    mut encoder: Encoder,
    jobs: Receiver<Job>,
    idle: Sender<usize>,
    messages: SyncSender<Numbered>,
    rows: Sender<Vec<u64>>,
    shared: &Shared,
) {
    // Announce each readiness, so dispatch can prefer low-numbered threads.
    if idle.send(index).is_err() {
        return;
    }
    while let Ok(Job { number, batch }) = jobs.recv() {
        let mut unfinished = Unfinished {
            number: Some(number),
            messages: &messages,
            shared,
        };
        let encoded = encoder.encode(&batch);
        for mut block in batch.blocks {
            recycle_rows(&mut block.rows);
            let _ = rows.send(block.rows);
        }
        let message = match encoded {
            Ok(encoded) => {
                shared.encoded[index].fetch_add(1, Ordering::Relaxed);
                Message::Batch {
                    generation: batch.generation,
                    schema: batch.schema,
                    encoded,
                    bound: batch.bound,
                    records: batch.records,
                }
            }
            Err(reason) => {
                report(&shared.failure, reason.clone());
                Message::Failed(reason)
            }
        };
        // Ready before handing the batch over: waiting on the writer queue is
        // not work. A thread that failed never announces, so no batch is lost.
        let ready = !matches!(message, Message::Failed(_)) && idle.send(index).is_ok();
        unfinished.number = None;
        if messages.send(Numbered { number, message }).is_err() || !ready {
            break;
        }
    }
}

fn write_loop(
    mut writer: Writer,
    messages: Receiver<Numbered>,
    credit: SyncSender<()>,
    shared: &Shared,
) {
    let mut waiting = BTreeMap::new();
    let mut next = 0;
    while let Ok(Numbered { number, message }) = messages.recv() {
        waiting.insert(number, message);
        while let Some(message) = waiting.remove(&next) {
            next += 1;
            let batch = matches!(message, Message::Batch { .. });
            if let Err(reason) = handle(&mut writer, message, shared) {
                // Leave the stream as written, like a failed single-threaded writer.
                return report(&shared.failure, reason);
            }
            if batch {
                // The storage thread may number another batch.
                let _ = credit.try_send(());
            }
        }
    }
    // Every sender is gone. Streams are published only when closed, so an
    // abandoned stream stays in staging, as with a single thread.
}

fn handle(writer: &mut Writer, message: Message, shared: &Shared) -> Result<(), String> {
    match message {
        Message::Batch { records, .. } if writer.stopped => {
            shared.discarded.fetch_add(records as u64, Ordering::AcqRel);
        }
        Message::Batch {
            generation,
            schema,
            encoded,
            bound,
            records,
        } => match writer.write(generation, &schema, encoded, bound)? {
            Written::Synced { full: true } => {
                shared.full_generation.store(generation, Ordering::Release);
            }
            Written::Synced { full: false } => {}
            Written::Stopped(reason) => {
                shared.discarded.fetch_add(records as u64, Ordering::AcqRel);
                report(&shared.stopped, reason);
            }
        },
        Message::Failed(reason) => return Err(reason),
        Message::Close(generation) => {
            if writer
                .active
                .as_ref()
                .is_some_and(|active| active.generation == generation)
            {
                writer.publish()?;
            }
        }
        Message::Sync(reply) => {
            let _ = reply.send(());
        }
    }
    Ok(())
}
