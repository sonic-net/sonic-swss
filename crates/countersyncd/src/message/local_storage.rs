use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};

#[derive(Debug, Clone, Default)]
pub struct LocalStorageStatus {
    failed: Arc<AtomicBool>,
    shutdown: Arc<AtomicBool>,
}

impl LocalStorageStatus {
    pub fn mark_failed(&self) {
        self.failed.store(true, Ordering::Release);
    }

    pub fn failed(&self) -> bool {
        self.failed.load(Ordering::Acquire)
    }

    pub fn request_shutdown(&self) {
        self.shutdown.store(true, Ordering::Release);
    }

    pub fn shutdown_requested(&self) -> bool {
        self.shutdown.load(Ordering::Acquire)
    }
}
