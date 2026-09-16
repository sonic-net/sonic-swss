use std::sync::Arc;
use std::time::Duration;

use ahash::{HashMap, HashMapExt};
use log::{debug, error, info, warn};
use swss_common::{CxxString, DbConnector};
use tokio::{select, sync::mpsc::Receiver, time::interval};

use crate::message::saistats::SAIStatsBatchMessage;
use crate::sai::{
    SaiBufferPoolStat, SaiIngressPriorityGroupStat, SaiObjectType, SaiPortStat, SaiQueueStat,
};
use crate::utilities::{record_comm_stats, ChannelLabel};

/// Unix socket path for Redis connection
#[allow(dead_code)] // Used in new() method but Rust may not detect it in all build configurations
const SOCK_PATH: &str = "/var/run/redis/redis.sock";
/// Counter database ID in Redis  
#[allow(dead_code)] // Used in new() method but Rust may not detect it in all build configurations
const COUNTERS_DB_ID: i32 = 2;

/// Unique key for identifying a counter in our local cache
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct CounterKey {
    pub object_name: Arc<str>,
    pub type_id: u32,
    pub stat_id: u32,
}

#[allow(dead_code)] // Methods used in tests and may be used by external code
impl CounterKey {
    pub fn new(object_name: Arc<str>, type_id: u32, stat_id: u32) -> Self {
        Self {
            object_name,
            type_id,
            stat_id,
        }
    }
}

/// Counter information with separate input freshness and value-change flags.
#[derive(Debug, Clone)]
#[allow(dead_code)] // Struct used throughout the code but may not be detected in all configurations
pub struct CounterValue {
    pub counter: u64,
    pub updated: bool,
    /// A sample arrived since the last flush, even if its value did not change.
    pub received: bool,
    pub last_written_value: Option<u64>,
    pub last_written_oid: Option<String>,
    /// Pre-resolved template stat name. Owned/manual inputs use the same view API.
    pub stat_name: Option<&'static str>,
}

#[allow(dead_code)] // Methods used throughout the code but may not be detected in all configurations
impl CounterValue {
    pub fn new(counter: u64) -> Self {
        Self {
            counter,
            updated: true,
            received: true,
            last_written_value: None,
            last_written_oid: None,
            stat_name: None,
        }
    }

    pub fn update(&mut self, counter: u64) {
        self.received = true;
        // Only mark as updated if the value actually changed
        if self.counter != counter {
            self.counter = counter;
            self.updated = true;
        }
        // If value is the same, leave updated flag as-is
    }

    pub fn mark_written(&mut self, oid: &str) {
        self.last_written_value = Some(self.counter);
        self.last_written_oid = Some(oid.to_string());
        self.updated = false;
        self.received = false;
    }

    pub fn has_changed(&self) -> bool {
        match self.last_written_value {
            None => self.updated, // Only if it's updated and never written
            Some(last_value) => self.updated && (self.counter != last_value),
        }
    }
}

/// Configuration for the CounterDBActor
#[derive(Debug)]
#[allow(dead_code)] // Used in initialization but field access may not be detected
pub struct CounterDBConfig {
    /// Write interval - how often to write updated counters to CounterDB
    pub interval: Duration,
}

impl CounterDBConfig {
    /// Create a new config
    pub fn new(interval: Duration) -> Self {
        Self { interval }
    }
}

impl Default for CounterDBConfig {
    fn default() -> Self {
        Self::new(Duration::from_secs(10))
    }
}

/// Actor responsible for writing SAI statistics to CounterDB.
///
/// The CounterDBActor handles:
/// - Receiving SAI statistics messages from IPFIX processor
/// - Maintaining a local cache of counter values
/// - Periodic writing of updated counters to CounterDB
/// - Mapping SAI object types to CounterDB table names
#[allow(dead_code)] // Main struct and fields used throughout but may not be detected in all configurations
pub struct CounterDBActor {
    /// Channel for receiving SAI statistics messages
    stats_receiver: Receiver<SAIStatsBatchMessage>,
    /// Configuration for writing behavior (includes timer)
    config: CounterDBConfig,
    /// Local cache of counter values
    counter_cache: HashMap<CounterKey, CounterValue>,
    /// Counter database connection
    counters_db: DbConnector,
    /// Total records received
    total_messages_received: u64,
    /// Total writes performed
    writes_performed: u64,
}

#[allow(dead_code)] // All methods are used but may not be detected in some build configurations
impl CounterDBActor {
    /// Creates a new CounterDBActor instance.
    ///
    /// # Arguments
    ///
    /// * `stats_receiver` - Channel for receiving SAI statistics messages
    /// * `config` - Configuration for writing behavior
    ///
    /// # Returns
    ///
    /// Result containing a new CounterDBActor instance or an error
    pub fn new(
        stats_receiver: Receiver<SAIStatsBatchMessage>,
        config: CounterDBConfig,
    ) -> Result<Self, Box<dyn std::error::Error>> {
        // Connect to CounterDB
        let counters_db = DbConnector::new_unix(COUNTERS_DB_ID, SOCK_PATH, 0)
            .map_err(|e| format!("Failed to connect to CounterDB: {}", e))?;

        info!(
            "CounterDBActor initialized with interval: {:?}",
            config.interval
        );

        Ok(Self {
            stats_receiver,
            config,
            counter_cache: HashMap::new(),
            counters_db,
            total_messages_received: 0,
            writes_performed: 0,
        })
    }

