use std::sync::Arc;

use super::harmonizer::HarmonizerConfig;

pub type IPFixTemplates = Arc<Vec<u8>>;

#[derive(Debug, Clone)]
pub struct IPFixTemplatesMessage {
    pub key: String,
    pub templates: Option<IPFixTemplates>,
    pub object_names: Option<Vec<String>>,
    pub object_ids: Option<Vec<u16>>,
    pub harmonizer_config: Option<HarmonizerConfig>,
    pub is_delete: bool,
}

impl IPFixTemplatesMessage {
    pub fn new(
        key: String,
        templates: IPFixTemplates,
        object_names: Option<Vec<String>>,
        object_ids: Option<Vec<u16>>,
        harmonizer_config: Option<HarmonizerConfig>,
    ) -> Self {
        Self {
            key,
            templates: Some(templates),
            object_names,
            object_ids,
            harmonizer_config,
            is_delete: false,
        }
    }

    pub fn delete(key: String) -> Self {
        Self {
            key,
            templates: None,
            object_names: None,
            object_ids: None,
            harmonizer_config: None,
            is_delete: true,
        }
    }
}
