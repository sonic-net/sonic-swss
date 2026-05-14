//! Process exit codes for countersyncd

/// Successful completion
pub const EXIT_SUCCESS: i32 = 0;

/// General failure
pub const EXIT_FAILURE: i32 = 1;

/// Actor failure 
//  OpenTelemetry export retries exhausted
pub const EXIT_OTEL_EXPORT_RETRIES_EXHAUSTED: i32 = 125;
