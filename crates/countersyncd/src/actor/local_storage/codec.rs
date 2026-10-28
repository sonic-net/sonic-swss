//! Standard Arrow IPC: each row is one block of a layout, in three non-null
//! fields `layout: UInt32`, `timestamps_ns: List<UInt64>` and
//! `values: List<UInt64>`. Schema metadata `layouts` is a JSON array whose
//! element l is the ordered series array of layout l. With T = len(timestamps_ns)
//! and C = len(layouts[layout]), values[c*T+t] is the raw value of series c at
//! timestamp t. Names occur only in `layouts`, including duplicate tuples.
//! All integers are exact, timestamps are nanoseconds, and there is no app delta.
use super::{MAX_RECORD_BYTES, MAX_ROWS};
use crate::message::saistats::SAIStatsView;
use arrow_array::{Array, ArrayRef, ListArray, RecordBatch, UInt32Array, UInt64Array};
use arrow_buffer::{OffsetBuffer, ScalarBuffer};
use arrow_ipc::{MessageHeader, reader::StreamReader};
use arrow_schema::{ArrowError, DataType, Field, Schema, SchemaRef};
use serde::{Deserialize, Serialize};
use std::{
    collections::HashMap,
    fs::File,
    io::{self, Read, Seek, SeekFrom},
    path::Path,
    sync::Arc,
};

pub(super) const MAX_SCHEMA_BYTES: usize = 16 * 1024 * 1024;
pub(super) const MAX_COUNTERS: usize = 65_536;
pub(super) const MAX_LAYOUTS: usize = 65_536;
pub(super) const MAX_ENCODED_BATCH_BYTES: u64 = (MAX_RECORD_BYTES as u64) * 2 + 64 * 1024 * 1024;

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Series {
    object_name: Arc<str>,
    type_name: Arc<str>,
    stat_name: Arc<str>,
}

fn item() -> Arc<Field> {
    Arc::new(Field::new("item", DataType::UInt64, false))
}

fn fields() -> [Field; 3] {
    [
        Field::new("layout", DataType::UInt32, false),
        Field::new("timestamps_ns", DataType::List(item()), false),
        Field::new("values", DataType::List(item()), false),
    ]
}

/// The stream schema; only the `layouts` JSON array differs between files.
pub(super) fn schema(layouts: String) -> SchemaRef {
    Arc::new(Schema::new_with_metadata(
        fields().to_vec(),
        HashMap::from([
            ("timestamp_unit".into(), "ns".into()),
            ("matrix_order".into(), "series-major".into()),
            ("layouts".into(), layouts),
        ]),
    ))
}

/// Wrap an owned buffer as a list column without copying its values.
pub(super) fn list(values: UInt64Array, offsets: Vec<i32>) -> ArrayRef {
    Arc::new(ListArray::new(
        item(),
        OffsetBuffer::new(ScalarBuffer::from(offsets)),
        Arc::new(values),
        None,
    ))
}

/// One raw observation in block order. Names are shared per file.
#[allow(dead_code)] // The daemon writes; library clients also read.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DecodedSample {
    pub layout: u32,
    pub stat_index: u32,
    pub object_name: Arc<str>,
    pub type_name: Arc<str>,
    pub stat_name: Arc<str>,
    pub observation_time: u64,
    pub value: u64,
}

pub(super) struct Layout {
    // Compared without hashing or allocating names; duplicate identities keep
    // their position.
    identities: Vec<(Arc<str>, u32, u32)>,
    /// The ordered JSON series array embedded in each file's `layouts`.
    pub json: String,
}

impl Layout {
    pub fn width(&self) -> usize {
        self.identities.len()
    }

    pub fn matches(&self, stats: SAIStatsView<'_>) -> bool {
        self.identities.len() == stats.len()
            && self
                .identities
                .iter()
                .zip(stats)
                .all(|((name, ty, id), stat)| {
                    *ty == stat.type_id
                        && *id == stat.stat_id
                        && (Arc::ptr_eq(name, stat.object_name) || name == stat.object_name)
                })
    }

    pub fn new(stats: SAIStatsView<'_>) -> Result<Self, String> {
        if stats.len() > MAX_COUNTERS {
            return Err("record exceeds 65536 counter limit".into());
        }
        // Reject oversized source names before allocating the metadata. Then count
        // actual JSON bytes (including escaping), not a per-counter estimate.
        let names_bytes = stats
            .iter()
            .try_fold(0usize, |n, stat| n.checked_add(stat.object_name.len()))
            .ok_or("schema size overflow")?;
        if names_bytes > MAX_SCHEMA_BYTES {
            return Err("record schema exceeds 16 MiB metadata budget".into());
        }
        let mut series = Vec::with_capacity(stats.len());
        let mut identities = Vec::with_capacity(stats.len());
        for stat in stats {
            // Shared IPFIX views reuse the names cached in their template metadata.
            series.push(Series {
                object_name: Arc::clone(stat.object_name),
                type_name: Arc::from(stat.type_name_or_id()),
                stat_name: Arc::from(stat.stat_name_or_id()),
            });
            identities.push((Arc::clone(stat.object_name), stat.type_id, stat.stat_id));
        }
        struct JsonSize(usize);
        impl io::Write for JsonSize {
            fn write(&mut self, bytes: &[u8]) -> io::Result<usize> {
                self.0 = self
                    .0
                    .checked_add(bytes.len())
                    .filter(|&n| n <= MAX_SCHEMA_BYTES)
                    .ok_or_else(|| {
                        io::Error::other("record schema exceeds 16 MiB metadata budget")
                    })?;
                Ok(bytes.len())
            }
            fn flush(&mut self) -> io::Result<()> {
                Ok(())
            }
        }
        let mut size = JsonSize(0);
        serde_json::to_writer(&mut size, &series).map_err(|e| e.to_string())?;
        let mut json = Vec::with_capacity(size.0);
        serde_json::to_writer(&mut json, &series).map_err(|e| e.to_string())?;
        Ok(Self {
            identities,
            json: String::from_utf8(json).map_err(|e| e.to_string())?,
        })
    }
}