    /// Runs the actor's main event loop.
    ///
    /// This method processes incoming SAI statistics messages and performs
    /// periodic writes to CounterDB based on the configured interval.
    pub async fn run(mut self) {
        info!("CounterDBActor started");

        // Create timer from config
        let mut write_timer = interval(self.config.interval);

        loop {
            select! {
                // Handle incoming statistics messages
                stats_msg = self.stats_receiver.recv() => {
                    match stats_msg {
                        Some(msg) => {
                            record_comm_stats(
                                ChannelLabel::IpfixToCounterDb,
                                self.stats_receiver.len(),
                            );
                            self.handle_stats_message(msg).await;
                        }
                        None => {
                            info!("CounterDBActor: stats channel closed, shutting down");
                            break;
                        }
                    }
                }

                // Handle periodic write timer
                _ = write_timer.tick() => {
                    self.write_updated_counters().await;
                }
            }
        }

        info!(
            "CounterDBActor shutdown. Total messages: {}, writes: {}",
            self.total_messages_received, self.writes_performed
        );
    }

    /// Handles a received SAI statistics message.
    ///
    /// Updates the local counter cache with new values and marks them as updated.
    async fn handle_stats_message(&mut self, batch: SAIStatsBatchMessage) {
        for msg in batch.iter() {
            self.total_messages_received += 1;

            debug!(
                "Received SAI stats message with {} counters at time {}",
                msg.stats.len(),
                msg.observation_time
            );

            for stat in msg.stats {
                let key =
                    CounterKey::new(Arc::clone(&stat.object_name), stat.type_id, stat.stat_id);

                match self.counter_cache.get_mut(&key) {
                    Some(counter_value) => {
                        // Record receipt even when the counter value is unchanged.
                        counter_value.update(stat.counter);
                    }
                    None => {
                        // Insert new counter
                        let mut value=CounterValue::new(stat.counter);
                        value.stat_name=stat.stat_name();
                        self.counter_cache.insert(key,value);
                    }
                }
            }

            debug!(
                "Updated {} counters in cache (total cached: {})",
                msg.stats.len(),
                self.counter_cache.len()
            );
        }
    }

    /// Revalidates mappings and writes newly received counters to CounterDB.
    async fn write_updated_counters(&mut self) {
        // Consume freshness even on failure: never replay an old sample into a
        // replacement mapping on a later flush without new input.
        let keys_to_update: Vec<_> = self
            .counter_cache
            .iter_mut()
            .filter_map(|(key, value)| {
                std::mem::take(&mut value.received).then(|| key.clone())
            })
            .collect();

        if keys_to_update.is_empty() {
            debug!("No newly received counters to write");
            return;
        }

        info!(
            "Checking {} newly received counters for CounterDB",
            keys_to_update.len()
        );

        let mut successful_writes = 0;
        let mut failed_writes = 0;
        // Cache both successful and failed lookups, only for this flush.
        let mut mappings: HashMap<(u32, Arc<str>), Result<String, String>> = HashMap::new();

        for key in keys_to_update {
            let object_key = (key.type_id, Arc::clone(&key.object_name));
            if !mappings.contains_key(&object_key) {
                let table = SaiObjectType::from_u32(key.type_id)
                    .ok_or_else(|| format!("Unknown SAI object type: {}", key.type_id))
                    .and_then(|object_type| self.get_counter_name_map_table(&object_type));
                let mapping = match table {
                    Ok(table) => self.get_oid_from_name_map(&table, &key.object_name).await,
                    Err(e) => Err(e),
                };
                if mapping.is_err() {
                    // Invalidate all stats of this typed object, but do not make
                    // stale samples eligible for replay when the mapping returns.
                    for (cached_key, value) in &mut self.counter_cache {
                        if cached_key.type_id == key.type_id
                            && cached_key.object_name == key.object_name
                        {
                            value.last_written_oid = None;
                        }
                    }
                }
                mappings.insert(object_key.clone(), mapping);
            }
            let oid = match &mappings[&object_key] {
                Ok(oid) => oid,
                Err(e) => {
                    failed_writes += 1;
                    error!("Failed to resolve counter {:?}: {}", key, e);
                    continue;
                }
            };
            // Get a copy of the value to avoid borrowing issues
            if let Some(value) = self.counter_cache.get(&key).cloned() {
                if value.last_written_value != Some(value.counter)
                    || value.last_written_oid.as_deref() != Some(oid.as_str())
                {
                    match self.write_counter_to_db(&key, &value, oid).await {
                        Ok(()) => {
                            successful_writes += 1;
                            // Mark counter as written in cache
                            if let Some(cached_value) = self.counter_cache.get_mut(&key) {
                                cached_value.mark_written(oid);
                            }
                        }
                        Err(e) => {
                            failed_writes += 1;
                            error!("Failed to write counter {:?}: {}", key, e);
                        }
                    }
                }
            }
        }

        self.writes_performed += 1;

        info!(
            "Write cycle completed: {} successful, {} failed",
            successful_writes, failed_writes
        );

        if failed_writes > 0 {
            warn!("{} counter writes failed", failed_writes);
        }
    }

