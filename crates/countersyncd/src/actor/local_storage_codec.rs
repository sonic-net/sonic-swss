//! Standard Arrow IPC v5: one row per block, two non-null List<UInt64> fields.
//! T = timestamps_ns[0].len(), C = schema metadata `series` JSON array length.
//! values[0][c*T+t] is the raw value for ordered series c at timestamp t.
//! Names occur only in `series`, including duplicate tuples.
//! All integers are exact, timestamps are nanoseconds, and there is no app delta.
use super::{BATCH_TARGET_BYTES, FORMAT_VERSION, MAX_RECORD_BYTES, MAX_ROWS};
use crate::{
    message::saistats::{SAIStat, SAIStatsRef},
    sai::{
        saibuffer::{SaiBufferPoolStat, SaiIngressPriorityGroupStat},
        saiport::SaiPortStat,
        saiqueue::SaiQueueStat,
        saitypes::SaiObjectType,
    },
};
use arrow_array::{Array, ArrayRef, Int32Array, ListArray, RecordBatch, UInt64Array};
use arrow_ipc::{reader::StreamReader, MessageHeader};
use arrow_schema::{ArrowError, DataType, Field, Schema, SchemaRef};
use serde::{Deserialize, Serialize};
use std::{
    collections::HashMap,
    fs::File,
    io::{self, Read, Seek, SeekFrom},
    path::Path,
    sync::Arc,
};

const MAX_SCHEMA_BYTES: usize = 16 * 1024 * 1024;
pub(super) const MAX_COUNTERS: usize = 65_536;
pub(super) const MAX_ENCODED_BATCH_BYTES: u64 = (MAX_RECORD_BYTES as u64) * 2 + 64 * 1024 * 1024;

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Series {
    object_name: Arc<str>,
    type_name: Arc<str>,
    stat_name: Arc<str>,
}

/// Wrap an owned primitive buffer without copying its values. ArrayData's builder
/// lets us use standard list offsets without adding an arrow-buffer dependency.
pub(super) fn list(array: UInt64Array) -> ArrayRef {
    let offsets = Int32Array::from(vec![0, i32::try_from(array.len()).expect("bounded list")]);
    let child = array.to_data();
    let data = child
        .clone()
        .into_builder()
        .data_type(DataType::List(Arc::new(Field::new(
            "item",
            DataType::UInt64,
            false,
        ))))
        .len(1)
        .buffers(vec![offsets.values().inner().clone()])
        .child_data(vec![child])
        .build()
        .expect("valid non-null list");
    Arc::new(ListArray::from(data))
}

/// One raw observation in original record/stat order. Names are shared per file.
#[allow(dead_code)] // The daemon writes; library clients also read.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DecodedSample {
    pub stat_index: u32,
    pub object_name: Arc<str>,
    pub type_name: Arc<str>,
    pub stat_name: Arc<str>,
    pub observation_time: u64,
    pub value: u64,
}

/// Full known SAI names, or unambiguous numeric suffixes for unknown IDs.
pub fn series_names(type_id: u32, stat_id: u32) -> (String, String) {
    let object_type = SaiObjectType::try_from(type_id).ok();
    let type_name = object_type
        .map(|t| t.to_c_name().to_string())
        .unwrap_or_else(|| format!("SAI_OBJECT_TYPE_UNKNOWN_{type_id}"));
    let stat_name = match object_type {
        Some(SaiObjectType::Port) => SaiPortStat::from_u32(stat_id).map(|s| s.to_c_name()),
        Some(SaiObjectType::Queue) => SaiQueueStat::from_u32(stat_id).map(|s| s.to_c_name()),
        Some(SaiObjectType::BufferPool) => {
            SaiBufferPoolStat::from_u32(stat_id).map(|s| s.to_c_name())
        }
        Some(SaiObjectType::IngressPriorityGroup) => {
            SaiIngressPriorityGroupStat::from_u32(stat_id).map(|s| s.to_c_name())
        }
        _ => None,
    }
    .map(str::to_string)
    .unwrap_or_else(|| format!("{type_name}_STAT_UNKNOWN_{stat_id}"));
    (type_name, stat_name)
}