fn validate_schema(schema: &Schema) -> Result<Vec<Vec<Series>>, String> {
    if schema.metadata().get("timestamp_unit").map(String::as_str) != Some("ns")
        || schema.metadata().get("matrix_order").map(String::as_str) != Some("series-major")
    {
        return Err("invalid Arrow storage schema".into());
    }
    let expected = fields();
    if schema.fields().len() != expected.len()
        || schema
            .fields()
            .iter()
            .zip(&expected)
            .any(|(field, expected)| {
                field.name() != expected.name()
                    || field.is_nullable()
                    || !field.metadata().is_empty()
                    || match (field.data_type(), expected.data_type()) {
                        (DataType::List(child), DataType::List(_)) => {
                            child.data_type() != &DataType::UInt64
                                || child.is_nullable()
                                || !child.metadata().is_empty()
                        }
                        (actual, expected) => actual != expected,
                    }
            })
    {
        return Err("invalid Arrow storage fields".into());
    }
    let json = schema
        .metadata()
        .get("layouts")
        .ok_or("missing layouts metadata")?;
    if json.len() > MAX_SCHEMA_BYTES {
        return Err("layouts metadata exceeds 16 MiB".into());
    }
    // Bound element counts before allocating named series, including malformed
    // JSON with millions of tiny entries.
    serde::Deserializer::deserialize_seq(
        &mut serde_json::Deserializer::from_str(json),
        LayoutsCount,
    )
    .map_err(|e| e.to_string())?;
    serde_json::from_str(json).map_err(|e| e.to_string())
}

struct LayoutsCount;

impl<'de> serde::de::Visitor<'de> for LayoutsCount {
    type Value = ();
    fn expecting(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("an array of series arrays")
    }
    fn visit_seq<A: serde::de::SeqAccess<'de>>(self, mut seq: A) -> Result<(), A::Error> {
        let mut count = 0;
        while seq.next_element_seed(SeriesCount)?.is_some() {
            count += 1;
            if count > MAX_LAYOUTS {
                return Err(serde::de::Error::custom("layouts exceed 65536 limit"));
            }
        }
        Ok(())
    }
}

struct SeriesCount;

impl<'de> serde::de::DeserializeSeed<'de> for SeriesCount {
    type Value = ();
    fn deserialize<D: serde::Deserializer<'de>>(self, deserializer: D) -> Result<(), D::Error> {
        deserializer.deserialize_seq(self)
    }
}

impl<'de> serde::de::Visitor<'de> for SeriesCount {
    type Value = ();
    fn expecting(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("an ordered series array")
    }
    fn visit_seq<A: serde::de::SeqAccess<'de>>(self, mut seq: A) -> Result<(), A::Error> {
        let mut count = 0;
        while seq.next_element::<serde::de::IgnoredAny>()?.is_some() {
            count += 1;
            if count > MAX_COUNTERS {
                return Err(serde::de::Error::custom(
                    "series exceeds 65536 counter limit",
                ));
            }
        }
        Ok(())
    }
}

/// One decoded row: T timestamps and C*T series-major values of one layout.
struct Block<'a> {
    layout: usize,
    timestamps: &'a [u64],
    values: &'a [u64],
}

fn validate_batch<'a>(
    batch: &'a RecordBatch,
    layouts: &[Vec<Series>],
) -> Result<Vec<Block<'a>>, String> {
    let rows = batch.num_rows();
    if rows == 0 || batch.num_columns() != 3 {
        return Err("invalid batch dimensions".into());
    }
    let ids = batch
        .column(0)
        .as_any()
        .downcast_ref::<UInt32Array>()
        .ok_or("invalid layout column")?;
    if ids.null_count() != 0 {
        return Err("invalid layout nulls".into());
    }
    let (time_offsets, times) = list_values(batch.column(1))?;
    let (value_offsets, values) = list_values(batch.column(2))?;
    let mut raw = 0usize;
    let mut blocks = Vec::with_capacity(rows);
    for row in 0..rows {
        let layout = ids.value(row) as usize;
        let width = layouts.get(layout).ok_or("invalid layout index")?.len();
        let timestamps = row_values(time_offsets, times, row)?;
        let values = row_values(value_offsets, values, row)?;
        if timestamps.is_empty()
            || timestamps.len() > MAX_ROWS
            || timestamps.len().checked_mul(width) != Some(values.len())
        {
            return Err("invalid block dimensions".into());
        }
        raw = raw.saturating_add(timestamps.len() + values.len());
        blocks.push(Block {
            layout,
            timestamps,
            values,
        });
    }
    if raw
        .checked_mul(8)
        .is_none_or(|bytes| bytes > MAX_RECORD_BYTES)
    {
        return Err("batch exceeds the raw byte limit".into());
    }
    Ok(blocks)
}