    /// Writes a single counter to CounterDB.
    async fn write_counter_to_db(
        &mut self,
        key: &CounterKey,
        value: &CounterValue,
        oid: &str,
    ) -> Result<(), Box<dyn std::error::Error>> {
        // Get object type from type_id
        let object_type = SaiObjectType::from_u32(key.type_id)
            .ok_or_else(|| format!("Unknown SAI object type: {}", key.type_id))?;

        // Get the stat name from stat_id
        let stat_name = match value.stat_name {
            Some(name)=>std::borrow::Cow::Borrowed(name),
            None=>std::borrow::Cow::Owned(self.get_stat_name(key.stat_id,&object_type)?),
        };

        // Write to COUNTERS table using hset to update only the specific stat field
        // The correct Redis key format is: COUNTERS:oid (e.g., COUNTERS:oid:0x1000000000013)
        // Use DBConnector::hset to set individual fields without affecting other existing fields
        let counters_key = format!("COUNTERS:{}", oid);
        let counter_value = CxxString::from(value.counter.to_string());

        // Use hset to set only this specific stat field, preserving other fields
        self.counters_db
            .hset(&counters_key, stat_name.as_ref(), &counter_value)
            .map_err(|e| format!("Failed to hset {}:{}: {}", counters_key, stat_name, e))?;

        debug!(
            "Wrote counter {} = {} to {}",
            stat_name, value.counter, counters_key
        );

        Ok(())
    }

    /// Gets the counter name map table name for a given object type.
    fn get_counter_name_map_table(&self, object_type: &SaiObjectType) -> Result<String, String> {
        if *object_type == SaiObjectType::IngressPriorityGroup {
            return Ok("COUNTERS_PG_NAME_MAP".to_string());
        }
        // Extract the type name from the C name (e.g., "SAI_OBJECT_TYPE_PORT" -> "PORT")
        let c_name = object_type.to_c_name();
        if let Some(type_suffix) = c_name.strip_prefix("SAI_OBJECT_TYPE_") {
            Ok(format!("COUNTERS_{}_NAME_MAP", type_suffix))
        } else {
            Err(format!("Invalid SAI object type C name: {}", c_name))
        }
    }

    /// Converts object_name format for counter DB lookup.
    /// In counter_db, composite keys use ':' as separator, but object_name uses '|'.
    /// We need to replace the last '|' with ':' for proper lookup.
    fn convert_object_name_for_lookup(&self, object_name: &str) -> String {
        if let Some(last_pipe_pos) = object_name.rfind('|') {
            let mut converted = object_name.to_string();
            converted.replace_range(last_pipe_pos..=last_pipe_pos, ":");
            converted
        } else {
            object_name.to_string()
        }
    }

    /// Gets the OID from the name map table for a given object name.
    /// Always reads Redis; the caller caches the result only within one flush.
    async fn get_oid_from_name_map(
        &mut self,
        table_name: &str,
        object_name: &str,
    ) -> Result<String, String> {
        // Convert object_name format for lookup
        let lookup_name = self.convert_object_name_for_lookup(object_name);

        debug!(
            "Looking up OID for object '{}' in table '{}' (lookup_name: '{}')",
            object_name, table_name, lookup_name
        );

        // For COUNTERS_PORT_NAME_MAP, the data is stored in Redis as:
        // Key: "COUNTERS_PORT_NAME_MAP", Hash fields: "Ethernet0", "Ethernet16", etc.
        // Hash values: "oid:0x1000000000013", "oid:0x100000000001b", etc.
        // Use DBConnector::hget to perform: HGET COUNTERS_PORT_NAME_MAP Ethernet0

        debug!("Performing HGET: {} {}", table_name, lookup_name);
        let oid_result = self
            .counters_db
            .hget(table_name, &lookup_name)
            .map_err(|e| format!("Failed to hget {}:{}: {}", table_name, lookup_name, e))?;

        debug!(
            "HGET result for {}:{}: {:?}",
            table_name, lookup_name, oid_result
        );

        match oid_result {
            Some(oid_value) => {
                // Convert CxxString to Rust String
                let oid = oid_value.to_string_lossy().to_string();
                debug!("Found OID for {}: {}", lookup_name, oid);

                Ok(oid)
            }
            None => {
                let error_msg = format!("Object {} not found in name map", lookup_name);
                debug!("{}", error_msg);
                Err(error_msg)
            }
        }
    }