pub(super) struct Layout {
    // Only the current layout is retained. No per-metric hashing or name allocation
    // occurs on the stable-layout path; even duplicate identities retain position.
    identities: Vec<(Arc<str>, u32, u32)>,
    pub schema: SchemaRef,
    pub row_bytes: usize,
    pub batch_rows: usize,
}

impl Layout {
    pub fn matches(&self, record: SAIStatsRef<'_>) -> bool {
        self.identities.len() == record.stats.len()
            && self
                .identities
                .iter()
                .zip(record.stats)
                .all(|((name, ty, id), stat)| {
                    *ty == stat.type_id
                        && *id == stat.stat_id
                        && (Arc::ptr_eq(name, &stat.object_name) || **name == *stat.object_name)
                })
    }

    pub fn new(stats: &[SAIStat]) -> Result<Self, String> {
        if stats.len() > MAX_COUNTERS {
            return Err("record exceeds 65536 counter limit".into());
        }
        let row_bytes = stats
            .len()
            .checked_add(1)
            .and_then(|n| n.checked_mul(8))
            .filter(|&n| n <= MAX_RECORD_BYTES)
            .ok_or("record exceeds 128 MiB raw limit")?;
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
            let (type_name, stat_name) = series_names(stat.type_id, stat.stat_id);
            series.push(Series {
                object_name: Arc::clone(&stat.object_name),
                type_name: type_name.into(),
                stat_name: stat_name.into(),
            });
            identities.push((Arc::clone(&stat.object_name), stat.type_id, stat.stat_id));
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
        let fields = ["timestamps_ns", "values"].map(|name| {
            Field::new(
                name,
                DataType::List(Arc::new(Field::new("item", DataType::UInt64, false))),
                false,
            )
        });
        Ok(Self {
            identities,
            schema: Arc::new(Schema::new_with_metadata(
                fields.to_vec(),
                HashMap::from([
                    ("format_version".into(), FORMAT_VERSION.into()),
                    ("timestamp_unit".into(), "ns".into()),
                    ("matrix_order".into(), "series-major".into()),
                    (
                        "series".into(),
                        String::from_utf8(json).map_err(|e| e.to_string())?,
                    ),
                ]),
            )),
            row_bytes,
            batch_rows: (BATCH_TARGET_BYTES / row_bytes).clamp(1, MAX_ROWS),
        })
    }
}

fn validate_schema(schema: &Schema) -> Result<Vec<Series>, String> {
    let version = schema.metadata().get("format_version").map(String::as_str);
    if version != Some(FORMAT_VERSION) {
        return Err(format!(
            "unsupported Arrow storage format version {}; expected {FORMAT_VERSION}",
            version.unwrap_or("<missing>")
        ));
    }
    if schema.metadata().get("timestamp_unit").map(String::as_str) != Some("ns")
        || schema.metadata().get("matrix_order").map(String::as_str) != Some("series-major")
        || schema.fields().len() != 2
    {
        return Err("invalid Arrow storage schema/version".into());
    }
    for (field, name) in schema.fields().iter().zip(["timestamps_ns", "values"]) {
        if field.name() != name
            || field.is_nullable()
            || !field.metadata().is_empty()
            || !matches!(field.data_type(), DataType::List(child)
                if child.data_type() == &DataType::UInt64 && !child.is_nullable()
                    && child.metadata().is_empty())
        {
            return Err("expected two non-null List<UInt64> fields".into());
        }
    }
    let json = schema
        .metadata()
        .get("series")
        .ok_or("missing series metadata")?;
    if json.len() > MAX_SCHEMA_BYTES {
        return Err("series metadata exceeds 16 MiB".into());
    }
    // Bound element count before allocating named series, including malformed JSON
    // with millions of tiny entries.
    struct SeriesCount;
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
    serde::Deserializer::deserialize_seq(
        &mut serde_json::Deserializer::from_str(json),
        SeriesCount,
    )
    .map_err(|e| e.to_string())?;
    serde_json::from_str(json).map_err(|e| e.to_string())
}