fn list_values(column: &ArrayRef) -> Result<(&[i32], &[u64]), String> {
    let list = column
        .as_any()
        .downcast_ref::<ListArray>()
        .ok_or("invalid list type")?;
    let values = list
        .values()
        .as_any()
        .downcast_ref::<UInt64Array>()
        .ok_or("invalid child type")?;
    let offsets = list.value_offsets();
    // Rows cover the child values exactly, from the first value to the last.
    if list.null_count() != 0
        || values.null_count() != 0
        || offsets.first() != Some(&0)
        || offsets.last().and_then(|&end| usize::try_from(end).ok()) != Some(values.len())
    {
        return Err("invalid list offsets or nulls".into());
    }
    Ok((offsets, values.values()))
}

fn row_values<'a>(offsets: &[i32], values: &'a [u64], row: usize) -> Result<&'a [u64], String> {
    let start = usize::try_from(offsets[row]).map_err(|e| e.to_string())?;
    let end = usize::try_from(offsets[row + 1]).map_err(|e| e.to_string())?;
    values
        .get(start..end)
        .ok_or_else(|| "invalid list offsets".into())
}

/// Reuse an array's allocation once Arrow has released every other reference.
/// The contents are kept; callers clear or overwrite them.
pub(super) fn reclaim<T: arrow_array::ArrowPrimitiveType>(
    array: arrow_array::PrimitiveArray<T>,
) -> Vec<T::Native> {
    array
        .into_parts()
        .1
        .into_inner()
        .into_vec()
        .unwrap_or_default()
}

pub(super) struct Counted<R> {
    pub inner: R,
    pub position: u64,
}

impl<R: Read> Read for Counted<R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let read = self.inner.read(buf)?;
        self.position += read as u64;
        Ok(read)
    }
}

// Inspect the standard IPC message envelope before Arrow allocates its body.
// These bounds cover our own crash-damaged files, not arbitrary hostile IPC:
// Arrow's nested FlatBuffer/compression allocations remain a trusted-dir boundary.
fn complete_message(
    file: &mut (impl Read + Seek),
    len: u64,
    position: u64,
    schema: bool,
) -> Result<bool, ArrowError> {
    file.seek(SeekFrom::Start(position))?;
    let mut word = [0; 4];
    let mut prefix = 4;
    if len.saturating_sub(position) < 4 {
        return Ok(false);
    }
    file.read_exact(&mut word)?;
    if word == [255; 4] {
        if len.saturating_sub(position) < 8 {
            return Ok(false);
        }
        file.read_exact(&mut word)?;
        prefix = 8;
    }
    let size = i32::from_le_bytes(word);
    if size == 0 && !schema {
        return Ok(false);
    }
    if size <= 0 || size as usize > MAX_SCHEMA_BYTES + 64 * 1024 {
        return Err(ArrowError::IpcError("invalid IPC metadata length".into()));
    }
    if position + prefix + size as u64 > len {
        return Ok(false);
    }
    let mut metadata = vec![0; size as usize];
    file.read_exact(&mut metadata)?;
    let message =
        arrow_ipc::root_as_message(&metadata).map_err(|e| ArrowError::IpcError(e.to_string()))?;
    let body = message.bodyLength();
    if body < 0 || body as u64 > MAX_ENCODED_BATCH_BYTES {
        return Err(ArrowError::IpcError("invalid IPC body length".into()));
    }
    if schema {
        if message.header_type() != MessageHeader::Schema || body != 0 {
            return Err(ArrowError::IpcError("expected IPC schema header".into()));
        }
    } else {
        let batch = message
            .header_as_record_batch()
            .ok_or_else(|| ArrowError::IpcError("expected IPC record batch".into()))?;
        let nodes = batch
            .nodes()
            .ok_or_else(|| ArrowError::IpcError("missing block nodes".into()))?;
        let buffers = batch
            .buffers()
            .ok_or_else(|| ArrowError::IpcError("missing block buffers".into()))?;
        if nodes.len() != 5 || buffers.len() != 10 {
            return Err(ArrowError::IpcError("invalid IPC batch dimensions".into()));
        }
        // Nodes: layout, timestamps list and child, values list and child.
        let length = |node: usize| usize::try_from(nodes.get(node).length()).unwrap_or(usize::MAX);
        let (rows, times, values) = (length(0), length(2), length(4));
        if rows == 0
            || rows > MAX_LAYOUTS
            || usize::try_from(batch.length()).ok() != Some(rows)
            || length(1) != rows
            || length(3) != rows
            || nodes.iter().any(|node| node.null_count() != 0)
            || times
                .checked_add(values)
                .and_then(|n| n.checked_mul(8))
                .is_none_or(|n| n > MAX_RECORD_BYTES)
        {
            return Err(ArrowError::IpcError(
                "invalid IPC block nodes/raw byte limit".into(),
            ));
        }
        // The raw cap covers u64 data, not Arrow's validity bitmaps, layout ids or
        // offsets. Budget each buffer from the validated nodes, so slack in one
        // cannot fund an oversized bitmap in another.
        let offsets = (rows + 1) * 4;
        let raw_limits = [
            rows.div_ceil(8),
            rows * 4,
            rows.div_ceil(8),
            offsets,
            times.div_ceil(8),
            times * 8,
            rows.div_ceil(8),
            offsets,
            values.div_ceil(8),
            values * 8,
        ];
        for (buffer, limit) in buffers.iter().zip(raw_limits) {
            let offset =
                u64::try_from(buffer.offset()).map_err(|e| ArrowError::IpcError(e.to_string()))?;
            let length =
                u64::try_from(buffer.length()).map_err(|e| ArrowError::IpcError(e.to_string()))?;
            if offset.checked_add(length).is_none_or(|n| n > body as u64) {
                return Err(ArrowError::IpcError("invalid IPC buffer range".into()));
            }
            let raw = if batch.compression().is_some() && length != 0 {
                if length < 8 {
                    return Err(ArrowError::IpcError("invalid compressed buffer".into()));
                }
                let start = position + prefix + size as u64 + offset;
                if start + 8 > len {
                    return Ok(false);
                }
                file.seek(SeekFrom::Start(start))?;
                let mut bytes = [0; 8];
                file.read_exact(&mut bytes)?;
                match i64::from_le_bytes(bytes) {
                    -1 => length - 8,
                    n if n >= 0 => n as u64,
                    _ => return Err(ArrowError::IpcError("invalid compressed length".into())),
                }
            } else {
                length
            };
            if raw > limit as u64 {
                return Err(ArrowError::IpcError(
                    "IPC expanded buffer exceeds node byte limit".into(),
                ));
            }
        }
    }
    Ok(position + prefix + size as u64 + body as u64 <= len)
}