    /// Gets the stat name from stat_id and object type.
    fn get_stat_name(&self, stat_id: u32, object_type: &SaiObjectType) -> Result<String, String> {
        match object_type {
            SaiObjectType::Port => {
                // Convert stat_id to SaiPortStat and get its C name
                if let Some(port_stat) = SaiPortStat::from_u32(stat_id) {
                    Ok(port_stat.to_c_name().to_string())
                } else {
                    Err(format!("Unknown port stat ID: {}", stat_id))
                }
            }
            SaiObjectType::Queue => {
                // Convert stat_id to SaiQueueStat and get its C name
                if let Some(queue_stat) = SaiQueueStat::from_u32(stat_id) {
                    Ok(queue_stat.to_c_name().to_string())
                } else {
                    Err(format!("Unknown queue stat ID: {}", stat_id))
                }
            }
            SaiObjectType::BufferPool => {
                // Convert stat_id to SaiBufferPoolStat and get its C name
                if let Some(buffer_stat) = SaiBufferPoolStat::from_u32(stat_id) {
                    Ok(buffer_stat.to_c_name().to_string())
                } else {
                    Err(format!("Unknown buffer pool stat ID: {}", stat_id))
                }
            }
            SaiObjectType::IngressPriorityGroup => {
                // Convert stat_id to SaiIngressPriorityGroupStat and get its C name
                if let Some(ipg_stat) = SaiIngressPriorityGroupStat::from_u32(stat_id) {
                    Ok(ipg_stat.to_c_name().to_string())
                } else {
                    Err(format!(
                        "Unknown ingress priority group stat ID: {}",
                        stat_id
                    ))
                }
            }
            _ => Err(format!(
                "Unsupported object type for stat name: {:?}",
                object_type
            )),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::message::saistats::{SAIStat, SAIStats, SAIStatsBatch};
    use crate::sai::saitypes::SaiObjectType;
    use std::sync::Arc;
    use tokio::sync::mpsc;

    fn redis_actor(test_name: &str) -> (CounterDBActor, String) {
        let (_tx, rx) = mpsc::channel(1);
        let actor = CounterDBActor::new(rx, CounterDBConfig::default())
            .expect("CounterDB tests require Redis at /var/run/redis/redis.sock");
        let nonce = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        (actor, format!("test_{test_name}_{}_{nonce}", std::process::id()))
    }

    fn counter_batch(name: &str, type_id: u32, values: &[(u32, u64)]) -> SAIStatsBatchMessage {
        let mut batch = SAIStatsBatch::default();
        batch.push_record(1, values.iter().map(|&(stat_id, value)| {
            SAIStat::new(name, type_id, stat_id, value)
        }));
        Arc::new(batch)
    }

    #[tokio::test]
    async fn test_redis_queue_and_pg_same_name_use_distinct_oids() {
        let (mut actor, prefix) = redis_actor("typed_maps");
        let name = format!("{prefix}|0");
        let lookup = format!("{prefix}:0");
        let queue_oid = format!("oid:{prefix}_queue");
        let pg_oid = format!("oid:{prefix}_pg");
        let queue_key = format!("COUNTERS:{queue_oid}");
        let pg_key = format!("COUNTERS:{pg_oid}");
        for (table, oid) in [
            ("COUNTERS_QUEUE_NAME_MAP", &queue_oid),
            ("COUNTERS_PG_NAME_MAP", &pg_oid),
        ] {
            actor.counters_db.hset(table, &lookup, &CxxString::from(oid.as_str())).unwrap();
        }
        for (object_type, value) in [
            (SaiObjectType::Queue, 10),
            (SaiObjectType::IngressPriorityGroup, 20),
        ] {
            actor.handle_stats_message(counter_batch(&name, object_type.to_u32(), &[(0, value), (1, value * 10)])).await;
        }
        actor.write_updated_counters().await;
        for (key, stat, expected) in [
            (&queue_key, "SAI_QUEUE_STAT_PACKETS", "10"),
            (&queue_key, "SAI_QUEUE_STAT_BYTES", "100"),
            (&pg_key, "SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS", "20"),
            (&pg_key, "SAI_INGRESS_PRIORITY_GROUP_STAT_BYTES", "200"),
        ] {
            assert_eq!(actor.counters_db.hget(key, stat).unwrap().unwrap().to_string_lossy(), expected);
        }
        assert!(actor.counters_db.hget(&queue_key, "SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS").unwrap().is_none());
        assert!(actor.counters_db.hget(&pg_key, "SAI_QUEUE_STAT_PACKETS").unwrap().is_none());
        for table in ["COUNTERS_QUEUE_NAME_MAP", "COUNTERS_PG_NAME_MAP"] {
            actor.counters_db.hdel(table, &lookup).unwrap();
        }
        for key in [&queue_key, &pg_key] {
            actor.counters_db.del(key).unwrap();
        }
    }

    #[tokio::test]
    async fn test_redis_remap_same_and_changed_values_requires_fresh_samples() {
        for object_type in [SaiObjectType::Queue, SaiObjectType::IngressPriorityGroup] {
            for replacement_value in [100, 200] {
                let (mut actor, prefix) = redis_actor("remap");
                let name = format!("{prefix}|0");
                let lookup = format!("{prefix}:0");
                let type_id = object_type.to_u32();
                let table = actor.get_counter_name_map_table(&object_type).unwrap();
                let stat = actor.get_stat_name(0, &object_type).unwrap();
                let stale_stat = actor.get_stat_name(1, &object_type).unwrap();
                let oids = [format!("oid:{prefix}_a"), format!("oid:{prefix}_b"), format!("oid:{prefix}_c")];
                let keys = oids.each_ref().map(|oid| format!("COUNTERS:{oid}"));
                actor.counters_db.hset(&table, &lookup, &CxxString::from(oids[0].as_str())).unwrap();
                actor.handle_stats_message(counter_batch(&name, type_id, &[(0, 100), (1, 1000)])).await;
                actor.write_updated_counters().await;
                assert_eq!(actor.counters_db.hget(&keys[0], &stat).unwrap().unwrap().to_string_lossy(), "100");

                // An equal fresh sample for the same OID must not issue HSET.
                actor.counters_db.hdel(&keys[0], &stat).unwrap();
                actor.handle_stats_message(counter_batch(&name, type_id, &[(0, 100)])).await;
                actor.write_updated_counters().await;
                assert!(actor.counters_db.hget(&keys[0], &stat).unwrap().is_none());
                actor.counters_db.hset(&keys[0], &stat, &CxxString::from("100")).unwrap();

                actor.counters_db.hset(&table, &lookup, &CxxString::from(oids[1].as_str())).unwrap();
                actor.write_updated_counters().await;
                assert!(!actor.counters_db.exists(&keys[1]).unwrap(), "remap without input must not publish cached counters");
                actor.handle_stats_message(counter_batch(&name, type_id, &[(0, replacement_value)])).await;
                actor.write_updated_counters().await;
                assert_eq!(actor.counters_db.hget(&keys[1], &stat).unwrap().unwrap().to_string_lossy(), replacement_value.to_string());
                assert_eq!(actor.counters_db.hget(&keys[0], &stat).unwrap().unwrap().to_string_lossy(), "100");
                assert!(actor.counters_db.hget(&keys[1], &stale_stat).unwrap().is_none(), "a fresh sibling stat must not revive stale input");

                actor.counters_db.hdel(&table, &lookup).unwrap();
                actor.handle_stats_message(counter_batch(&name, type_id, &[(0, replacement_value + 1)])).await;
                actor.write_updated_counters().await;
                assert_eq!(actor.counters_db.hget(&keys[1], &stat).unwrap().unwrap().to_string_lossy(), replacement_value.to_string(), "missing map must not fall back to the old OID");
                actor.counters_db.hset(&table, &lookup, &CxxString::from(oids[2].as_str())).unwrap();
                actor.write_updated_counters().await;
                assert!(!actor.counters_db.exists(&keys[2]).unwrap(), "failed samples must not be replayed after recreation");
                actor.handle_stats_message(counter_batch(&name, type_id, &[(0, replacement_value + 1)])).await;
                actor.write_updated_counters().await;
                assert_eq!(actor.counters_db.hget(&keys[2], &stat).unwrap().unwrap().to_string_lossy(), (replacement_value + 1).to_string());

                // Observed deletion also invalidates dedup if the OID is reused.
                actor.counters_db.hdel(&table, &lookup).unwrap();
                actor.counters_db.del(&keys[2]).unwrap();
                actor.handle_stats_message(counter_batch(&name, type_id, &[(0, replacement_value + 1)])).await;
                actor.write_updated_counters().await;
                actor.counters_db.hset(&table, &lookup, &CxxString::from(oids[2].as_str())).unwrap();
                actor.write_updated_counters().await;
                assert!(!actor.counters_db.exists(&keys[2]).unwrap());
                actor.handle_stats_message(counter_batch(&name, type_id, &[(0, replacement_value + 1)])).await;
                actor.write_updated_counters().await;
                assert_eq!(actor.counters_db.hget(&keys[2], &stat).unwrap().unwrap().to_string_lossy(), (replacement_value + 1).to_string());

                actor.counters_db.hdel(&table, &lookup).unwrap();
                for key in keys {
                    actor.counters_db.del(&key).unwrap();
                }
            }
        }
    }

    #[tokio::test]
    async fn test_redis_missing_map_invalidates_stale_sibling_dedup() {
        for object_type in [SaiObjectType::Queue, SaiObjectType::IngressPriorityGroup] {
            let (mut actor, prefix) = redis_actor("sibling_dedup");
            let name = format!("{prefix}|0");
            let lookup = format!("{prefix}:0");
            let oid = format!("oid:{prefix}");
            let counters_key = format!("COUNTERS:{oid}");
            let type_id = object_type.to_u32();
            let table = actor.get_counter_name_map_table(&object_type).unwrap();
            let packets = actor.get_stat_name(0, &object_type).unwrap();
            let bytes = actor.get_stat_name(1, &object_type).unwrap();

            actor.counters_db.hset(&table, &lookup, &CxxString::from(oid.as_str())).unwrap();
            actor.handle_stats_message(counter_batch(&name, type_id, &[(0, 10), (1, 100)])).await;
            actor.write_updated_counters().await;
            assert_eq!(actor.counters_db.hget(&counters_key, &packets).unwrap().unwrap().to_string_lossy(), "10");
            assert_eq!(actor.counters_db.hget(&counters_key, &bytes).unwrap().unwrap().to_string_lossy(), "100");

            // Only packets are fresh when the missing mapping is observed.
            actor.counters_db.hdel(&table, &lookup).unwrap();
            actor.counters_db.del(&counters_key).unwrap();
            actor.handle_stats_message(counter_batch(&name, type_id, &[(0, 20)])).await;
            actor.write_updated_counters().await;
            let byte_key = CounterKey::new(name.as_str().into(), type_id, 1);
            let cached_bytes = &actor.counter_cache[&byte_key];
            assert_eq!(cached_bytes.last_written_oid, None);
            assert_eq!(cached_bytes.last_written_value, Some(100));
            assert!(!cached_bytes.received);
            assert!(!cached_bytes.updated);
            assert!(!actor.counters_db.exists(&counters_key).unwrap());

            actor.counters_db.hset(&table, &lookup, &CxxString::from(oid.as_str())).unwrap();
            actor.write_updated_counters().await;
            assert!(!actor.counters_db.exists(&counters_key).unwrap(), "recreation alone must not replay either stat");
            actor.handle_stats_message(counter_batch(&name, type_id, &[(1, 100)])).await;
            actor.write_updated_counters().await;
            assert_eq!(actor.counters_db.hget(&counters_key, &bytes).unwrap().unwrap().to_string_lossy(), "100");
            assert!(actor.counters_db.hget(&counters_key, &packets).unwrap().is_none(), "fresh bytes must not replay stale packets");

            actor.counters_db.hdel(&table, &lookup).unwrap();
            actor.counters_db.del(&counters_key).unwrap();
        }
    }

    #[tokio::test]
    async fn test_redis_name_map_error_does_not_return_previous_oid() {
        let (mut actor, table) = redis_actor("map_error");
        actor.counters_db.hset(&table, "object", &CxxString::from("oid:0x1")).unwrap();
        assert_eq!(actor.get_oid_from_name_map(&table, "object").await.unwrap(), "oid:0x1");
        // A unique table permits a real WRONGTYPE error without touching shared maps.
        actor.counters_db.set(&table, &CxxString::from("not a hash")).unwrap();
        assert!(actor.get_oid_from_name_map(&table, "object").await.unwrap_err().contains("Failed to hget"));
        actor.counters_db.del(&table).unwrap();
        assert!(actor.get_oid_from_name_map(&table, "object").await.is_err());
    }

    #[test]
    fn test_counter_key_creation() {
        let key = CounterKey::new("Ethernet0".into(), 1, 0);
        assert_eq!(key.object_name.as_ref(), "Ethernet0");
        assert_eq!(key.type_id, 1);
        assert_eq!(key.stat_id, 0);
    }

    #[test]
    fn test_counter_value_update() {
        let mut value = CounterValue::new(100);
        assert_eq!(value.counter, 100);
        assert!(value.updated);
        assert!(value.has_changed());

        value.mark_written("oid:0x1");
        assert!(!value.updated);
        assert!(!value.received);
        assert!(!value.has_changed());
        assert_eq!(value.last_written_value, Some(100));
        assert_eq!(value.last_written_oid.as_deref(), Some("oid:0x1"));

        // Same value - should not mark as updated
        value.update(100);
        assert_eq!(value.counter, 100);
        assert!(value.received);
        assert!(!value.updated);
        assert!(!value.has_changed());

        // Different value - should mark as updated
        value.update(200);
        assert_eq!(value.counter, 200);
        assert!(value.updated);
        assert!(value.has_changed());
    }

    #[test]
    fn test_config_default() {
        let config = CounterDBConfig::default();
        assert_eq!(config.interval, Duration::from_secs(10));
    }

    #[test]
    fn test_get_counter_name_map_table() {
        // Create a test actor instance to test the real method
        let (_tx, rx) = mpsc::channel::<SAIStatsBatchMessage>(1);
        let config = CounterDBConfig::default();

        // Test with a real actor instance
        match CounterDBActor::new(rx, config) {
            Ok(actor) => {
                // Test the real method that uses string concatenation
                assert_eq!(
                    actor.get_counter_name_map_table(&SaiObjectType::Port),
                    Ok("COUNTERS_PORT_NAME_MAP".to_string())
                );
                assert_eq!(
                    actor.get_counter_name_map_table(&SaiObjectType::Queue),
                    Ok("COUNTERS_QUEUE_NAME_MAP".to_string())
                );
                assert_eq!(
                    actor.get_counter_name_map_table(&SaiObjectType::BufferPool),
                    Ok("COUNTERS_BUFFER_POOL_NAME_MAP".to_string())
                );
                assert_eq!(
                    actor.get_counter_name_map_table(&SaiObjectType::IngressPriorityGroup),
                    Ok("COUNTERS_PG_NAME_MAP".to_string())
                );
            }
            Err(_) => {
                // Fallback for environments without Redis - test passes
            }
        }
    }

    #[test]
    fn test_get_stat_name() {
        // Create a test actor instance to test the real method
        let (_tx, rx) = mpsc::channel::<SAIStatsBatchMessage>(1);
        let config = CounterDBConfig::default();

        match CounterDBActor::new(rx, config) {
            Ok(actor) => {
                // Test Port stats
                assert_eq!(
                    actor.get_stat_name(0, &SaiObjectType::Port),
                    Ok("SAI_PORT_STAT_IF_IN_OCTETS".to_string())
                );
                assert_eq!(
                    actor.get_stat_name(1, &SaiObjectType::Port),
                    Ok("SAI_PORT_STAT_IF_IN_UCAST_PKTS".to_string())
                );

                // Test Queue stats
                assert_eq!(
                    actor.get_stat_name(0, &SaiObjectType::Queue),
                    Ok("SAI_QUEUE_STAT_PACKETS".to_string())
                );
                assert_eq!(
                    actor.get_stat_name(1, &SaiObjectType::Queue),
                    Ok("SAI_QUEUE_STAT_BYTES".to_string())
                );

                // Test BufferPool stats
                assert_eq!(
                    actor.get_stat_name(0, &SaiObjectType::BufferPool),
                    Ok("SAI_BUFFER_POOL_STAT_CURR_OCCUPANCY_BYTES".to_string())
                );
                assert_eq!(
                    actor.get_stat_name(1, &SaiObjectType::BufferPool),
                    Ok("SAI_BUFFER_POOL_STAT_WATERMARK_BYTES".to_string())
                );

                // Test IngressPriorityGroup stats
                assert_eq!(
                    actor.get_stat_name(0, &SaiObjectType::IngressPriorityGroup),
                    Ok("SAI_INGRESS_PRIORITY_GROUP_STAT_PACKETS".to_string())
                );
                assert_eq!(
                    actor.get_stat_name(1, &SaiObjectType::IngressPriorityGroup),
                    Ok("SAI_INGRESS_PRIORITY_GROUP_STAT_BYTES".to_string())
                );

                // Test invalid stat ID
                assert!(actor
                    .get_stat_name(0xFFFFFFFF, &SaiObjectType::Port)
                    .is_err());
                assert!(actor
                    .get_stat_name(0xFFFFFFFF, &SaiObjectType::Queue)
                    .is_err());
            }
            Err(_) => {
                // Fallback for environments without Redis - test passes
            }
        }
    }

    #[test]
    fn test_convert_object_name_for_lookup() {
        // Create a test actor instance to test the real method
        let (_tx, rx) = mpsc::channel::<SAIStatsBatchMessage>(1);
        let config = CounterDBConfig::default();

        match CounterDBActor::new(rx, config) {
            Ok(actor) => {
                // Test the real conversion logic
                assert_eq!(
                    actor.convert_object_name_for_lookup("Ethernet0"),
                    "Ethernet0"
                );
                assert_eq!(
                    actor.convert_object_name_for_lookup("Ethernet0|Queue1"),
                    "Ethernet0:Queue1"
                );
                assert_eq!(
                    actor.convert_object_name_for_lookup("Port|Lane0|Buffer1"),
                    "Port|Lane0:Buffer1"
                );
            }
            Err(_) => {
                // Fallback for environments without Redis - test passes
            }
        }
    }

    #[tokio::test]
    async fn test_counter_db_actor_integration() {
        // This test uses real Redis connection
        let (_tx, rx) = mpsc::channel::<SAIStatsBatchMessage>(10);
        let config = CounterDBConfig::default();

        // Try to create a real CounterDBActor
        match CounterDBActor::new(rx, config) {
            Ok(mut actor) => {
                // Create a test SAI stats message
                let stats = vec![SAIStat {
                    object_name: "Ethernet0".into(),
                    type_id: SaiObjectType::Port.to_u32(),
                    stat_id: 0, // IF_IN_OCTETS
                    counter: 1000,
                }];

                let sai_stats = SAIStats::new(12345, stats);
                let msg = Arc::new(SAIStatsBatch::from_stats(sai_stats));

                // Test message handling
                actor.handle_stats_message(msg.clone()).await;
                assert_eq!(actor.total_messages_received, 1);
                assert_eq!(actor.counter_cache.len(), 1);

                // Verify the counter is marked as changed
                let key = CounterKey::new("Ethernet0".into(), SaiObjectType::Port.to_u32(), 0);
                let cached_value = actor.counter_cache.get(&key).unwrap();
                assert!(cached_value.has_changed());
                assert_eq!(cached_value.counter, 1000);

                // Send the same message again - should not be marked as changed
                actor.handle_stats_message(msg.clone()).await;
                assert_eq!(actor.total_messages_received, 2);
                let cached_value = actor.counter_cache.get(&key).unwrap();
                // The value hasn't been written yet, so it should still be considered changed for the first write
                // But this specific counter didn't change from the previous value, so updated should still be true from first time
                assert!(cached_value.updated); // Still true from first time
                assert!(cached_value.has_changed()); // Still needs to be written

                // Simulate writing to database by marking as written
                if let Some(cached_value) = actor.counter_cache.get_mut(&key) {
                    cached_value.mark_written("oid:0x1");
                }

                // Now send the same message again - should not be marked as changed
                actor.handle_stats_message(msg.clone()).await;
                assert_eq!(actor.total_messages_received, 3);
                let cached_value = actor.counter_cache.get(&key).unwrap();
                assert!(!cached_value.updated); // Should be false after mark_written
                assert!(!cached_value.has_changed()); // No change needed

                // Send a different value
                let stats2 = vec![SAIStat {
                    object_name: "Ethernet0".into(),
                    type_id: SaiObjectType::Port.to_u32(),
                    stat_id: 0,
                    counter: 2000, // Changed value
                }];
                let sai_stats2 = SAIStats::new(12346, stats2);
                let mut batch2 = SAIStatsBatch::from_stats(sai_stats2);
                batch2.push_record(
                    12347,
                    [SAIStat::new(
                        "Ethernet0",
                        SaiObjectType::Port.to_u32(),
                        0,
                        3000,
                    )],
                );
                let msg2 = Arc::new(batch2);

                actor.handle_stats_message(msg2).await;
                assert_eq!(actor.total_messages_received, 5);
                let cached_value = actor.counter_cache.get(&key).unwrap();
                assert!(cached_value.has_changed()); // Value changed
                assert_eq!(cached_value.counter, 3000);
                let mut shared=SAIStatsBatch::default();
                shared.push_shared_record(12348,Arc::from(vec![crate::message::saistats::SAIStatMetadata::new("Ethernet0",1,0)]),[4000]);
                actor.handle_stats_message(Arc::new(shared)).await;
                assert_eq!(actor.total_messages_received,6);
                assert_eq!(actor.counter_cache[&key].counter,4000);
            }
            Err(e) => {
                // This is acceptable in CI environments where Redis might not be running
                let _ = e; // Suppress unused variable warning
            }
        }
    }

    #[tokio::test]
    async fn test_write_counter_uses_hset() {
        let (mut actor, prefix) = redis_actor("hset");
        let oid = format!("oid:{prefix}");
        let counters_key = format!("COUNTERS:{oid}");
        actor.counters_db.hset(&counters_key, "existing", &CxxString::from("preserved")).unwrap();
        let key = CounterKey::new(prefix.into(), SaiObjectType::Port.to_u32(), 0);
        actor.write_counter_to_db(&key, &CounterValue::new(1000), &oid).await.unwrap();
        assert_eq!(actor.counters_db.hget(&counters_key, "existing").unwrap().unwrap().to_string_lossy(), "preserved");
        assert_eq!(actor.counters_db.hget(&counters_key, "SAI_PORT_STAT_IF_IN_OCTETS").unwrap().unwrap().to_string_lossy(), "1000");
        actor.counters_db.del(&counters_key).unwrap();
    }

    #[tokio::test]
    async fn test_write_counter_redis_key_format() {
        let (mut actor, prefix) = redis_actor("key_format");
        let oid = format!("oid:{prefix}");
        let counters_key = format!("COUNTERS:{oid}");
        let key = CounterKey::new(prefix.into(), SaiObjectType::Port.to_u32(), 0);
        actor.write_counter_to_db(&key, &CounterValue::new(1000), &oid).await.unwrap();
        assert_eq!(actor.counters_db.hget(&counters_key, "SAI_PORT_STAT_IF_IN_OCTETS").unwrap().unwrap().to_string_lossy(), "1000");
        assert!(!actor.counters_db.exists(&format!("COUNTERS:{counters_key}")).unwrap());
        actor.counters_db.del(&counters_key).unwrap();
    }
}