fn validate_batch(batch: &RecordBatch, counters: usize) -> Result<[&UInt64Array; 2], String> {
    if batch.num_rows() != 1 || batch.num_columns() != 2 {
        return Err("invalid batch dimensions or null values".into());
    }
    let mut arrays = Vec::with_capacity(2);
    for column in batch.columns() {
        let list = column
            .as_any()
            .downcast_ref::<ListArray>()
            .ok_or("invalid list type")?;
        let values = list
            .values()
            .as_any()
            .downcast_ref::<UInt64Array>()
            .ok_or("invalid child type")?;
        if list.null_count() != 0
            || values.null_count() != 0
            || list.value_offsets() != [0, i32::try_from(values.len()).map_err(|e| e.to_string())?]
        {
            return Err("invalid list offsets or nulls".into());
        }
        arrays.push(values);
    }
    let rows = arrays[0].len();
    if rows == 0
        || rows > MAX_ROWS
        || rows.checked_mul(counters) != Some(arrays[1].len())
        || counters
            .checked_add(1)
            .and_then(|n| n.checked_mul(rows))
            .and_then(|n| n.checked_mul(8))
            .is_none_or(|n| n > MAX_RECORD_BYTES)
    {
        return Err("invalid matrix dimensions or raw byte limit".into());
    }
    Ok([arrays[0], arrays[1]])
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
    counters: usize,
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
            .ok_or_else(|| ArrowError::IpcError("missing matrix nodes".into()))?;
        let buffers = batch
            .buffers()
            .ok_or_else(|| ArrowError::IpcError("missing matrix buffers".into()))?;
        if batch.length() != 1 || nodes.len() != 4 || buffers.len() != 8 {
            return Err(ArrowError::IpcError("invalid IPC batch dimensions".into()));
        }
        let rows = usize::try_from(nodes.get(1).length()).unwrap_or(usize::MAX);
        if rows == 0
            || rows > MAX_ROWS
            || counters
                .checked_add(1)
                .and_then(|n| n.checked_mul(rows))
                .and_then(|n| n.checked_mul(8))
                .is_none_or(|n| n > MAX_RECORD_BYTES)
            || nodes.iter().enumerate().any(|(i, n)| {
                n.null_count() != 0
                    || usize::try_from(n.length()).ok()
                        != Some(match i {
                            0 | 2 => 1,
                            1 => rows,
                            _ => counters * rows,
                        })
            })
        {
            return Err(ArrowError::IpcError(
                "invalid IPC matrix nodes/raw byte limit".into(),
            ));
        }
        // The raw cap covers u64 data, not Arrow's validity bitmaps or offsets.
        // Budget each buffer from the validated nodes: at most 128 MiB of data,
        // ceil(T/8) + ceil(C*T/8) child validity bytes, two one-byte list validity
        // buffers and 16 offset bytes. Slack cannot fund an oversized bitmap.
        let values = counters * rows;
        let raw_limits = [
            1,
            8,
            rows.div_ceil(8),
            rows * 8,
            1,
            8,
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
    let boundary = scan_readers(probe, input, len, |_, arrays| {
        records += arrays[0].len() as u64;
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
    mut visit: impl FnMut(&[Series], [&UInt64Array; 2]) -> Result<(), String>,
) -> Result<u64, String> {
    let complete = match complete_message(&mut probe, len, 0, true, 0) {
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
    let names = validate_schema(&reader.schema())?;
    let mut boundary = reader.get_ref().position;
    loop {
        match complete_message(&mut probe, len, boundary, false, names.len()) {
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
                let arrays = match validate_batch(&batch, names.len()) {
                    Ok(arrays) => arrays,
                    Err(_) => break,
                };
                visit(&names, arrays)?;
                boundary = reader.get_ref().position;
            }
            Some(Err(reason)) if operational_io_error(&reason) => return Err(reason.to_string()),
            _ => break,
        }
    }
    Ok(boundary)
}

/// Read raw samples in record/stat order, stopping at an incomplete/invalid tail.
/// Never modifies the file. Operational I/O and visitor errors are returned.
/// Empty records emit no samples; standard Arrow StreamReader exposes their lists.
#[allow(dead_code)]
pub fn read_shard(
    path: &Path,
    mut visit: impl FnMut(DecodedSample) -> Result<(), String>,
) -> Result<(), String> {
    let input = File::open(path).map_err(|e| e.to_string())?;
    let len = input.metadata().map_err(|e| e.to_string())?.len();
    let probe = File::open(path).map_err(|e| e.to_string())?;
    scan_readers(probe, input, len, |names, columns| {
        let rows = columns[0].len();
        for row in 0..rows {
            for (
                i,
                Series {
                    object_name,
                    type_name,
                    stat_name,
                },
            ) in names.iter().enumerate()
            {
                visit(DecodedSample {
                    stat_index: i as u32,
                    object_name: Arc::clone(object_name),
                    type_name: Arc::clone(type_name),
                    stat_name: Arc::clone(stat_name),
                    observation_time: columns[0].value(row),
                    value: columns[1].value(i * rows + row),
                })?;
            }
        }
        Ok(())
    })?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
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

    #[test]
    fn reader_io_errors_abort_scan_without_modifying_partial() {
        let layout = Layout::new(&[]).unwrap();
        let mut writer = StreamWriter::try_new(Vec::new(), &layout.schema).unwrap();
        let mut boundaries = Vec::new();
        for _ in 0..2 {
            let batch = RecordBatch::try_new(
                Arc::clone(&layout.schema),
                vec![
                    list(UInt64Array::from(vec![u64::MAX])),
                    list(UInt64Array::from(Vec::<u64>::new())),
                ],
            )
            .unwrap();
            writer.write(&batch).unwrap();
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
    fn series_metadata_limits_use_actual_json_bytes_and_explicit_counter_limit() {
        let stats = vec![SAIStat::new("", 1, 0, 0); MAX_COUNTERS + 1];
        assert!(Layout::new(&stats).err().unwrap().contains("65536"));
        let empty = Layout::new(&stats[..1]).unwrap();
        let overhead = empty.schema.metadata()["series"].len();
        let name = "x".repeat(MAX_SCHEMA_BYTES - overhead);
        let layout = Layout::new(&[SAIStat::new(name.as_str(), 1, 0, 0)]).unwrap();
        assert_eq!(layout.schema.metadata()["series"].len(), MAX_SCHEMA_BYTES);
        assert_eq!(
            validate_schema(&layout.schema).unwrap()[0]
                .object_name
                .as_ref(),
            name
        );
        drop(layout);
        assert!(Layout::new(&[SAIStat::new(format!("{name}x").as_str(), 1, 0, 0)]).is_err());
        // JSON escaping, rather than source string size, controls admission.
        let escaped = "\0".repeat(MAX_SCHEMA_BYTES / 6 + 1);
        assert!(Layout::new(&[SAIStat::new(escaped.as_str(), 1, 0, 0)]).is_err());
    }

    #[test]
    fn v4_three_field_stream_is_explicitly_unsupported_and_unchanged() {
        let layout = Layout::new(&[SAIStat::new("a", 1, 0, 0)]).unwrap();
        let mut metadata = layout.schema.metadata().clone();
        metadata.insert("format_version".into(), "sonic-hft-arrow-v4".into());
        let mut fields: Vec<_> = layout.schema.fields().iter().cloned().collect();
        fields.insert(
            1,
            Arc::new(Field::new(
                "record_seq",
                fields[0].data_type().clone(),
                false,
            )),
        );
        let schema = Schema::new_with_metadata(fields, metadata);
        let mut writer = StreamWriter::try_new(Vec::new(), &schema).unwrap();
        writer
            .write(
                &RecordBatch::try_new(
                    Arc::new(schema),
                    vec![
                        list(vec![42].into()),
                        list(vec![0].into()),
                        list(vec![u64::MAX].into()),
                    ],
                )
                .unwrap(),
            )
            .unwrap();
        writer.finish().unwrap();
        let bytes = writer.into_inner().unwrap();
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("v4.arrow");
        std::fs::write(&path, &bytes).unwrap();
        assert_eq!(read_shard(&path, |_| panic!("v4 must not emit samples")), Err(
            "unsupported Arrow storage format version sonic-hft-arrow-v4; expected sonic-hft-arrow-v5".into()
        ));
        assert_eq!(std::fs::read(path).unwrap(), bytes);
    }

    #[test]
    fn rejects_old_versions_extra_fields_and_invalid_series() {
        let layout = Layout::new(&[SAIStat::new("a", 1, 0, 0)]).unwrap();
        for (key, value) in [
            ("format_version", "sonic-hft-arrow-v3"),
            ("matrix_order", "time-major"),
            ("timestamp_unit", "ms"),
            ("series", "{}"),
            ("series", "[{}]"),
            (
                "series",
                r#"[{"object_name":"a","type_name":"b","stat_name":"c","stat_index":0}]"#,
            ),
        ] {
            let mut metadata = layout.schema.metadata().clone();
            metadata.insert(key.into(), value.into());
            let schema = Schema::new_with_metadata(layout.schema.fields().clone(), metadata);
            assert!(validate_schema(&schema).is_err(), "{key}={value}");
            let mut writer = StreamWriter::try_new(Vec::new(), &schema).unwrap();
            writer.finish().unwrap();
            let bytes = writer.into_inner().unwrap();
            assert!(scan_readers(
                io::Cursor::new(&bytes),
                &bytes[..],
                bytes.len() as u64,
                |_, _| Ok(())
            )
            .is_err());
        }
        let mut fields: Vec<_> = layout.schema.fields().iter().cloned().collect();
        fields.push(Arc::new(Field::new("extra", DataType::UInt64, false)));
        assert!(validate_schema(&Schema::new_with_metadata(
            fields,
            layout.schema.metadata().clone()
        ))
        .is_err());
        let mut metadata = layout.schema.metadata().clone();
        metadata.insert(
            "series".into(),
            format!("[{}null]", "null,".repeat(MAX_COUNTERS)),
        );
        assert!(validate_schema(&Schema::new_with_metadata(
            layout.schema.fields().clone(),
            metadata
        ))
        .err()
        .unwrap()
        .contains("65536"));
    }

    #[test]
    fn matrix_validation_checks_dimensions_offsets_and_nulls() {
        let layout = Layout::new(&[SAIStat::new("a", 1, 0, 0)]).unwrap();
        for (times, values) in [
            (vec![], vec![]),
            (vec![1, 2], vec![3]),
            (vec![1], vec![3, 4]),
        ] {
            let batch = RecordBatch::try_new(
                Arc::clone(&layout.schema),
                vec![list(times.into()), list(values.into())],
            )
            .unwrap();
            assert!(validate_batch(&batch, 1).is_err());
        }
        let offsets = Int32Array::from(vec![1, 2]);
        let data = list(vec![1, 2].into())
            .to_data()
            .into_builder()
            .buffers(vec![offsets.values().inner().clone()])
            .build()
            .unwrap();
        let batch = RecordBatch::try_new(
            Arc::clone(&layout.schema),
            vec![Arc::new(ListArray::from(data)), list(vec![3].into())],
        )
        .unwrap();
        assert!(validate_batch(&batch, 1).is_err());
        use arrow_array::types::UInt64Type;
        for column in [
            ListArray::from_iter_primitive::<UInt64Type, _, _>([None::<Vec<Option<u64>>>]),
            ListArray::from_iter_primitive::<UInt64Type, _, _>([Some(vec![None])]),
        ] {
            let batch = RecordBatch::try_from_iter(vec![
                ("timestamps_ns", Arc::new(column) as ArrayRef),
                ("values", list(vec![3].into())),
            ])
            .unwrap();
            assert!(validate_batch(&batch, 1).is_err());
        }
    }

    #[test]
    fn compressed_raw_limit_block_keeps_valid_suffix() {
        use arrow_ipc::{writer::IpcWriteOptions, CompressionType};
        let counters = 4095;
        assert_eq!((counters + 1) * MAX_ROWS * 8, MAX_RECORD_BYTES);
        let layout = Layout::new(&vec![SAIStat::new("a", 1, 0, 0); counters]).unwrap();
        let options = IpcWriteOptions::default()
            .try_with_compression(Some(CompressionType::ZSTD))
            .unwrap();
        let mut writer =
            StreamWriter::try_new_with_options(Vec::new(), &layout.schema, options).unwrap();
        let mut starts = Vec::new();
        for (block, rows) in [1, MAX_ROWS, 1].into_iter().enumerate() {
            starts.push(writer.get_ref().len());
            let batch = RecordBatch::try_new(
                Arc::clone(&layout.schema),
                vec![
                    list(vec![block as u64; rows].into()),
                    list(vec![42; counters * rows].into()),
                ],
            )
            .unwrap();
            writer.write(&batch).unwrap();
        }
        writer.finish().unwrap();
        let mut bytes = writer.into_inner().unwrap();
        for &start in &starts {
            assert!(complete_message(
                &mut io::Cursor::new(&bytes),
                bytes.len() as u64,
                start as u64,
                false,
                counters,
            )
            .unwrap());
        }
        let mut arrow_count = 0;
        let mut blocks = 0;
        for batch in StreamReader::try_new(io::Cursor::new(&bytes), None).unwrap() {
            let batch = batch.unwrap();
            let [times, values] = validate_batch(&batch, counters).unwrap();
            assert_eq!(times.len(), [1, MAX_ROWS, 1][blocks]);
            assert!(times.values().iter().all(|&time| time == blocks as u64));
            assert!(values.values().iter().all(|&value| value == 42));
            arrow_count += values.len();
            blocks += 1;
        }
        assert_eq!(blocks, 3);
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
        assert_eq!(buffers.len(), 8);
        assert!(buffers.iter().all(|buffer| buffer.length() >= 8));
        let body = start + 8 + size;
        let value_start = body + buffers.get(7).offset() as usize;
        let bitmap_start = body + buffers.get(6).offset() as usize;
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
                counters,
            )
            .unwrap_err();
            assert!(error.to_string().contains("node byte limit"));
            bytes[offset..offset + 8].copy_from_slice(&original);
        }
    }

    #[test]
    fn standard_matrix_has_eight_buffers_and_rejects_oversized_expansion() {
        use arrow_ipc::{writer::IpcWriteOptions, CompressionType};
        for width in [0, 500, 8000] {
            let stats = vec![SAIStat::new("a", 1, 0, 0); width];
            let layout = Layout::new(&stats).unwrap();
            let options = IpcWriteOptions::default()
                .try_with_compression(Some(CompressionType::ZSTD))
                .unwrap();
            let mut writer =
                StreamWriter::try_new_with_options(Vec::new(), &layout.schema, options).unwrap();
            let start = writer.get_ref().len();
            let batch = RecordBatch::try_new(
                Arc::clone(&layout.schema),
                vec![
                    list(vec![u64::MAX, 0].into()),
                    list((0..width * 2).map(|i| i as u64).collect::<Vec<_>>().into()),
                ],
            )
            .unwrap();
            writer.write(&batch).unwrap();
            writer.finish().unwrap();
            let mut bytes = writer.into_inner().unwrap();
            let size = i32::from_le_bytes(bytes[start + 4..start + 8].try_into().unwrap()) as usize;
            let message = arrow_ipc::root_as_message(&bytes[start + 8..start + 8 + size]).unwrap();
            let header = message.header_as_record_batch().unwrap();
            assert_eq!(header.length(), 1);
            assert_eq!(header.nodes().unwrap().len(), 4);
            assert_eq!(header.buffers().unwrap().len(), 8);
            let timestamp_buffer =
                start + 8 + size + header.buffers().unwrap().get(3).offset() as usize;
            assert!(complete_message(
                &mut io::Cursor::new(&bytes),
                bytes.len() as u64,
                start as u64,
                false,
                width
            )
            .unwrap());
            bytes[timestamp_buffer..timestamp_buffer + 8]
                .copy_from_slice(&(MAX_RECORD_BYTES as i64 + 1).to_le_bytes());
            assert!(complete_message(
                &mut io::Cursor::new(&bytes),
                bytes.len() as u64,
                start as u64,
                false,
                width
            )
            .is_err());
        }
    }
}
