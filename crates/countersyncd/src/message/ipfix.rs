use std::sync::Arc;

pub type IPFixTemplates = Arc<Vec<u8>>;

pub(crate) const MAX_TEMPLATE_CONFIG_BYTES: usize = 4 * 1024 * 1024;
pub(crate) const MAX_OBJECT_METADATA_BYTES: usize = 4 * 1024 * 1024;
pub(crate) const MAX_OBJECTS_PER_UPDATE: usize = 32_767;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IPFixTemplateOperation {
    Update,
    Deactivate,
    Delete,
}

#[derive(Debug, Clone)]
pub struct IPFixOwnerUpdate {
    pub key: String,
    pub templates: Option<IPFixTemplates>,
    pub object_names: Option<Vec<String>>,
    pub object_ids: Option<Vec<u16>>,
    pub operation: IPFixTemplateOperation,
}

impl IPFixOwnerUpdate {
    pub fn new(
        key: String,
        templates: IPFixTemplates,
        object_names: Option<Vec<String>>,
        object_ids: Option<Vec<u16>>,
    ) -> Self {
        Self {
            key,
            templates: Some(templates),
            object_names,
            object_ids,
            operation: IPFixTemplateOperation::Update,
        }
    }

    pub fn delete(key: String) -> Self {
        Self {
            key,
            templates: None,
            object_names: None,
            object_ids: None,
            operation: IPFixTemplateOperation::Delete,
        }
    }

    pub fn deactivate(key: String) -> Self {
        Self {
            key,
            templates: None,
            object_names: None,
            object_ids: None,
            operation: IPFixTemplateOperation::Deactivate,
        }
    }
}

/// A single owner change or a complete owner snapshot after notification loss.
/// Reconciliation requires a snapshot and cannot contain another envelope.
///
/// ```compile_fail
/// use countersyncd::message::ipfix::IPFixTemplatesMessage;
/// let message = IPFixTemplatesMessage::Reconcile(None);
/// ```
///
/// ```compile_fail
/// use countersyncd::message::ipfix::IPFixTemplatesMessage;
/// let message = IPFixTemplatesMessage::Reconcile(vec![
///     IPFixTemplatesMessage::Reconcile(vec![]),
/// ]);
/// ```
#[derive(Debug, Clone)]
pub enum IPFixTemplatesMessage {
    Owner(IPFixOwnerUpdate),
    Reconcile(Vec<IPFixOwnerUpdate>),
}
