use super::aggregator::AggregatorConfigMessage;
use std::sync::Arc;

pub type IPFixTemplates = Arc<Vec<u8>>;

#[derive(Debug, Clone)]
pub struct IPFixTemplatesMessage {
    pub key: String,
    pub templates: Option<IPFixTemplates>,
    pub object_names: Option<Vec<String>>,
    pub object_ids: Option<Vec<u16>>,
    pub is_delete: bool,
    pub aggregator_config: Option<AggregatorConfigMessage>,
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
            is_delete: false,
            aggregator_config: None,
        }
    }

    pub fn with_aggregator_config(mut self, config: AggregatorConfigMessage) -> Self {
        self.aggregator_config = Some(config);
        self
    }

    pub fn config(config: AggregatorConfigMessage) -> Self {
        Self {
            key: config.key.clone(),
            templates: None,
            object_names: None,
            object_ids: None,
            is_delete: false,
            aggregator_config: Some(config),
        }
    }

    pub fn delete(key: String) -> Self {
        Self {
            key,
            templates: None,
            object_names: None,
            object_ids: None,
            is_delete: true,
            aggregator_config: None,
        }
    }

    pub fn delete_with_aggregator_config(key: String, config: AggregatorConfigMessage) -> Self {
        Self::delete(key).with_aggregator_config(config)
    }
}
