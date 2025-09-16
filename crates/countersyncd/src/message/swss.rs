#[allow(dead_code)]
pub struct SwssCfgStreamTelemetry {}

#[allow(dead_code)]
pub struct SwssCfgTelemetryGroup {}

#[allow(dead_code)]
pub enum SessionStatus {
    Enabled,
    Disabled,
}

#[allow(dead_code)]
pub enum SessionType {
    Ipfix,
}

#[allow(dead_code)]
pub struct SwssStateTelemetrySession {
    session_status: SessionStatus,
    session_type: SessionType,
    session_template: [u8],
}
