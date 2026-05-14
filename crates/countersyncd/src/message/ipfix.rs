use std::sync::Arc;

pub type IPFixTemplates = Arc<Vec<u8>>;

#[derive(Debug, Clone)]
pub struct IPFixTemplatesMessage {
    pub key: String,
    pub templates: Option<IPFixTemplates>,
    pub object_names: Option<Vec<String>>,
    pub is_delete: bool,
}

impl IPFixTemplatesMessage {
    pub fn new(key: String, templates: IPFixTemplates, object_names: Option<Vec<String>>) -> Self {
        Self {
            key,
            templates: Some(templates),
            object_names,
            is_delete: false,
        }
    }

    pub fn delete(key: String) -> Self {
        Self {
            key,
            templates: None,
            object_names: None,
            is_delete: true,
        }
    }
}
