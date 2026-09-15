//! Disk-backed local-storage benchmark; unavailable on armhf (32-bit ARM).

#![recursion_limit = "512"]

#[cfg(not(target_arch = "arm"))]
#[path = "local_storage_perf/implementation.rs"]
mod implementation;

#[cfg(not(target_arch = "arm"))]
fn main() {
    implementation::main();
}

#[cfg(target_arch = "arm")]
fn main() {
    eprintln!(
        "Skipping local-storage benchmark: local storage is not available on 32-bit ARM (armhf)."
    );
}
