#![cfg(feature = "local-storage-benchmark")]
#![recursion_limit = "512"]

// The custom bench has no libtest harness; compile its checks here as real tests.
#[allow(dead_code, unused_attributes)]
#[path = "../benches/local_storage_perf.rs"]
mod benchmark;