#[cfg(test)]
pub(super) struct Recovery {
    pub boundary: u64,
    pub records: u64,
}

/// Return only the prefix of completely decoded batches, never partial rows.
#[cfg(test)]
pub(super) fn scan(path: &Path) -> Result<Recovery, String> {
    let probe = File::open(path).map_err(|e| e.to_string())?;
    let len = probe.metadata().map_err(|e| e.to_string())?.len();
    let input = File::open(path).map_err(|e| e.to_string())?;
    let mut records = 0;
    let boundary = scan_readers(probe, input, len, |_, blocks| {
        records += blocks
            .iter()
            .map(|b| b.timestamps.len() as u64)
            .sum::<u64>();
        Ok(())
    })?;
    Ok(Recovery { boundary, records })
}

// Never turn an operational read/seek failure into a successful partial read.
// Inspect the source, not Arrow's display string (including wrapped IoErrors).
fn operational_io_error(error: &ArrowError) -> bool {
    let mut source: Option<&(dyn std::error::Error + 'static)> = Some(error);
    while let Some(error) = source {
        if let Some(error) = error.downcast_ref::<io::Error>() {
            return error.kind() != io::ErrorKind::UnexpectedEof;
        }
        source = error.source();
    }
    false
}

fn scan_readers(
    mut probe: impl Read + Seek,
    input: impl Read,
    len: u64,
    mut visit: impl FnMut(&[Vec<Series>], &[Block<'_>]) -> Result<(), String>,
) -> Result<u64, String> {
    let complete = match complete_message(&mut probe, len, 0, true) {
        Ok(complete) => complete,
        Err(ArrowError::IoError(_, error)) if error.kind() == io::ErrorKind::UnexpectedEof => false,
        Err(error) => return Err(error.to_string()),
    };
    if !complete {
        return Ok(0);
    }
    let input = Counted {
        inner: input,
        position: 0,
    };
    let mut reader = StreamReader::try_new(input, None).map_err(|e| e.to_string())?;
    let layouts = validate_schema(&reader.schema())?;
    let mut boundary = reader.get_ref().position;
    loop {
        match complete_message(&mut probe, len, boundary, false) {
            Ok(true) => (),
            Ok(false) => break,
            Err(reason) => {
                if operational_io_error(&reason) {
                    return Err(reason.to_string());
                }
                log::warn!("Ignoring invalid IPC tail: {reason}");
                break;
            }
        }
        match reader.next() {
            Some(Ok(batch)) => {
                let blocks = match validate_batch(&batch, &layouts) {
                    Ok(blocks) => blocks,
                    Err(_) => break,
                };
                visit(&layouts, &blocks)?;
                boundary = reader.get_ref().position;
            }
            Some(Err(reason)) if operational_io_error(&reason) => return Err(reason.to_string()),
            _ => break,
        }
    }
    Ok(boundary)
}

/// Read raw samples block by block, in record/stat order within each block,
/// stopping at an incomplete/invalid tail. Never modifies the file.
/// Operational I/O and visitor errors are returned. Empty records emit no
/// samples; standard Arrow StreamReader exposes their timestamps.
#[allow(dead_code)]
pub fn read_shard(
    path: &Path,
    mut visit: impl FnMut(DecodedSample) -> Result<(), String>,
) -> Result<(), String> {
    let input = File::open(path).map_err(|e| e.to_string())?;
    let len = input.metadata().map_err(|e| e.to_string())?.len();
    let probe = File::open(path).map_err(|e| e.to_string())?;
    scan_readers(probe, input, len, |layouts, blocks| {
        for block in blocks {
            let rows = block.timestamps.len();
            for (row, &observation_time) in block.timestamps.iter().enumerate() {
                for (
                    i,
                    Series {
                        object_name,
                        type_name,
                        stat_name,
                    },
                ) in layouts[block.layout].iter().enumerate()
                {
                    visit(DecodedSample {
                        layout: block.layout as u32,
                        stat_index: i as u32,
                        object_name: Arc::clone(object_name),
                        type_name: Arc::clone(type_name),
                        stat_name: Arc::clone(stat_name),
                        observation_time,
                        value: block.values[i * rows + row],
                    })?;
                }
            }
        }
        Ok(())
    })?;
    Ok(())
}

/// A schema whose layouts are built from the given records.
#[cfg(test)]
pub(super) fn test_schema(layouts: &[&[crate::message::saistats::SAIStat]]) -> SchemaRef {
    let layouts: Vec<_> = layouts
        .iter()
        .map(|stats| Layout::new(SAIStatsView::Owned(stats)).unwrap().json)
        .collect();
    schema(format!("[{}]", layouts.join(",")))
}

/// A batch of `(layout, timestamps, series-major values)` blocks.
#[cfg(test)]
pub(super) fn test_batch(schema: &SchemaRef, blocks: &[(u32, &[u64], &[u64])]) -> RecordBatch {
    let mut ids = Vec::new();
    let (mut times, mut values) = (Vec::new(), Vec::new());
    let (mut time_offsets, mut value_offsets) = (vec![0], vec![0]);
    for (layout, block_times, block_values) in blocks {
        ids.push(*layout);
        times.extend_from_slice(block_times);
        values.extend_from_slice(block_values);
        time_offsets.push(times.len() as i32);
        value_offsets.push(values.len() as i32);
    }
    RecordBatch::try_new(
        Arc::clone(schema),
        vec![
            Arc::new(UInt32Array::from(ids)),
            list(times.into(), time_offsets),
            list(values.into(), value_offsets),
        ],
    )
    .unwrap()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::saistats::SAIStat;
    use arrow_array::types::UInt64Type;
    use arrow_ipc::writer::StreamWriter;

    struct ReadFailure {
        file: File,
        position: u64,
        fail_at: u64,
    }

    impl Read for ReadFailure {
        fn read(&mut self, bytes: &mut [u8]) -> io::Result<usize> {
            if self.position >= self.fail_at {
                return Err(io::Error::from_raw_os_error(libc::EIO));
            }
            let count = bytes.len().min((self.fail_at - self.position) as usize);
            let read = self.file.read(&mut bytes[..count])?;
            self.position += read as u64;
            Ok(read)
        }
    }

    impl Seek for ReadFailure {
        fn seek(&mut self, from: SeekFrom) -> io::Result<u64> {
            self.position = self.file.seek(from)?;
            Ok(self.position)
        }
    }

    fn compressed_writer(schema: &SchemaRef) -> StreamWriter<Vec<u8>> {
        use arrow_ipc::{CompressionType, writer::IpcWriteOptions};
        let options = IpcWriteOptions::default()
            .try_with_compression(Some(CompressionType::ZSTD))
            .unwrap();
        StreamWriter::try_new_with_options(Vec::new(), schema, options).unwrap()
    }

    #[test]
    fn reader_io_errors_abort_scan_without_modifying_partial() {
        let schema = test_schema(&[&[]]);
        let mut writer = StreamWriter::try_new(Vec::new(), &schema).unwrap();
        let mut boundaries = Vec::new();
        for _ in 0..2 {
            writer
                .write(&test_batch(&schema, &[(0, &[u64::MAX], &[])]))
                .unwrap();
            boundaries.push(writer.get_ref().len() as u64);
        }
        writer.finish().unwrap();
        let bytes = writer.into_inner().unwrap();
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("read-error.arrow.partial");
        std::fs::write(&path, &bytes).unwrap();
        let failing = |fail_at| ReadFailure {
            file: File::open(&path).unwrap(),
            position: 0,
            fail_at,
        };
        // Fail after a good batch, in the probe metadata, Arrow metadata, and
        // Arrow body respectively. No successful prefix may escape on EIO.
        let probe_error = scan_readers(
            failing(boundaries[0]),
            File::open(&path).unwrap(),
            bytes.len() as u64,
            |_, _| Ok(()),
        );
        assert!(probe_error.err().unwrap().contains("Input/output error"));
        for fail_at in [boundaries[0], boundaries[1] - 1] {
            let error = scan_readers(
                File::open(&path).unwrap(),
                failing(fail_at),
                bytes.len() as u64,
                |_, _| Ok(()),
            );
            assert!(error.err().unwrap().contains("Input/output error"));
            assert_eq!(std::fs::read(&path).unwrap(), bytes);
        }
        assert_eq!(scan(&path).unwrap().records, 2);
        assert_eq!(std::fs::read(&path).unwrap(), bytes);
    }

    #[test]
    fn reader_classifies_io_sources_not_error_messages() {
        for kind in [
            io::ErrorKind::Other,
            io::ErrorKind::PermissionDenied,
            io::ErrorKind::TimedOut,
        ] {
            let error = ArrowError::IoError(
                "looks like a truncated stream".into(),
                io::Error::from(kind),
            );
            assert!(operational_io_error(&error));
            assert!(operational_io_error(&ArrowError::ExternalError(Box::new(
                error
            ))));
        }
        assert!(!operational_io_error(&ArrowError::from(io::Error::from(
            io::ErrorKind::UnexpectedEof
        ))));
        assert!(!operational_io_error(&ArrowError::IpcError(
            "Input/output error".into()
        )));
    }

    #[test]
    fn layouts_metadata_limits_use_actual_json_bytes_and_explicit_counter_limit() {
        let stats = vec![SAIStat::new("", 1, 0, 0); MAX_COUNTERS + 1];
        assert!(
            Layout::new(SAIStatsView::Owned(&stats))
                .err()
                .unwrap()
                .contains("65536")
        );
        // The file's `layouts` array adds two brackets around a single layout.
        let empty = Layout::new(SAIStatsView::Owned(&stats[..1])).unwrap();
        let name = "x".repeat(MAX_SCHEMA_BYTES - empty.json.len() - 2);
        let layout =
            Layout::new(SAIStatsView::Owned(&[SAIStat::new(name.as_str(), 1, 0, 0)])).unwrap();
        let layouts = schema(format!("[{}]", layout.json));
        assert_eq!(layouts.metadata()["layouts"].len(), MAX_SCHEMA_BYTES);
        assert_eq!(
            validate_schema(&layouts).unwrap()[0][0]
                .object_name
                .as_ref(),
            name
        );
        let longer = format!("{name}x");
        let layout = Layout::new(SAIStatsView::Owned(&[SAIStat::new(
            longer.as_str(),
            1,
            0,
            0,
        )]))
        .unwrap();
        assert!(validate_schema(&schema(format!("[{}]", layout.json))).is_err());
        // JSON escaping, rather than source string size, controls admission.
        let escaped = "\0".repeat(MAX_SCHEMA_BYTES / 6 + 1);
        assert!(
            Layout::new(SAIStatsView::Owned(&[SAIStat::new(
                escaped.as_str(),
                1,
                0,
                0
            )]))
            .is_err()
        );
    }

    #[test]
    fn foreign_stream_fields_are_rejected_and_unchanged() {
        let valid = test_schema(&[&[SAIStat::new("a", 1, 0, 0)]]);
        let fields: Vec<_> = ["timestamps_ns", "record_seq", "values"]
            .into_iter()
            .map(|name| Field::new(name, DataType::List(item()), false))
            .collect();
        let schema = Arc::new(Schema::new_with_metadata(fields, valid.metadata().clone()));
        let mut writer = StreamWriter::try_new(Vec::new(), &schema).unwrap();
        let column = || list(vec![42].into(), vec![0, 1]);
        writer
            .write(&RecordBatch::try_new(schema, vec![column(), column(), column()]).unwrap())
            .unwrap();
        writer.finish().unwrap();
        let bytes = writer.into_inner().unwrap();
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("foreign.arrow");
        std::fs::write(&path, &bytes).unwrap();
        assert_eq!(
            read_shard(&path, |_| panic!("a foreign schema must not emit samples")),
            Err("invalid Arrow storage fields".into())
        );
        assert_eq!(std::fs::read(path).unwrap(), bytes);
    }

    #[test]
    fn rejects_nullable_fields_extra_fields_and_invalid_layouts() {
        let valid = test_schema(&[&[SAIStat::new("a", 1, 0, 0)]]);
        assert!(validate_schema(&valid).is_ok());
        for (column, nullable_child) in [(0, false), (1, false), (2, false), (1, true), (2, true)] {
            let mut fields: Vec<_> = valid.fields().iter().cloned().collect();
            let field = fields[column].as_ref().clone();
            fields[column] = Arc::new(if nullable_child {
                let DataType::List(child) = field.data_type() else {
                    unreachable!();
                };
                let data_type =
                    DataType::List(Arc::new(child.as_ref().clone().with_nullable(true)));
                field.with_data_type(data_type)
            } else {
                field.with_nullable(true)
            });
            let schema = Schema::new_with_metadata(fields, valid.metadata().clone());
            assert_eq!(
                validate_schema(&schema).err().unwrap(),
                "invalid Arrow storage fields",
                "column={column}, nullable_child={nullable_child}",
            );
        }
        let mut fields: Vec<_> = valid.fields().iter().cloned().collect();
        fields.swap(1, 2);
        assert!(
            validate_schema(&Schema::new_with_metadata(fields, valid.metadata().clone())).is_err()
        );
        for (key, value) in [
            ("matrix_order", "time-major"),
            ("timestamp_unit", "ms"),
            ("layouts", "{}"),
            ("layouts", "[{}]"),
            ("layouts", "[[{}]]"),
            (
                "layouts",
                r#"[[{"object_name":"a","type_name":"b","stat_name":"c","stat_index":0}]]"#,
            ),
        ] {
            let mut metadata = valid.metadata().clone();
            metadata.insert(key.into(), value.into());
            let schema = Schema::new_with_metadata(valid.fields().clone(), metadata);
            assert!(validate_schema(&schema).is_err(), "{key}={value}");
            let mut writer = StreamWriter::try_new(Vec::new(), &schema).unwrap();
            writer.finish().unwrap();
            let bytes = writer.into_inner().unwrap();
            assert!(
                scan_readers(
                    io::Cursor::new(&bytes),
                    &bytes[..],
                    bytes.len() as u64,
                    |_, _| Ok(())
                )
                .is_err()
            );
        }
        let mut fields: Vec<_> = valid.fields().iter().cloned().collect();
        fields.push(Arc::new(Field::new("extra", DataType::UInt64, false)));
        assert!(
            validate_schema(&Schema::new_with_metadata(fields, valid.metadata().clone())).is_err()
        );
        for (layouts, limit) in [
            (
                format!("[[{}null]]", "null,".repeat(MAX_COUNTERS)),
                "65536 counter",
            ),
            (format!("[{}[]]", "[],".repeat(MAX_LAYOUTS)), "65536 limit"),
        ] {
            let mut metadata = valid.metadata().clone();
            metadata.insert("layouts".into(), layouts);
            assert!(
                validate_schema(&Schema::new_with_metadata(valid.fields().clone(), metadata))
                    .err()
                    .unwrap()
                    .contains(limit)
            );
        }
    }

    #[test]
    fn block_validation_checks_layouts_dimensions_offsets_and_nulls() {
        let schema = test_schema(&[
            &[SAIStat::new("a", 1, 0, 0)],
            &[SAIStat::new("a", 1, 0, 0), SAIStat::new("b", 1, 0, 0)],
        ]);
        let layouts = validate_schema(&schema).unwrap();
        let batch = test_batch(&schema, &[(1, &[2, 3], &[1, 2, 3, 4]), (0, &[1], &[5])]);
        let blocks = validate_batch(&batch, &layouts).unwrap();
        assert_eq!(blocks.len(), 2);
        assert_eq!(
            (blocks[0].layout, blocks[0].timestamps, blocks[0].values),
            (1, &[2, 3][..], &[1, 2, 3, 4][..])
        );
        assert_eq!(
            (blocks[1].layout, blocks[1].timestamps, blocks[1].values),
            (0, &[1][..], &[5][..])
        );
        let too_many_rows = vec![0; MAX_ROWS + 1];
        for blocks in [
            &[][..],
            &[(0, &[][..], &[][..])][..],
            &[(0, &[1, 2][..], &[3][..])][..],
            &[(0, &[1][..], &[3, 4][..])][..],
            &[(1, &[1][..], &[3][..])][..],
            &[(2, &[1][..], &[3][..])][..],
            &[(0, &too_many_rows[..], &too_many_rows[..])][..],
        ] {
            let batch = test_batch(&schema, blocks);
            assert!(validate_batch(&batch, &layouts).is_err(), "{blocks:?}");
        }
        // Rows must cover the child values exactly.
        let batch = test_batch(&schema, &[(0, &[1], &[3])]);
        let shifted = list(vec![1, 2].into(), vec![1, 2]);
        let columns = vec![
            Arc::clone(batch.column(0)),
            shifted,
            Arc::clone(batch.column(2)),
        ];
        let batch = RecordBatch::try_new(Arc::clone(&schema), columns).unwrap();
        assert!(validate_batch(&batch, &layouts).is_err());
        for [layout, timestamps, values] in [
            [
                Arc::new(UInt32Array::from(vec![None])) as ArrayRef,
                list(vec![1].into(), vec![0, 1]),
                list(vec![3].into(), vec![0, 1]),
            ],
            [
                Arc::new(UInt32Array::from(vec![0])) as ArrayRef,
                Arc::new(ListArray::from_iter_primitive::<UInt64Type, _, _>([
                    None::<Vec<Option<u64>>>,
                ])),
                list(vec![3].into(), vec![0, 1]),
            ],
            [
                Arc::new(UInt32Array::from(vec![0])) as ArrayRef,
                list(vec![1].into(), vec![0, 1]),
                Arc::new(ListArray::from_iter_primitive::<UInt64Type, _, _>([Some(
                    vec![None],
                )])),
            ],
        ] {
            let batch = RecordBatch::try_from_iter([
                ("layout", layout),
                ("timestamps_ns", timestamps),
                ("values", values),
            ])
            .unwrap();
            assert!(validate_batch(&batch, &layouts).is_err());
        }
    }

    #[test]
    fn compressed_raw_limit_block_keeps_valid_suffix() {
        let counters = 4095;
        assert_eq!((counters + 1) * MAX_ROWS * 8, MAX_RECORD_BYTES);
        let schema = test_schema(&[&vec![SAIStat::new("a", 1, 0, 0); counters]]);
        let layouts = validate_schema(&schema).unwrap();
        let mut writer = compressed_writer(&schema);
        let mut starts = Vec::new();
        for (block, rows) in [1, MAX_ROWS, 1].into_iter().enumerate() {
            starts.push(writer.get_ref().len());
            let times = vec![block as u64; rows];
            let values = vec![42; counters * rows];
            writer
                .write(&test_batch(&schema, &[(0, &times, &values)]))
                .unwrap();
        }
        writer.finish().unwrap();
        let mut bytes = writer.into_inner().unwrap();
        for &start in &starts {
            assert!(
                complete_message(
                    &mut io::Cursor::new(&bytes),
                    bytes.len() as u64,
                    start as u64,
                    false,
                )
                .unwrap()
            );
        }
        let mut arrow_count = 0;
        let mut batches = 0;
        for batch in StreamReader::try_new(io::Cursor::new(&bytes), None).unwrap() {
            let batch = batch.unwrap();
            let blocks = validate_batch(&batch, &layouts).unwrap();
            assert_eq!(blocks.len(), 1);
            assert_eq!(blocks[0].timestamps.len(), [1, MAX_ROWS, 1][batches]);
            assert!(
                blocks[0]
                    .timestamps
                    .iter()
                    .all(|&time| time == batches as u64)
            );
            assert!(blocks[0].values.iter().all(|&value| value == 42));
            arrow_count += blocks[0].values.len();
            batches += 1;
        }
        assert_eq!(batches, 3);
        assert_eq!(arrow_count, 16_781_310);
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("raw-limit.arrow.partial");
        std::fs::write(&path, &bytes).unwrap();
        let mut count = 0;
        let mut suffix_count = 0;
        read_shard(&path, |sample| {
            let expected_time = if count < counters {
                0
            } else if count < counters * (MAX_ROWS + 1) {
                1
            } else {
                2
            };
            assert_eq!(sample.observation_time, expected_time);
            assert_eq!(sample.stat_index as usize, count % counters);
            assert_eq!(sample.value, 42);
            count += 1;
            suffix_count += usize::from(sample.observation_time == 2);
            Ok(())
        })
        .unwrap();
        assert_eq!(count, arrow_count);
        assert_eq!(suffix_count, counters);

        let start = starts[1];
        let size = i32::from_le_bytes(bytes[start + 4..start + 8].try_into().unwrap()) as usize;
        let message = arrow_ipc::root_as_message(&bytes[start + 8..start + 8 + size]).unwrap();
        let buffers = message.header_as_record_batch().unwrap().buffers().unwrap();
        assert_eq!(buffers.len(), 10);
        assert!(buffers.iter().all(|buffer| buffer.length() >= 8));
        let body = start + 8 + size;
        let value_start = body + buffers.get(9).offset() as usize;
        let bitmap_start = body + buffers.get(8).offset() as usize;
        let value_bytes = (counters * MAX_ROWS * 8) as i64;
        assert_eq!(
            i64::from_le_bytes(bytes[value_start..value_start + 8].try_into().unwrap()),
            value_bytes
        );
        // Neither one extra u64 nor a bitmap inflated into the data budget may
        // pass preflight, even though the encoded body remains tiny.
        for (offset, oversized) in [
            (value_start, value_bytes + 8),
            (bitmap_start, (counters * MAX_ROWS).div_ceil(8) as i64 + 1),
        ] {
            let original: [u8; 8] = bytes[offset..offset + 8].try_into().unwrap();
            bytes[offset..offset + 8].copy_from_slice(&oversized.to_le_bytes());
            let error = complete_message(
                &mut io::Cursor::new(&bytes),
                bytes.len() as u64,
                start as u64,
                false,
            )
            .unwrap_err();
            assert!(error.to_string().contains("node byte limit"));
            bytes[offset..offset + 8].copy_from_slice(&original);
        }
    }

    #[test]
    fn standard_blocks_have_ten_buffers_and_reject_oversized_expansion() {
        for width in [0, 500, 8000] {
            let stats = vec![SAIStat::new("a", 1, 0, 0); width];
            let schema = test_schema(&[&stats[..], &stats[..width / 2]]);
            let mut writer = compressed_writer(&schema);
            let start = writer.get_ref().len();
            let values: Vec<_> = (0..width * 2).map(|i| i as u64).collect();
            let half = &values[..width / 2 * 2];
            writer
                .write(&test_batch(
                    &schema,
                    &[(0, &[u64::MAX, 0], &values), (1, &[1, 2], half)],
                ))
                .unwrap();
            writer.finish().unwrap();
            let mut bytes = writer.into_inner().unwrap();
            let size = i32::from_le_bytes(bytes[start + 4..start + 8].try_into().unwrap()) as usize;
            let message = arrow_ipc::root_as_message(&bytes[start + 8..start + 8 + size]).unwrap();
            let header = message.header_as_record_batch().unwrap();
            assert_eq!(header.length(), 2);
            assert_eq!(header.nodes().unwrap().len(), 5);
            assert_eq!(header.buffers().unwrap().len(), 10);
            let timestamp_buffer =
                start + 8 + size + header.buffers().unwrap().get(5).offset() as usize;
            assert!(
                complete_message(
                    &mut io::Cursor::new(&bytes),
                    bytes.len() as u64,
                    start as u64,
                    false,
                )
                .unwrap()
            );
            bytes[timestamp_buffer..timestamp_buffer + 8]
                .copy_from_slice(&(MAX_RECORD_BYTES as i64 + 1).to_le_bytes());
            assert!(
                complete_message(
                    &mut io::Cursor::new(&bytes),
                    bytes.len() as u64,
                    start as u64,
                    false,
                )
                .is_err()
            );
        }
    }
}
