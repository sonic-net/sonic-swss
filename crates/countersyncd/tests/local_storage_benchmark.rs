#![cfg(all(feature = "local-storage-benchmark", not(target_arch = "arm")))]
#![recursion_limit = "512"]

// The custom bench has no libtest harness; compile its checks here as real tests.
#[allow(dead_code, unused_attributes)]
#[path = "../benches/local_storage_perf/implementation.rs"]
mod benchmark;
