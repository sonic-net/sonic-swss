use std::sync::Arc;

pub type IPFixTemplates = Arc<Vec<u8>>;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IPFixTemplateOperation {
    Update,
    Deactivate,
    Delete,
    Reconcile,
}

#[derive(Debug, Clone)]
pub struct IPFixTemplatesMessage {
    pub key: String,
    pub templates: Option<IPFixTemplates>,
    pub object_names: Option<Vec<String>>,
    pub object_ids: Option<Vec<u16>>,
    pub operation: IPFixTemplateOperation,
    /// Complete owner snapshot after Redis notification loss; never nested.
    pub reconciliation: Option<Vec<IPFixTemplatesMessage>>,
}

impl IPFixTemplatesMessage {
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
            reconciliation: None,
        }
    }

    pub fn delete(key: String) -> Self {
        Self {
            key,
            templates: None,
            object_names: None,
            object_ids: None,
            operation: IPFixTemplateOperation::Delete,
            reconciliation: None,
        }
    }

    pub fn deactivate(key: String) -> Self {
        Self {
            key,
            templates: None,
            object_names: None,
            object_ids: None,
            operation: IPFixTemplateOperation::Deactivate,
            reconciliation: None,
        }
    }

    pub fn reconcile(snapshots: Vec<Self>) -> Self {
        Self {
            key: String::new(),
            templates: None,
            object_names: None,
            object_ids: None,
            operation: IPFixTemplateOperation::Reconcile,
            reconciliation: Some(snapshots),
        }
    }
}
